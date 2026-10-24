#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Full index diagnostics remain coherent while mining, and support status/abort."""

from concurrent.futures import ThreadPoolExecutor
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, get_rpc_proxy


class RecycleIndexCheckTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle", "-recyclemode=index", "-rpcthreads=8"], ["-recyclemode=index"]]

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        node, disabled = self.nodes
        assert_equal(node.checkrecycleindex("status"), None)
        assert_equal(node.checkrecycleindex("abort"), False)
        assert_raises_rpc_error(-8, "Invalid action", node.checkrecycleindex, "repair")
        assert_raises_rpc_error(-1, "Recycling is disabled", disabled.checkrecycleindex, "start")
        assert_equal(node.checkrecycleindex("status"), None)
        empty = node.checkrecycleindex("start")
        assert empty["success"] and empty["valid"]
        assert_equal(empty["indexed_coins"], 0)
        assert_equal(node.cli.checkrecycleindex("start")["valid"], True)
        assert_equal(node.cli("-named").checkrecycleindex(action="start")["valid"], True)

        # Enough live outputs to observe a scan without sleeps or test-only RPCs.
        coinbase = create_coinbase(1)
        coinbase.vout = [CTxOut(1, b"\x51") for _ in range(80_000)]
        tip = node.getblockheader(node.getbestblockhash())
        block = create_block(int(tip["hash"], 16), coinbase, max(int(time.time()), tip["time"] + 1))
        block.solve()
        assert_equal(node.submitblock(block.serialize().hex()), None)
        original_tip = node.getbestblockhash()

        def wait_scanning(future):
            def scanning():
                status = node.checkrecycleindex("status")
                assert not future.done(), "Fixture scanned before its concurrent behavior could be exercised"
                return status is not None and status["phase"] in ("coins", "index")
            self.wait_until(scanning)

        with ThreadPoolExecutor(max_workers=1) as executor:
            scan_rpc = get_rpc_proxy(node.url, node.index, timeout=60)
            future = executor.submit(scan_rpc.checkrecycleindex, "start")
            wait_scanning(future)
            assert_raises_rpc_error(-8, "already in progress", node.checkrecycleindex, "start")
            self.generatetodescriptor(node, 1, "raw(6a)", sync_fun=lambda: None)
            checked = future.result(timeout=60)
            assert checked["success"] and checked["valid"]
            assert_equal(checked["height"], 1)
            assert_equal(checked["bestblock"], original_tip)
            assert_equal(checked["indexed_coins"], 80_000)
            assert_equal(checked["index_entries"], 80_000)
            assert_equal(checked["issues"], [])
            assert_equal(node.checkrecycleindex("status"), None)

            future = executor.submit(scan_rpc.checkrecycleindex, "start")
            wait_scanning(future)
            assert_equal(node.checkrecycleindex("abort"), True)
            interrupted = future.result(timeout=60)
            assert_equal(interrupted["success"], False)
            assert "valid" not in interrupted
            assert not (node.chain_path / "recycle-corrupt").exists()

        # Abort must release both the RPC reservation and database snapshot.
        checked = node.checkrecycleindex("start")
        assert checked["success"] and checked["valid"]
        assert_equal(checked["bestblock"], node.getbestblockhash())
        self.restart_node(0)
        assert node.checkrecycleindex("start")["valid"]
        node.invalidateblock(original_tip)
        assert_equal(node.checkrecycleindex("start")["indexed_coins"], 0)
        node.reconsiderblock(original_tip)
        assert node.checkrecycleindex("start")["valid"]


if __name__ == "__main__":
    RecycleIndexCheckTest(__file__).main()
