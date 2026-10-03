#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Expired inputs release independent inputs of unconfirmed transactions and descendants."""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class RecycleConflictsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("conflicts")
        wallet = node.get_wallet_rpc("conflicts")
        address = wallet.getnewaddress()

        def coin():
            block = self.generatetoaddress(node, 1, address)[0]
            tx = node.getblock(block, 2)["tx"][0]
            return {"txid": tx["txid"], "vout": next(i for i, out in enumerate(tx["vout"]) if out["value"] > 0)}

        expiring = coin()  # expires at height 201
        self.generatetodescriptor(node, 49, "raw(6a)")
        independent = [coin(), coin()]  # expire at heights 251 and 252
        self.generatetodescriptor(node, 148, "raw(6a)")
        assert_equal(node.getblockcount(), 200)

        def spend(inputs, amount):
            raw = wallet.createrawtransaction(inputs, {address: amount})
            signed = wallet.signrawtransactionwithwallet(raw)
            assert signed["complete"]
            return node.sendrawtransaction(signed["hex"])

        parent = spend([expiring, independent[0]], Decimal("99.999"))
        child = spend([{"txid": parent, "vout": 0}, independent[1]], Decimal("149.998"))
        assert_equal(wallet.getbalance(), Decimal("149.998"))

        def available():
            return {(out["txid"], out["vout"]) for out in wallet.listunspent()}

        def check_expired():
            assert_equal(node.getrawmempool(), [])
            assert_equal(node.gettxout(expiring["txid"], expiring["vout"]), None)
            assert_equal(wallet.getbalance(), Decimal("100"))
            for out in independent:
                assert_equal(node.gettxout(out["txid"], out["vout"])["value"], Decimal("50"))
                assert (out["txid"], out["vout"]) in available()
            # Deriving availability must not persist an abandonment decision.
            for txid in [parent, child]:
                tx = wallet.gettransaction(txid)
                assert_equal(tx["confirmations"], 0)
                assert all(not entry.get("abandoned", False) for entry in tx["details"])
            raw = wallet.createrawtransaction([], {wallet.getnewaddress(): Decimal("1")})
            funded = wallet.decoderawtransaction(wallet.fundrawtransaction(raw)["hex"])
            assert all((vin["txid"], vin["vout"]) in available() for vin in funded["vin"])

        def check_restored():
            assert node.gettxout(expiring["txid"], expiring["vout"], False) is not None
            # The live pending graph reserves B and C again, even before resubmission.
            assert all((out["txid"], out["vout"]) not in available() for out in independent)

        expiry = self.generateblock(node, "raw(6a)", [])["hash"]
        check_expired()
        wallet.unloadwallet()
        node.loadwallet("conflicts")
        wallet = node.get_wallet_rpc("conflicts")
        check_expired()
        self.restart_node(0)
        node.loadwallet("conflicts")
        wallet = node.get_wallet_rpc("conflicts")
        check_expired()
        wallet.rescanblockchain()
        check_expired()

        node.invalidateblock(expiry)
        check_restored()
        # Missing the expiry notification while unloaded must give the same result.
        wallet.unloadwallet()
        node.reconsiderblock(expiry)
        node.loadwallet("conflicts")
        wallet = node.get_wallet_rpc("conflicts")
        check_expired()
        node.invalidateblock(expiry)
        check_restored()
        self.restart_node(0)
        node.loadwallet("conflicts")
        wallet = node.get_wallet_rpc("conflicts")
        self.wait_until(lambda: set(node.getrawmempool()) == {parent, child})
        check_restored()
        self.generateblock(node, "raw(6a)", [parent, child])
        assert_equal(wallet.getbalance(), Decimal("149.998"))
        # Confirmed spends stay spent when their original inputs reach expiry.
        self.generatetodescriptor(node, 51, "raw(6a)")
        assert_equal(node.getblockcount(), 252)
        assert_equal(wallet.getbalance(), Decimal("149.998"))
        assert all((out["txid"], out["vout"]) not in available() for out in independent)
        assert node.verifychain(4, 0)


if __name__ == "__main__":
    RecycleConflictsTest(__file__).main()
