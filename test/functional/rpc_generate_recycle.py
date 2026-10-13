#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Check final-list rewards and GBT transaction changes at expiry boundaries."""

from decimal import Decimal

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import CBlock, CTransaction, from_hex
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class GenerateRecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle", f"-recyclemode={self.options.recycle_mode}"]]

    def add_options(self, parser):
        parser.add_argument("--recycle-mode", choices=("index", "block"), default="index")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.add_p2p_connection(P2PInterface())
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

        def gbt_change(raw_tx, template_reward, changed_reward, stale_reward=None):
            template = node.getblocktemplate({"rules": ["segwit"]})
            assert_equal(template["transactions"], [])
            assert_equal(template["coinbasevalue"], template_reward)
            assert "transactions" in template["mutable"]
            coinbase = create_coinbase(template["height"])
            coinbase.vout[0].nValue = template_reward
            block = create_block(int(template["previousblockhash"], 16), coinbase, template["curtime"])
            block.nVersion = template["version"]
            block.nBits = int(template["bits"], 16)

            def check(expected):
                block.hashMerkleRoot = block.calc_merkle_root()
                assert_equal(node.getblocktemplate({"mode": "proposal", "data": block.serialize().hex(), "rules": ["segwit"]}), expected)

            check(None)
            block.vtx.append(from_hex(CTransaction(), raw_tx))
            add_witness_commitment(block)
            # Ordinary fees alone cannot compensate for a reduced recycle reward.
            if stale_reward is not None:
                block.vtx[0].vout[0].nValue = stale_reward
                check("bad-cb-amount")
            block.vtx[0].vout[0].nValue = changed_reward
            check(None)
            block.vtx[0].vout[0].nValue += 1
            check("bad-cb-amount")

        # Before expiry, a supplied raw transaction is valid and fees stay unclaimed.
        proposal([signed], 25_0000_0000)
        gbt_change(signed, 25_0000_0000, 25_0000_0000 + 100_000)
        self.generateblock(node, "raw(6a)", [])
        assert_equal(node.getblockcount(), 200)
        proposal([], 26_0000_0000)
        proposal([signed], 25_0000_0000)
        gbt_change(signed, 26_0000_0000, 25_0000_0000 + 100_000, 26_0000_0000 + 100_000)
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
        gbt_change(signed_b, 26_0000_0000, 26_0000_0000 + 100_000)
        self.generateblock(node, "raw(6a)", [signed_b])
        assert node.verifychain(4, 0)


if __name__ == "__main__":
    GenerateRecycleTest(__file__).main()
