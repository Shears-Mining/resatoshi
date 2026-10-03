#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""generateblock computes recycle rewards from the final transaction list."""

from decimal import Decimal

from test_framework.messages import CBlock, from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class GenerateRecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("generate")
        wallet = node.get_wallet_rpc("generate")
        address = wallet.getnewaddress()
        hashes = self.generatetoaddress(node, 2, address)
        inputs = []
        for h in hashes:
            tx = node.getblock(h, 2)["tx"][0]
            inputs.append({"txid": tx["txid"], "vout": next(i for i, out in enumerate(tx["vout"]) if out["value"] > 0)})
        self.generatetodescriptor(node, 197, "raw(6a)")
        raw = wallet.createrawtransaction([inputs[0]], {address: Decimal("49.999")})
        signed = wallet.signrawtransactionwithwallet(raw)["hex"]

        def proposal(txs, reward):
            result = self.generateblock(node, "raw(6a)", txs, False)
            block = from_hex(CBlock(), result["hex"])
            assert_equal(sum(out.nValue for out in block.vtx[0].vout), reward)
            assert_equal(node.getblocktemplate({"mode": "proposal", "data": result["hex"], "rules": ["segwit"]}), None)
            return result

        # Before expiry, a supplied raw transaction is valid and fees stay unclaimed.
        proposal([signed], 25_0000_0000)
        self.generateblock(node, "raw(6a)", [])
        assert_equal(node.getblockcount(), 200)
        proposal([], 26_0000_0000)
        proposal([signed], 25_0000_0000)
        txid = node.sendrawtransaction(signed)
        proposal([txid], 25_0000_0000)
        mined = self.generateblock(node, "raw(6a)", [txid])["hash"]
        assert_equal(node.getblock(mined)["tx"][1], txid)
        assert_equal(node.getblockcount(), 201)
        node.invalidateblock(mined)
        # Excluding the spend fills the pool; a later expiry spend need not reduce
        # the one-coin reward if the existing pool can already pay it.
        self.generateblock(node, "raw(6a)", [])
        raw_b = wallet.createrawtransaction([inputs[1]], {address: Decimal("49.999")})
        signed_b = wallet.signrawtransactionwithwallet(raw_b)["hex"]
        proposal([signed_b], 26_0000_0000)
        self.generateblock(node, "raw(6a)", [signed_b])
        assert node.verifychain(4, 0)


if __name__ == "__main__":
    GenerateRecycleTest(__file__).main()
