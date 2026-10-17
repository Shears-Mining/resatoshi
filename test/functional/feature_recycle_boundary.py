#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Compare expiry boundaries against an independent ledger and optional reference node."""

from decimal import Decimal
import http.client
import json
from pathlib import Path
import random
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut
from test_framework.script import CScript, OP_RETURN, OP_TRUE
from test_framework.script_util import script_to_p2sh_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

LIFETIME = 200
REDEEM = CScript([OP_TRUE])
PAY = script_to_p2sh_script(REDEEM)
BURN = CScript([OP_RETURN])


class RecycleBoundaryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [['-test=recycle', '-coinstatsindex', '-checklevel=4', '-checkblocks=0'] for _ in range(2)]

    def add_options(self, parser):
        parser.add_argument('--reference-bitcoind')
        parser.add_argument('--expiry-mode', choices=('index', 'block'), default='index')
        parser.add_argument('--boundary-seed', type=int, default=17)
        parser.add_argument('--boundary-results')
        parser.add_argument('--small-cache', action='store_true')
        parser.add_argument('--crash-height', type=int, choices=(200, 201, 202))

    def setup_network(self):
        # Submit every block explicitly to both implementations. No peer relay
        # or transient branch selection can hide a disagreement in acceptance.
        if self.options.small_cache:
            self.extra_args = [args + ['-dbcache=4', '-dbbatchsize=512'] for args in self.extra_args]
        self.extra_args[0].append(f'-recyclemode={self.options.expiry_mode}')
        self.add_nodes(2, extra_args=self.extra_args)
        if self.options.reference_bitcoind:
            self.nodes[1].args[0] = str(Path(self.options.reference_bitcoind).resolve())
        self.start_nodes()

    def mark(self, label):
        self.cases.append(label)
        self.log.info(label)

    def spend(self, points, *, fee=1000, values=None, burn=False):
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(txid, 16), n), CScript([REDEEM])) for txid, n in points]
        total = sum(self.coins[p][0] for p in points)
        if values is None:
            values = [total - fee]
        tx.vout = [CTxOut(v, BURN if burn else PAY) for v in values]
        return tx

    @staticmethod
    def point(tx, n=0):
        return (tx.txid_hex, n)

    def transition(self, txs, height):
        coins = self.coins.copy()
        fees = 0
        for tx in txs:
            value = 0
            for vin in tx.vin:
                key = (f'{vin.prevout.hash:064x}', vin.prevout.n)
                amount, origin, coinbase = coins.pop(key)
                assert not coinbase or height - origin >= 100
                value += amount
            fees += value - sum(o.nValue for o in tx.vout)
            for n, output in enumerate(tx.vout):
                if output.scriptPubKey != BURN:
                    coins[self.point(tx, n)] = (output.nValue, height, False)
        expired = [key for key, (_, origin, _) in coins.items() if height - origin == LIFETIME]
        available = self.pool + sum(coins[key][0] for key in expired)
        for key in expired:
            del coins[key]
        return coins, fees, available, expired

    def candidate(self, txs=(), *, values=None, claim=None, over=0, spendable_reward=False, raw_value=None):
        height = self.height + 1
        coinbase = create_coinbase(height)
        subsidy = coinbase.vout[0].nValue
        if raw_value is None:
            _, fees, available, _ = self.transition(txs, height)
            value = subsidy + fees + (min(COIN, available) if claim is None else claim) + over
        else:
            value = raw_value
        coinbase.vout = [CTxOut(v, PAY) for v in values] if values is not None else [CTxOut(value, PAY if spendable_reward else BURN)]
        self.unique += 1
        coinbase.vin[0].scriptSig = CScript(bytes(coinbase.vin[0].scriptSig) + bytes(CScript([self.unique])))
        block = create_block(int(self.tip, 16), coinbase, max(int(time.time()), self.nodes[0].getblockheader(self.tip)['time'] + 1), txlist=list(txs))
        block.solve()
        return block

    def accept(self, block):
        height = self.height + 1
        crash = height == self.options.crash_height and not self.crashed
        if crash:
            self.restart_node(0, extra_args=self.extra_args[0] + ['-dbbatchsize=1', '-dbcrashratio=1'])
        coins, fees, available, expired = self.transition(block.vtx[1:], height)
        subsidy = create_coinbase(height).vout[0].nValue
        claimed = max(0, sum(o.nValue for o in block.vtx[0].vout) - subsidy - fees)
        assert claimed <= min(COIN, available)
        for node in self.nodes:
            assert_equal(node.submitblock(block.serialize().hex()), None)
            assert_equal(node.getblockstats(block.hash_hex)['expired_outputs'], len(expired))
        for n, output in enumerate(block.vtx[0].vout):
            if output.scriptPubKey != BURN:
                coins[self.point(block.vtx[0], n)] = (output.nValue, height, True)
        self.coins, self.pool, self.height, self.tip = coins, available - claimed, height, block.hash_hex
        self.history[self.tip] = (self.height, self.pool, self.coins.copy())
        self.accepted += 1
        if crash:
            self.mark(f'Interrupted index flush at height {height}, then recover against reference state')
            node = self.nodes[0]
            with node.assert_debug_log(['Simulating a crash. Goodbye.']):
                try:
                    node.gettxoutsetinfo('muhash', use_index=False)
                except (http.client.RemoteDisconnected, ConnectionResetError):
                    pass
                node.wait_until_stopped(timeout=20)
            self.start_node(0)
            self.crashed = True
            self.check()
        for node in self.nodes:
            assert_equal(node.getblockchaininfo(True)['recycling']['pool_balance'], Decimal(self.pool) / COIN)
        return block

    def reject(self, block, reason):
        for node in self.nodes:
            assert_equal(node.submitblock(block.serialize().hex()), reason)
            assert_equal(node.getbestblockhash(), self.tip)
        self.rejected += 1
        self.check()

    def advance(self, height):
        while self.height < height:
            self.accept(self.candidate())

    def check(self):
        states = []
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()['coinstatsindex']['synced'])
            scan = node.gettxoutsetinfo('muhash', use_index=False)
            indexed = node.gettxoutsetinfo('muhash')
            fields = ('bestblock', 'txouts', 'total_amount', 'muhash')
            assert_equal([scan[k] for k in fields], [indexed[k] for k in fields])
            assert_equal(scan['bestblock'], self.tip)
            assert_equal(scan['txouts'], len(self.coins))
            assert_equal(scan['total_amount'], Decimal(sum(c[0] for c in self.coins.values())) / COIN)
            assert_equal(node.getblockchaininfo(True)['recycling']['pool_balance'], Decimal(self.pool) / COIN)
            states.append([scan[k] for k in fields])
        assert_equal(*states)
        self.checkpoints += 1

    def restore(self, tip):
        self.tip = tip
        self.height, self.pool, coins = self.history[tip]
        self.coins = coins.copy()
        self.check()

    def run_test(self):
        self.coins, self.pool, self.height, self.unique = {}, 0, 0, 0
        self.crashed = False
        self.accepted, self.rejected, self.checkpoints, self.cases = 0, 0, 0, []
        self.tip = self.nodes[0].getbestblockhash()
        self.history = {self.tip: (0, 0, {})}
        for node in self.nodes:
            node.setmocktime(int(time.time()))
        first = self.accept(self.candidate(values=[2 * COIN] * 8 + [1, 1, 0]))
        origin = first.vtx[0]
        second = self.accept(self.candidate(values=[COIN // 2, 1, 0, COIN]))
        self.accept(self.candidate())
        self.accept(self.candidate(values=[1] * 300))
        cohorts = {}
        for height in (55, 56, 57):
            self.advance(height - 1)
            cohorts[height] = self.accept(self.candidate(values=[100_000, 0])).vtx[0]
        self.advance(99)
        self.mark('Coinbase maturity: reject age 99; accept age 100')
        early = self.spend([self.point(origin, 5)], fee=1000)
        self.reject(self.candidate([early], raw_value=0), 'bad-txns-premature-spend-of-coinbase')
        self.accept(self.candidate())
        regular = self.spend([self.point(origin, 5)], values=[COIN, COIN // 2, COIN // 2 - 1000])
        self.accept(self.candidate([regular]))
        replacement = self.spend([self.point(regular, 2)], fee=1000)
        self.accept(self.candidate([replacement]))
        self.mark('Subsidy halving: exact limit and one satoshi over at 149/150/151')
        self.advance(148)
        for _ in range(3):
            self.reject(self.candidate(over=1), 'bad-cb-amount')
            self.accept(self.candidate())
        self.advance(199)
        self.mark('Expiry minus one: no recycle reward; valid spend resets lifetime')
        before = self.spend([self.point(origin, 0)], fee=100_000)
        self.reject(self.candidate([before], over=1), 'bad-cb-amount')
        self.accept(self.candidate([before]))
        at200 = self.tip
        self.check()
        self.mark('Mempool parent and descendant are removed when their input expires')
        pending = self.spend([self.point(origin, 3)], fee=10_000)
        descendant = CTransaction()
        descendant.vin = [CTxIn(COutPoint(pending.txid_int, 0), CScript([REDEEM]))]
        descendant.vout = [CTxOut(pending.vout[0].nValue - 10_000, PAY)]
        for node in self.nodes:
            node.sendrawtransaction(pending.serialize().hex())
            node.sendrawtransaction(descendant.serialize().hex())
            assert_equal(set(node.getrawmempool()), {pending.txid_hex, descendant.txid_hex})
        self.mark('Expiry double spends are rejected without changing the ledger')
        duplicate = self.spend([self.point(origin, 1)], fee=1000)
        duplicate.vin.append(duplicate.vin[0])
        self.reject(self.candidate([duplicate], raw_value=0), 'bad-txns-inputs-duplicate')
        conflict = [self.spend([self.point(origin, 1)], fee=fee) for fee in (1000, 2000)]
        self.reject(self.candidate(conflict, raw_value=0), 'bad-txns-inputs-missingorspent')
        self.mark('Exact expiry: spends win; parent/child in one block; fees and partial reward')
        exact = self.spend([self.point(origin, 1)], fee=100_000)
        parent = self.spend([self.point(origin, 4)], fee=20_000)
        child = CTransaction()
        child.vin = [CTxIn(COutPoint(parent.txid_int, 0), CScript([REDEEM]))]
        child.vout = [CTxOut(parent.vout[0].nValue - 20_000, PAY)]
        self.reject(self.candidate([exact, parent, child], over=1), 'bad-cb-amount')
        canonical201 = self.accept(self.candidate([exact, parent, child], claim=COIN // 4, spendable_reward=True))
        for node in self.nodes:
            assert_equal(node.getrawmempool(), [])
            assert_equal(node.gettxout(parent.txid_hex, 0), None)
        self.mark('Expiry plus one: expired inputs cannot be spent; adjacent height and zero values expire')
        late = CTransaction()
        late.vin = [CTxIn(COutPoint(origin.txid_int, 2), CScript([REDEEM]))]
        late.vout = [CTxOut(COIN, PAY)]
        self.reject(self.candidate([late], raw_value=0), 'bad-txns-inputs-missingorspent')
        self.accept(self.candidate())
        self.accept(self.candidate())
        self.accept(self.candidate())
        self.check()
        self.mark('Pool drain: empty expiry height, one-coin cap, sub-coin remainder, empty pool')
        while self.pool >= COIN:
            self.accept(self.candidate())
        self.reject(self.candidate(over=1), 'bad-cb-amount')
        self.accept(self.candidate())
        assert_equal(self.pool, 0)
        self.reject(self.candidate(over=1), 'bad-cb-amount')
        self.accept(self.candidate())
        canonical_tip, canonical_height = self.tip, self.height
        self.mark('Multi-block reorg across expiry, alternate spends, and switching branches twice')
        for node in self.nodes:
            node.invalidateblock(canonical201.hash_hex)
        self.restore(at200)
        alt_spends = [self.spend([self.point(origin, n)], fee=30_000) for n in (2, 3)]
        alternate201 = self.accept(self.candidate(alt_spends, claim=COIN // 2, spendable_reward=True))
        alt202 = self.spend([self.point(second.vtx[0], 0)], fee=10_000)
        self.accept(self.candidate([alt202], claim=0))
        self.advance(canonical_height + 1)
        alternate_tip = self.tip
        for node in self.nodes:
            node.reconsiderblock(canonical201.hash_hex)
            assert_equal(node.getbestblockhash(), alternate_tip)
            node.invalidateblock(alternate201.hash_hex)
        self.restore(canonical_tip)
        for node in self.nodes:
            node.reconsiderblock(alternate201.hash_hex)
        self.restore(alternate_tip)
        for height in (254, 255, 256, 257, 258):
            if height == 256:
                self.mark('Every output in the expiry cohort is spent, including a zero-value output')
                all_spent = self.spend([self.point(cohorts[56], n) for n in range(2)], fee=1000)
                self.reject(self.candidate([all_spent], over=1), 'bad-cb-amount')
                boundary = self.accept(self.candidate([all_spent]))
                for node in self.nodes:
                    assert_equal(node.getblockstats(boundary.hash_hex)['expired_outputs'], 0)
            else:
                self.advance(height)
            self.check()
        self.mark('Height encoding boundaries 255/256/257 agree with the independent ledger')
        self.advance(299)
        self.mark('Recycled coinbase maturity and second-generation regular output expiry')
        reward_spend = self.spend([self.point(alternate201.vtx[0])], fee=1000)
        self.reject(self.candidate([reward_spend], raw_value=0), 'bad-txns-premature-spend-of-coinbase')
        preceding = self.spend([self.point(regular, 0)], fee=1000)
        self.accept(self.candidate([preceding]))
        self.reject(self.candidate([reward_spend], over=1), 'bad-cb-amount')
        self.accept(self.candidate([reward_spend]))
        self.accept(self.candidate())
        self.check()
        self.advance(399)
        self.mark('Outputs renewed at 200/201/202 expire at 400/401/402, not at their ancestors expiry')
        self.accept(self.candidate([self.spend([self.point(before)], fee=1000)]))
        self.accept(self.candidate([self.spend([self.point(alt_spends[0])], fee=1000)]))
        self.accept(self.candidate())
        self.check()
        self.advance(501)
        self.check()
        self.mark('Randomized spends, zero outputs, partial rewards, restarts, and new expiry cohorts')
        rng = random.Random(self.options.boundary_seed)
        for height in range(502, 751):
            eligible = [p for p, (_, origin_height, coinbase) in self.coins.items() if not coinbase or height - origin_height >= 100]
            rng.shuffle(eligible)
            selected = eligible[:rng.randrange(min(5, len(eligible)) + 1)]
            txs = []
            if selected:
                total = sum(self.coins[p][0] for p in selected)
                fee = min(1000, total // 20)
                txs = [self.spend(selected, fee=fee, burn=rng.randrange(7) == 0)]
            # Mix claimed rewards with new coinbase cohorts below the subsidy.
            if height % 3:
                block = self.candidate(txs, values=[rng.randrange(20_001) for _ in range(rng.randrange(1, 5))])
            else:
                block = self.candidate(txs, claim=0 if height % 2 else None)
            self.accept(block)
            if height % 17 == 0:
                self.check()
            if height % 61 == 0:
                self.restart_node(0)
                self.check()
        self.check()
        self.mark('Independent restart/reindex and historical expiry undo')
        for i in range(2):
            self.restart_node(i)
        self.check()
        for node in self.nodes:
            assert node.verifychain(4, 0)
        self.restart_node(1, extra_args=self.extra_args[1] + ['-reindex-chainstate'])
        self.check()
        final_tip = self.tip
        for node in self.nodes:
            node.invalidateblock(alternate201.hash_hex)
        self.restore(canonical_tip)
        for node in self.nodes:
            node.reconsiderblock(alternate201.hash_hex)
        self.restore(final_tip)
        for node in self.nodes:
            assert node.verifychain(4, 0)
        assert self.crashed == (self.options.crash_height is not None)
        result = {'crash_height': self.options.crash_height, 'seed': self.options.boundary_seed, 'small_cache': self.options.small_cache,
                  'reference': self.options.reference_bitcoind, 'expiry_mode': self.options.expiry_mode, 'accepted_blocks': self.accepted,
                  'rejected_blocks': self.rejected, 'state_checkpoints': self.checkpoints,
                  'final_height': self.height, 'utxos': len(self.coins), 'pool_satoshis': self.pool,
                  'cases': self.cases}
        if self.options.boundary_results:
            Path(self.options.boundary_results).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        self.log.info('Boundary matrix passed: %s', result)


if __name__ == '__main__':
    RecycleBoundaryTest(__file__).main()
