#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check indexed expiry, crash recovery, restart, and explicit pool snapshots."""

import http.client
from pathlib import Path
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class RecycleIndexTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [['-test=recycle', '-recyclemode=index', '-coinstatsindex', '-checklevel=4', '-checkblocks=0'] for _ in range(2)]

    def add_options(self, parser):
        parser.add_argument('--crash-flush', action='store_true', help='Recover a deliberately interrupted flush')

    def stats(self, node):
        self.wait_until(lambda: node.getindexinfo()['coinstatsindex']['synced'])
        indexed = node.gettxoutsetinfo('muhash')
        scanned = node.gettxoutsetinfo('muhash', use_index=False)
        keys = ('bestblock', 'txouts', 'total_amount', 'muhash')
        assert_equal([indexed[k] for k in keys], [scanned[k] for k in keys])
        return [scanned[k] for k in keys] + [node.getblockchaininfo(True)['recycling']]

    def run_test(self):
        new, old = self.nodes
        self.log.info('Mine identical blocks on two independent nodes')
        tip = new.getblockheader(new.getbestblockhash())
        coinbase = create_coinbase(1)
        coinbase.vout = [CTxOut(100_000, b'\x51') for _ in range(300)]
        block = create_block(int(tip['hash'], 16), coinbase, max(int(time.time()), tip['time'] + 1))
        block.solve()
        assert_equal(new.submitblock(block.serialize().hex()), None)
        self.sync_blocks()
        self.generatetodescriptor(new, 100, 'raw(6a)')
        spend = CTransaction()
        spend.vin = [CTxIn(COutPoint(coinbase.txid_int, n)) for n in range(250)]
        spend.vout = [CTxOut(24_900_000, b'\x51')]
        tip = new.getblockheader(new.getbestblockhash())
        spent_block = create_block(int(tip['hash'], 16), create_coinbase(102), tip['time'] + 1, txlist=[spend])
        spent_block.solve()
        assert_equal(new.submitblock(spent_block.serialize().hex()), None)
        self.sync_blocks()
        self.generatetodescriptor(new, 98, 'raw(6a)')
        self.sync_blocks()
        assert_equal(self.stats(new), self.stats(old))
        before = self.stats(new)
        if self.options.crash_flush:
            self.restart_node(1, extra_args=self.extra_args[1] + ['-dbbatchsize=1', '-dbcrashratio=1'])
            self.connect_nodes(0, 1)
        self.generatetodescriptor(new, 1, 'raw(6a)')
        self.sync_blocks()
        expired_hash = new.getbestblockhash()
        assert_equal(new.getblockstats(expired_hash)['expired_outputs'], 50)
        after = self.stats(new)
        if self.options.crash_flush:
            with old.assert_debug_log(['Simulating a crash. Goodbye.']):
                try:
                    old.gettxoutsetinfo('muhash', use_index=False)
                except (http.client.RemoteDisconnected, ConnectionResetError):
                    pass
                old.wait_until_stopped(timeout=20)
        else:
            assert_equal(self.stats(old), after)

        self.log.info('Restart persisted state, then disconnect typed expiry undo')
        if old.running:
            self.stop_node(1)
        self.start_node(1)
        self.connect_nodes(0, 1)
        assert_equal(self.stats(old), after)
        assert old.verifychain(4, 0)
        for node in self.nodes:
            node.invalidateblock(expired_hash)
        assert_equal(self.stats(old), before)
        assert_equal(self.stats(new), before)
        for node in self.nodes:
            node.reconsiderblock(expired_hash)
        self.sync_blocks()
        assert_equal(self.stats(old), after)
        assert_equal(self.stats(new), after)

        self.log.info('Snapshots contain transaction outputs and a separate pool header')
        snapshot = old.dumptxoutset(str(Path(self.options.tmpdir) / 'indexed.dat'), 'latest')
        # Pool state never contributes a fake output to the snapshot count.
        assert_equal(snapshot['coins_written'], old.gettxoutsetinfo()['txouts'])
        self.restart_node(1, extra_args=self.extra_args[1] + ['-reindex-chainstate'])
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(self.stats(old), after)

        self.log.info('Former reserved outpoints follow ordinary missing-input policy')
        for n in (1_000_001, 1_005_000):
            tx = CTransaction()
            tx.vin = [CTxIn(COutPoint(0, n))]
            tx.vout = [CTxOut(1000, b'\x00\x14' + bytes(20))]
            result = new.testmempoolaccept([tx.serialize().hex()])[0]
            assert_equal(result['allowed'], False)
            assert_equal(result['reject-reason'], 'missing-inputs')



if __name__ == '__main__':
    RecycleIndexTest(__file__).main()
