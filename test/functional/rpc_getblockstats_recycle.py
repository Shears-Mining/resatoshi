#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Compare block UTXO statistics with chainstate across expiry and reorgs."""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTxOut
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class GetblockstatsRecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle"]]

    def submit_outputs(self, count, script=None):
        node = self.nodes[0]
        tip = node.getblockheader(node.getbestblockhash())
        coinbase = create_coinbase(tip["height"] + 1)
        coinbase.vout = [CTxOut(0, CScript([OP_TRUE]) if script is None else script) for _ in range(count)]
        if count == 0:
            coinbase.vout = [CTxOut(0, CScript([OP_RETURN]))]
        block = create_block(int(tip["hash"], 16), coinbase, tip["time"] + 1)
        block.solve()
        assert_equal(node.submitblock(block.serialize().hex()), None)
        return block.hash_hex

    def check_delta(self, blockhash, expected, output_size, expired=0, unspendable=0, size_delta=None):
        node = self.nodes[0]
        stats = node.getblockstats(blockhash)
        assert_equal(stats["utxo_increase_actual"], expected)
        assert_equal(stats["utxo_size_inc_actual"], expected * output_size if size_delta is None else size_delta)
        assert_equal(stats["utxo_increase"], expected + expired + unspendable)
        assert_equal(stats["utxo_size_inc"], (expected + expired + unspendable) * output_size)
        assert_equal(stats["expired_outputs"], expired)
        for field in ("utxo_increase", "utxo_increase_actual", "utxo_size_inc", "utxo_size_inc_actual", "expired_outputs"):
            assert_equal(node.getblockstats(blockhash, [field]), {field: stats[field]})
        assert_equal(stats["ins"], 0)
        assert_equal(stats["totalfee"], 0)

    def run_test(self):
        node = self.nodes[0]
        first = self.submit_outputs(1)
        output_size = node.getblockstats(first)["utxo_size_inc_actual"]
        self.check_delta(first, 1, output_size)
        self.submit_outputs(10_000)
        self.submit_outputs(10_000)
        self.submit_outputs(20_000)
        # Cross the CompactSize script-length boundary (one byte to three).
        self.submit_outputs(1, CScript([OP_TRUE] * 253))
        self.generatetodescriptor(node, 195, "raw(6a)")

        cases = []
        for created, expired in ((1, 1), (10_000, 10_000), (1, 10_000), (0, 20_000)):
            before = node.gettxoutsetinfo()
            blockhash = self.submit_outputs(created)
            after = node.gettxoutsetinfo()
            delta = created - expired
            assert_equal(after["txouts"] - before["txouts"], delta)
            unspendable = int(created == 0)
            self.check_delta(blockhash, delta, output_size, expired, unspendable)
            assert_equal(node.getblockstats(blockhash)["outs"], created + unspendable)
            cases.append((blockhash, delta, expired, unspendable, None))

        before = node.gettxoutsetinfo()["txouts"]
        blockhash = self.submit_outputs(1)
        assert_equal(node.gettxoutsetinfo()["txouts"], before)
        # 252 fewer script bytes and two fewer CompactSize bytes.
        self.check_delta(blockhash, 0, output_size, 1, size_delta=-254)
        cases.append((blockhash, 0, 1, 0, -254))

        # The corresponding old block contains only an unspendable output.
        blockhash = self.submit_outputs(1)
        self.check_delta(blockhash, 1, output_size)
        cases.append((blockhash, 1, 0, 0, None))

        self.restart_node(0)
        for blockhash, delta, expired, unspendable, size_delta in cases:
            self.check_delta(blockhash, delta, output_size, expired, unspendable, size_delta)

        node.invalidateblock(cases[0][0])
        assert_equal(node.gettxoutsetinfo()["txouts"], 40_002)
        replacement = self.submit_outputs(2)
        assert_equal(node.gettxoutsetinfo()["txouts"], 40_003)
        self.check_delta(replacement, 1, output_size, 1)


if __name__ == '__main__':
    GetblockstatsRecycleTest(__file__).main()
