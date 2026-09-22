#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Expired outputs can be unlocked individually, but cannot be newly locked."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class RecycleLocksTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("locks")
        wallet = node.get_wallet_rpc("locks")
        blocks = self.generatetoaddress(node, 2, wallet.getnewaddress())
        outputs = []
        for block in blocks:
            tx = node.getblock(block, 2)["tx"][0]
            outputs.append({"txid": tx["txid"], "vout": next(i for i, out in enumerate(tx["vout"]) if out["value"] > 0)})
        self.generatetodescriptor(node, 198, "raw(6a)")
        assert_equal(wallet.lockunspent(False, outputs, True), True)
        expiry = self.generatetodescriptor(node, 1, "raw(6a)")[0]
        assert_equal(node.gettxout(outputs[0]["txid"], outputs[0]["vout"]), None)
        assert node.gettxout(outputs[1]["txid"], outputs[1]["vout"]) is not None

        self.restart_node(0)
        node.loadwallet("locks")
        wallet = node.get_wallet_rpc("locks")
        assert_equal(len(wallet.listlockunspent()), 2)
        assert_equal(wallet.lockunspent(True, [outputs[0]]), True)
        assert_equal(wallet.listlockunspent(), [outputs[1]])
        assert_raises_rpc_error(-8, "expected unspent output", wallet.lockunspent, False, [outputs[0]])
        assert_raises_rpc_error(-8, "expected locked output", wallet.lockunspent, True, [outputs[0]])

        # The expired lock was removed from disk, without disturbing the other lock.
        self.restart_node(0)
        node.loadwallet("locks")
        wallet = node.get_wallet_rpc("locks")
        assert_equal(wallet.listlockunspent(), [outputs[1]])
        node.invalidateblock(expiry)
        assert_equal(wallet.lockunspent(False, [outputs[0]]), True)
        assert_equal(wallet.lockunspent(True, [outputs[0]]), True)
        assert_equal(wallet.listlockunspent(), [outputs[1]])


if __name__ == "__main__":
    RecycleLocksTest(__file__).main()
