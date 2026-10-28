#!/usr/bin/env python3
# Copyright (c) 2026 The Resatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Restore an old-version wallet into a separately synchronized new-version node."""

from decimal import Decimal
import json
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class RecycleWalletUpgradeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.wallet_names = [None, None]
        self.extra_args = [['-test=recycle', '-coinstatsindex', '-txindex', '-blockfilterindex'] for _ in range(2)]

    def add_options(self, parser):
        parser.add_argument('--reference-bitcoind')
        parser.add_argument('--candidate-mode', choices=('index', 'block'), default='index')
        parser.add_argument('--upgrade-results')

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.extra_args[0].append(f'-recyclemode={self.options.candidate_mode}')
        self.add_nodes(2, extra_args=self.extra_args)
        if self.options.reference_bitcoind:
            self.nodes[1].args[0] = str(Path(self.options.reference_bitcoind).resolve())
            self.nodes[1].replace_in_config([("prevoutfetchthreads=1\n", "")])
        self.start_nodes()
        self.connect_nodes(0, 1)
        self.sync_all()

    def run_test(self):
        candidate, reference = self.nodes
        reference.createwallet('owner')
        owner = reference.get_wallet_rpc('owner')
        address = owner.getnewaddress('', 'bech32m')
        self.generatetoaddress(reference, 1, address)
        self.generatetodescriptor(reference, 100, 'raw(6a)')
        assert_equal(owner.getbalance(), Decimal(50))
        backup = str(Path(self.options.tmpdir) / 'old-owner.bak')
        owner.backupwallet(backup)

        self.log.info('Restore the original wallet after P2P synchronization into the fresh new-format folder')
        candidate.restorewallet('restored', backup, True)
        restored = candidate.get_wallet_rpc('restored')
        assert restored.getaddressinfo(address)['ismine']
        assert_equal(restored.getbalance(), owner.getbalance())
        fields = ('txid', 'vout', 'amount')
        assert_equal([[c[k] for k in fields] for c in restored.listunspent()],
                     [[c[k] for k in fields] for c in owner.listunspent()])

        reference.createwallet('recipient')
        recipient = reference.get_wallet_rpc('recipient')
        payment = restored.sendtoaddress(recipient.getnewaddress(), Decimal(1), fee_rate=2)
        self.sync_mempools()
        assert payment in reference.getrawmempool()
        self.generatetodescriptor(reference, 1, 'raw(6a)')
        assert_equal(recipient.getbalance(), Decimal(1))
        assert_equal(restored.getbalance(), owner.getbalance())
        assert_equal(restored.gettransaction(payment)['confirmations'], 1)

        expected_balance = restored.getbalance()
        self.restart_node(0)
        self.connect_nodes(0, 1)
        self.sync_all()
        assert 'restored' in candidate.listwallets()
        assert_equal(candidate.get_wallet_rpc('restored').getbalance(), expected_balance)
        states = []
        for node in self.nodes:
            assert node.verifychain(4, 0)
            self.wait_until(lambda: all(v['synced'] for v in node.getindexinfo().values()))
            stats = node.gettxoutsetinfo('muhash', use_index=False)
            states.append([stats[k] for k in ('bestblock', 'txouts', 'total_amount', 'muhash')])
        assert_equal(*states)
        if self.options.upgrade_results:
            Path(self.options.upgrade_results).write_text(json.dumps({
                'status': 'passed', 'candidate_mode': self.options.candidate_mode,
                'reference_binary': self.options.reference_bitcoind,
                'height': candidate.getblockcount(), 'restored_wallet_balance': str(expected_balance),
                'backup_restore_rescan': True, 'payment_relay_and_confirmation': True,
                'wallet_reloaded_after_restart': True,
            }, indent=2) + '\n')


if __name__ == '__main__':
    RecycleWalletUpgradeTest(__file__).main()
