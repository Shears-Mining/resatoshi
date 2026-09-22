#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check explicit sendall inputs across expiry, restart and rollback."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class SendallRecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("expiry")
        wallet = node.get_wallet_rpc("expiry")
        address = wallet.getnewaddress()
        origin = self.generatetoaddress(node, 1, address)[0]
        coinbase = node.getblock(origin, 2)["tx"][0]
        vout = next(i for i, output in enumerate(coinbase["vout"]) if output["value"] > 0)
        prevout = {"txid": coinbase["txid"], "vout": vout}
        self.generatetodescriptor(node, 199, "raw(6a)")

        def check_available():
            result = wallet.sendall([address], inputs=[prevout], fee_rate=1, add_to_wallet=False)
            assert_equal(result["complete"], True)
            assert_equal(node.testmempoolaccept([result["hex"]])[0]["allowed"], True)

        def check_expired():
            assert_equal(node.gettxout(coinbase["txid"], vout), None)
            assert_equal(wallet.listunspent(), [])
            assert_equal(wallet.getbalance(), 0)
            transactions = wallet.listtransactions(count=100)
            for options in ({}, {"add_to_wallet": False}, {"psbt": True}, {"lock_unspents": True}):
                assert_raises_rpc_error(
                    -8, f"Input not available. UTXO ({coinbase['txid']}:{vout}) has expired.",
                    wallet.sendall, [address], inputs=[prevout], fee_rate=1, **options,
                )
            # A nonexistent output of an expired transaction is still "not found".
            assert_raises_rpc_error(
                -8, "is not part of wallet", wallet.sendall, [address], fee_rate=1,
                inputs=[{"txid": coinbase["txid"], "vout": len(coinbase["vout"])}],
            )
            assert_equal(wallet.listtransactions(count=100), transactions)
            assert_equal(wallet.listlockunspent(), [])
            assert_equal(node.getrawmempool(), [])

        self.log.info("Spending in the expiry block is still valid")
        check_available()
        expiry = self.generatetodescriptor(node, 1, "raw(6a)")[0]
        self.log.info("Expired inputs fail before signing, wallet insertion or locking")
        check_expired()

        self.restart_node(0)
        node.loadwallet("expiry")
        wallet = node.get_wallet_rpc("expiry")
        check_expired()

        self.log.info("Disconnecting the expiry block restores availability")
        node.invalidateblock(expiry)
        assert_equal(node.getblockcount(), 200)
        check_available()
        node.reconsiderblock(expiry)
        check_expired()


if __name__ == "__main__":
    SendallRecycleTest(__file__).main()
