#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Stress full-weight blocks, mass expiry, and old/new chainstate agreement."""

from copy import deepcopy
from decimal import Decimal
import http.client
import json
from pathlib import Path
import time

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, MAX_BLOCK_WEIGHT, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut
from test_framework.p2p import P2PDataStore
from test_framework.script import CScript, OP_DROP, OP_RETURN, OP_TRUE
from test_framework.script_util import script_to_p2wsh_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

PAY = CScript([OP_TRUE])
BURN = CScript([OP_RETURN])
WITNESS_SCRIPT = CScript([OP_DROP] * 200 + [OP_TRUE])


def spendable(output):
    return len(output.scriptPubKey) <= 10000 and output.scriptPubKey[0] != OP_RETURN


class RecycleFullBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [['-test=recycle', '-recyclemode=index', '-coinstatsindex', '-checklevel=4', '-checkblocks=0'] for _ in range(2)]

    def add_options(self, parser):
        parser.add_argument('--reference-bitcoind')
        parser.add_argument('--full-block-results')
        parser.add_argument('--small-cache', action='store_true')
        parser.add_argument('--crash-expiry', action='store_true')

    def setup_network(self):
        if self.options.small_cache:
            self.extra_args = [args + ['-dbcache=4', '-dbbatchsize=512', '-checklevel=2'] for args in self.extra_args]
        self.add_nodes(2, extra_args=self.extra_args)
        if self.options.reference_bitcoind:
            self.nodes[1].args[0] = str(Path(self.options.reference_bitcoind).resolve())
        self.start_nodes()

    def transition(self, txs):
        coins = self.coins.copy()
        fees = 0
        for tx in txs:
            value = 0
            for vin in tx.vin:
                amount, origin, coinbase = coins.pop((vin.prevout.hash, vin.prevout.n))
                assert not coinbase or self.height + 1 - origin >= 100
                value += amount
            fees += value - sum(o.nValue for o in tx.vout)
            assert fees >= 0
            txid = tx.txid_int
            for n, output in enumerate(tx.vout):
                if spendable(output):
                    coins[(txid, n)] = (output.nValue, self.height + 1, False)
        expired = [key for key, (_, origin, _) in coins.items() if self.height + 1 - origin == 200]
        available = self.pool + sum(coins[key][0] for key in expired)
        for key in expired:
            del coins[key]
        return coins, fees, available, len(expired)

    def candidate(self, txs=(), *, full=False):
        coinbase = create_coinbase(self.height + 1)
        _, fees, available, _ = self.transition(txs)
        coinbase.vout = [CTxOut(coinbase.vout[0].nValue + fees + min(COIN, available), BURN)]
        if full:
            coinbase.vout.append(CTxOut(0, CScript(bytes(BURN) + b'\0' * 1000)))
        block = create_block(int(self.tip, 16), coinbase, self.timestamp + self.height + 1, txlist=list(txs))
        if full:
            padding = (MAX_BLOCK_WEIGHT - block.get_weight()) // 4
            assert padding >= 0
            coinbase.vout[-1].scriptPubKey = CScript(bytes(coinbase.vout[-1].scriptPubKey) + b'\0' * padding)
            # A padding script that crosses CompactSize's uint16 boundary adds two bytes.
            excess = (block.get_weight() - MAX_BLOCK_WEIGHT) // 4
            if excess:
                coinbase.vout[-1].scriptPubKey = CScript(bytes(coinbase.vout[-1].scriptPubKey)[:-excess])
            assert_equal(block.get_weight(), MAX_BLOCK_WEIGHT)
        self.solve(block)
        return block

    @staticmethod
    def solve(block):
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()

    def accept(self, block, label=None):
        coins, fees, available, expired = self.transition(block.vtx[1:])
        claimed = max(0, sum(o.nValue for o in block.vtx[0].vout) - create_coinbase(self.height + 1).vout[0].nValue - fees)
        assert claimed <= min(COIN, available)
        raw = block.serialize().hex()
        timings = []
        for node in self.nodes:
            start = time.monotonic()
            if block.get_weight() == MAX_BLOCK_WEIGHT:
                peer = node.add_p2p_connection(P2PDataStore())
                peer.send_blocks_and_test([block], node)
                node.disconnect_p2ps()
            else:
                assert_equal(node.submitblock(raw), None)
            timings.append(round(time.monotonic() - start, 3))
            assert_equal(node.getblockstats(block.hash_hex)['expired_outputs'], expired)
            assert_equal(node.getbestblockhash(), block.hash_hex)
        self.height += 1
        self.pool, self.tip = available - claimed, block.hash_hex
        txid = block.vtx[0].txid_int
        for n, output in enumerate(block.vtx[0].vout):
            if spendable(output):
                coins[(txid, n)] = (output.nValue, self.height, True)
        self.coins = coins
        self.accepted += 1
        if label:
            result = dict(label=label, height=self.height, weight=block.get_weight(), bytes=len(raw) // 2,
                          transactions=len(block.vtx), outputs=sum(len(tx.vout) for tx in block.vtx),
                          expired=expired, utxos=len(coins), accept_seconds=timings,
                          delivery='p2p' if block.get_weight() == MAX_BLOCK_WEIGHT else 'rpc')
            self.profiles.append(result)
            self.log.info('Profile: %s', result)
        return block

    def reject(self, block, reason):
        for node in self.nodes:
            assert_equal(node.submitblock(block.serialize().hex()), reason)
            assert_equal(node.getbestblockhash(), self.tip)
        self.rejected += 1
        self.check()

    def check(self):
        states = []
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()['coinstatsindex']['synced'])
            fields = ('bestblock', 'txouts', 'total_amount', 'muhash')
            scan = node.gettxoutsetinfo('muhash', use_index=False)
            indexed = node.gettxoutsetinfo('muhash')
            assert_equal([scan[k] for k in fields], [indexed[k] for k in fields])
            assert_equal(scan['bestblock'], self.tip)
            assert_equal(scan['txouts'], len(self.coins))
            assert_equal(scan['total_amount'], Decimal(sum(c[0] for c in self.coins.values())) / COIN)
            assert_equal(node.getblockchaininfo(True)['recycling']['pool_balance'], Decimal(self.pool) / COIN)
            states.append([scan[k] for k in fields])
        assert_equal(*states)
        self.checkpoints += 1

    def check_exact_utxos(self):
        # Counts and total value alone could hide replacement of one output by
        # another. Independently enumerate every live output as well.
        scripts = (PAY, script_to_p2wsh_script(WITNESS_SCRIPT))
        scan = self.nodes[0].scantxoutset('start', [f'raw({script.hex()})' for script in scripts])
        assert_equal(scan['bestblock'], self.tip)
        actual = {(int(coin['txid'], 16), coin['vout']):
                  (int(coin['amount'] * COIN), coin['height'], coin['coinbase']) for coin in scan['unspents']}
        assert actual == self.coins
        self.exact_utxo_checks += 1

    def advance(self, height):
        while self.height < height:
            self.accept(self.candidate())

    @staticmethod
    def transaction(points, outputs):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(txid, n)) for txid, n in points]
        tx.vout = outputs
        return tx

    def overfull(self, block):
        oversized = deepcopy(block)
        oversized.vtx[0].vout[-1].scriptPubKey = CScript(bytes(oversized.vtx[0].vout[-1].scriptPubKey) + b'\0')
        self.solve(oversized)
        assert_equal(oversized.get_weight(), MAX_BLOCK_WEIGHT + 4)
        self.reject(oversized, 'bad-blk-length')

    def run_test(self):
        self.height, self.pool, self.coins = 0, 0, {}
        self.accepted, self.rejected, self.checkpoints = 0, 0, 0
        self.profiles = []
        self.exact_utxo_checks = 0
        self.timestamp = int(time.time())
        self.tip = self.nodes[0].getbestblockhash()
        for node in self.nodes:
            node.setmocktime(self.timestamp)
        funding = self.candidate()
        funding.vtx[0].vout = [CTxOut(COIN, PAY)] + [CTxOut(100000, script_to_p2wsh_script(WITNESS_SCRIPT)) for _ in range(40)]
        self.solve(funding)
        self.accept(funding)
        origin = funding.vtx[0].txid_int
        self.advance(100)

        self.log.info('Create 99,000 spendable outputs in an exactly full block')
        fanout = self.transaction([(origin, 0)], [CTxOut(1, PAY) for _ in range(99000)])
        block = self.candidate([fanout], full=True)
        self.overfull(block)
        self.accept(block, 'maximum output cohort')
        fanout_id = fanout.txid_int
        self.check()
        self.check_exact_utxos()
        self.log.info('Consecutive full blocks with 16,300 independent transactions each')
        for offset in (0, 16300):
            txs = [self.transaction([(fanout_id, n)], [CTxOut(1, PAY)]) for n in range(offset, offset + 16300)]
            block = self.candidate(txs, full=True)
            self.overfull(block)
            self.accept(block, 'many transactions')
            self.check()

        self.log.info('Witness data: exact 4,000,000 WU and one WU over the limit')
        witness = self.transaction([(origin, n) for n in range(1, 41)], [CTxOut(4000000, PAY)])
        for _ in witness.vin:
            item = CTxInWitness()
            item.scriptWitness.stack = [b'\0' * 490 for _ in range(200)] + [bytes(WITNESS_SCRIPT)]
            witness.wit.vtxinwit.append(item)
        block = self.candidate([witness])
        add_witness_commitment(block)
        remaining = MAX_BLOCK_WEIGHT - block.get_weight()
        assert remaining > 0
        for item in witness.wit.vtxinwit:
            for n in range(200):
                extra = min(remaining, 520 - len(item.scriptWitness.stack[n]))
                item.scriptWitness.stack[n] += b'\0' * extra
                remaining -= extra
        assert_equal(remaining, 0)
        block.vtx[0].vout.pop()
        add_witness_commitment(block)
        self.solve(block)
        assert_equal(block.get_weight(), MAX_BLOCK_WEIGHT)
        oversized = deepcopy(block)
        oversized.vtx[1].wit.vtxinwit[-1].scriptWitness.stack[199] += b'\0'
        oversized.vtx[0].vout.pop()
        add_witness_commitment(oversized)
        self.solve(oversized)
        assert_equal(oversized.get_weight(), MAX_BLOCK_WEIGHT + 1)
        self.reject(oversized, 'bad-blk-weight')
        self.accept(block, 'maximum witness payload')
        self.check()

        self.advance(300)
        before = (self.tip, self.height, self.pool, self.coins.copy())
        self.log.info('At expiry, fill a block with 16,300 spends while 50,100 outputs expire')
        txs = [self.transaction([(fanout_id, n)], [CTxOut(1, PAY)]) for n in range(32600, 48900)]
        block = self.candidate(txs, full=True)
        self.overfull(block)
        # These failures occur after the transaction overlay has been changed,
        # unlike weight-limit rejection. Excess reward and an asynchronous
        # script failure also exercise rollback after mass expiry was applied.
        self.log.info('Reject late validation failures without leaking modified expiry state')
        excessive = deepcopy(block)
        excessive.vtx[0].vout[0].nValue += 1
        self.solve(excessive)
        self.reject(excessive, 'bad-cb-amount')
        bad_script = deepcopy(block)
        bad_script.vtx[-1].vin[0].scriptSig = BURN
        bad_script.vtx[0].vout[-1].scriptPubKey = CScript(bytes(bad_script.vtx[0].vout[-1].scriptPubKey)[:-1])
        self.solve(bad_script)
        assert_equal(bad_script.get_weight(), MAX_BLOCK_WEIGHT)
        self.reject(bad_script, 'block-script-verify-flag-failed (OP_RETURN was encountered)')
        conflict = deepcopy(block)
        conflict.vtx[-1].vin[0].prevout = deepcopy(conflict.vtx[1].vin[0].prevout)
        # Keep the conflicting transaction distinct from the first transaction.
        conflict.vtx[-1].nLockTime = 1
        self.solve(conflict)
        assert_equal(conflict.get_weight(), MAX_BLOCK_WEIGHT)
        self.reject(conflict, 'bad-txns-inputs-missingorspent')
        if self.options.crash_expiry:
            self.restart_node(0, extra_args=self.extra_args[0] + ['-dbbatchsize=1', '-dbcrashratio=1'])
        expiry = self.accept(block, 'full block at mass expiry')
        if self.options.crash_expiry:
            self.log.info('Interrupt the mass-expiry database batch and recover')
            node = self.nodes[0]
            with node.assert_debug_log(['Simulating a crash. Goodbye.']):
                try:
                    node.gettxoutsetinfo('muhash', use_index=False)
                except (http.client.RemoteDisconnected, ConnectionResetError):
                    pass
                node.wait_until_stopped(timeout=60)
            self.start_node(0)
        self.check()
        self.check_exact_utxos()
        self.accept(self.candidate(), 'next cohort expiry')
        self.accept(self.candidate(), 'following cohort expiry')
        self.accept(self.candidate(), 'witness output expiry')
        self.check()
        after = (self.tip, self.height, self.pool, self.coins.copy())
        self.log.info('Disconnect and reconnect full blocks across the mass-expiry boundary')
        for node in self.nodes:
            node.invalidateblock(expiry.hash_hex)
        self.tip, self.height, self.pool, self.coins = deepcopy(before)
        self.check()
        for node in self.nodes:
            node.reconsiderblock(expiry.hash_hex)
        self.tip, self.height, self.pool, self.coins = deepcopy(after)
        self.check()

        # Full-chain disconnect verification needs enough cache for the 99k-coin
        # cohort. Keep the ingestion/crash/reorg phase at 4 MiB, then verify all
        # historical blocks with the normal cache budget on both implementations.
        if self.options.small_cache:
            self.extra_args = [[arg for arg in args if arg not in ('-dbcache=4', '-dbbatchsize=512', '-checklevel=2')] for args in self.extra_args]
            for i, node in enumerate(self.nodes):
                node.extra_args = self.extra_args[i]
        self.log.info('Restart, verify all blocks, and rebuild chainstate independently')
        for i in range(2):
            self.restart_node(i)
            assert self.nodes[i].verifychain(4, 0)
        self.check()
        self.restart_node(1, extra_args=self.extra_args[1] + ['-reindex-chainstate'])
        self.check()
        for node in self.nodes:
            node.invalidateblock(expiry.hash_hex)
        self.tip, self.height, self.pool, self.coins = deepcopy(before)
        self.check()
        for node in self.nodes:
            node.reconsiderblock(expiry.hash_hex)
        self.tip, self.height, self.pool, self.coins = deepcopy(after)
        self.check()
        self.advance(501)
        self.check()
        self.check_exact_utxos()
        assert_equal(len(self.coins), 0)
        assert_equal(self.pool, 0)
        self.restart_node(0, extra_args=self.extra_args[0] + ['-reindex-chainstate'])
        self.check()
        result = dict(reference=self.options.reference_bitcoind, small_cache=self.options.small_cache,
                      crash_expiry=self.options.crash_expiry, accepted_blocks=self.accepted,
                      rejected_blocks=self.rejected, checkpoints=self.checkpoints, exact_utxo_checks=self.exact_utxo_checks, profiles=self.profiles,
                      final_height=self.height, final_utxos=len(self.coins), final_pool=self.pool)
        if self.options.full_block_results:
            Path(self.options.full_block_results).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        self.log.info('Full block matrix passed: %s', result)


if __name__ == '__main__':
    RecycleFullBlockTest(__file__).main()
