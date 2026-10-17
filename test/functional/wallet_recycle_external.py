#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""External input evidence survives reload, rescans and changes of its confirming block."""
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class ExternalExpiryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.wallet_names = []
        self.extra_args = [["-test=recycle", "-blockfilterindex=basic", "-blockmintxfee=1"]]

    def add_options(self, parser):
        parser.add_argument("--reorg", action="store_true")
        parser.add_argument("--orphan", action="store_true")

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        n = self.nodes[0]
        for name in ["external", "owner"]:
            n.createwallet(name)
        external = n.get_wallet_rpc("external")
        owner = n.get_wallet_rpc("owner")

        def coin(wallet):
            block = self.generatetoaddress(n, 1, wallet.getnewaddress())[0]
            tx = n.getblock(block, 2)["tx"][0]
            return {"txid": tx["txid"], "vout": next(i for i, v in enumerate(tx["vout"]) if v["value"] > 0)}

        def signed(inputs, amount, signer, recipient):
            raw = owner.createrawtransaction(inputs, {recipient.getnewaddress(): amount})
            return signer.signrawtransactionwithwallet(raw)["hex"]

        def available(point):
            return any(x["txid"] == point["txid"] and x["vout"] == point["vout"] for x in owner.listunspent())

        a = coin(external)
        if self.options.reorg:
            b = coin(owner)
            self.generatetodescriptor(n, 98, "raw(6a)")
            a_raw = signed([a], Decimal("49.999"), external, external)
            a_txid = n.sendrawtransaction(a_raw)
            old_anchor = self.generateblock(n, "raw(6a)", [a_raw])["hash"]
            self.generatetodescriptor(n, 49, "raw(6a)")
            b_raw = signed([b], Decimal("49.999"), owner, owner)
            b_txid = n.sendrawtransaction(b_raw)
            self.generateblock(n, "raw(6a)", [b_raw])
            self.generatetodescriptor(n, 149, "raw(6a)")
            a = {"txid": a_txid, "vout": 0}
            b = {"txid": b_txid, "vout": 0}
            raw = signed([a, b], Decimal("99.997"), external, owner)
            joint = owner.signrawtransactionwithwallet(raw)["hex"]
            txid = n.sendrawtransaction(joint)
            owner.gettransaction(txid)
            owner.unloadwallet()
            # Reconfirm the SAME external transaction at a different height and hash.
            n.invalidateblock(old_anchor)
            assert_equal(n.getblockcount(), 100)
            self.generateblock(n, "raw(6a)", [])
            self.generateblock(n, "raw(6a)", [] if self.options.orphan else [a_raw])  # new expiry is 302, not 301
            self.generatetodescriptor(n, 48, "raw(6a)")
            self.generateblock(n, "raw(6a)", [b_raw])
            self.generatetodescriptor(n, 150, "raw(6a)")
            n.loadwallet("owner")
            owner = n.get_wallet_rpc("owner")
            assert_equal(n.getblockcount(), 301)
            assert not available(b)
            self.generateblock(n, "raw(6a)", [])
            if self.options.orphan:
                # The old anchor is off-chain and A is absent from the UTXO set.
                # Neither fact proves expiry, so B must remain reserved.
                assert_equal(owner.getbalance(), Decimal("0"))
                assert not available(b)
            else:
                assert_equal(owner.getbalance(), Decimal("49.999"))
                assert available(b)
        else:
            self.generatetodescriptor(n, 49, "raw(6a)")
            b, c = coin(owner), coin(owner)
            self.generatetodescriptor(n, 148, "raw(6a)")
            assert_raises_rpc_error(-5, "Invalid or non-wallet transaction id", owner.gettransaction, a["txid"])
            raw = signed([a, b], Decimal("99.999"), external, owner)
            parent = n.sendrawtransaction(owner.signrawtransactionwithwallet(raw)["hex"])
            child = n.sendrawtransaction(signed([{"txid": parent, "vout": 0}, c], Decimal("149.998"), owner, owner))
            owner.gettransaction(child)
            expiry = self.generateblock(n, "raw(6a)", [])["hash"]
            self.stop_node(0)
            self.start_node(0)
            n.loadwallet("owner")
            owner = n.get_wallet_rpc("owner")
            assert_equal(owner.getbalance(), Decimal("100"))
            owner.rescanblockchain()
            assert_equal(owner.getbalance(), Decimal("100"))
            assert available(b) and available(c)
            funded = owner.fundrawtransaction(owner.createrawtransaction([], {owner.getnewaddress(): Decimal("1")}))
            assert all(v["txid"] in {b["txid"], c["txid"]} for v in owner.decoderawtransaction(funded["hex"])["vin"])
            owner.unloadwallet()
            n.loadwallet("owner")
            owner = n.get_wallet_rpc("owner")
            assert_equal(owner.getbalance(), Decimal("100"))
            n.invalidateblock(expiry)
            assert not available(b) and not available(c)
            owner.unloadwallet()
            n.reconsiderblock(expiry)
            n.loadwallet("owner")
            owner = n.get_wallet_rpc("owner")
            assert_equal(owner.getbalance(), Decimal("100"))
            assert "recycle_input_origins" not in owner.gettransaction(parent)
        assert n.verifychain(4, 0)


if __name__ == "__main__":
    ExternalExpiryTest(__file__).main()
