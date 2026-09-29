#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fill mining templates with standard transactions at their inputs' expiry height."""

from decimal import Decimal
import json
from pathlib import Path
import time

from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, MAX_BLOCK_WEIGHT, CTransaction, CTxOut, from_hex
from test_framework.p2p import P2PInterface
from test_framework.script import CScript, OP_RETURN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet

COUNT = 10000
VALUE = 100000
FEE = 1000
BURN = CScript([OP_RETURN])


class RecycleMiningLoadTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Require standard relay policy even on regtest. Sample the expensive
        # debug consistency check; checking all entries on every insertion
        # makes this workload unnecessarily quadratic. Consensus/policy checks
        # still run for every transaction.
        self.extra_args = [['-test=recycle', '-coinstatsindex', '-acceptnonstdtxn=0', '-checkmempool=1000', '-checklevel=4', '-checkblocks=0'] for _ in range(2)]

    def add_options(self, parser):
        parser.add_argument('--legacy-bitcoind')
        parser.add_argument('--mining-results')

    def setup_network(self):
        self.add_nodes(2, extra_args=self.extra_args)
        if self.options.legacy_bitcoind:
            self.nodes[1].args[0] = str(Path(self.options.legacy_bitcoind).resolve())
        self.start_nodes()
        for node in self.nodes:
            node.add_p2p_connection(P2PInterface())

    def states(self, *, count, value, pool):
        states = []
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()['coinstatsindex']['synced'])
            scan = node.gettxoutsetinfo('muhash', use_index=False)
            indexed = node.gettxoutsetinfo('muhash')
            fields = ('bestblock', 'txouts', 'total_amount', 'muhash')
            assert_equal([scan[k] for k in fields], [indexed[k] for k in fields])
            assert_equal(scan['txouts'], count)
            assert_equal(scan['total_amount'], Decimal(value) / COIN)
            assert_equal(node.getblockchaininfo(True)['recycling']['pool_balance'], Decimal(pool) / COIN)
            states.append([scan[k] for k in fields])
        assert_equal(*states)
        self.checkpoints += 1
        return states[0]

    def candidate(self, height, txs=(), reward=None):
        coinbase = create_coinbase(height)
        coinbase.vout[0].scriptPubKey = BURN
        if reward is not None:
            coinbase.vout[0].nValue = reward
        block = create_block(int(self.nodes[0].getbestblockhash(), 16), coinbase, self.timestamp + height, txlist=list(txs))
        if txs:
            add_witness_commitment(block)
        block.solve()
        return block

    def submit(self, block):
        for node in self.nodes:
            assert_equal(node.submitblock(block.serialize().hex()), None)
            assert_equal(node.getbestblockhash(), block.hash_hex)

    def run_test(self):
        self.timestamp = int(time.time())
        self.checkpoints = 0
        for node in self.nodes:
            node.setmocktime(self.timestamp)
        wallet = MiniWallet(self.nodes[0])
        funding = self.candidate(1)
        funding.vtx[0].vout = [CTxOut(VALUE, wallet.get_output_script()) for _ in range(COUNT)]
        funding.hashMerkleRoot = funding.calc_merkle_root()
        funding.solve()
        self.submit(funding)
        origin = funding.vtx[0].txid_hex
        for height in range(2, 201):
            self.submit(self.candidate(height))
        before = self.states(count=COUNT, value=COUNT * VALUE, pool=0)
        self.log.info('Fill both mempools with 10,000 standard Taproot script-path transactions')
        transactions = {}
        for n in range(COUNT):
            utxo = dict(txid=origin, vout=n, value=Decimal(VALUE) / COIN)
            tx = wallet.create_self_transfer(utxo_to_spend=utxo, fee=Decimal(FEE) / COIN)
            transactions[tx['txid']] = tx['hex']
            for node in self.nodes:
                assert_equal(node.sendrawtransaction(tx['hex']), tx['txid'])
            if (n + 1) % 2000 == 0:
                self.log.info('Accepted %d standard transactions on each node', n + 1)
        for node in self.nodes:
            assert_equal(set(node.getrawmempool()), set(transactions))

        self.log.info('Build a near-full template at expiry; exclude selected inputs from recycle reward')
        templates = [node.getblocktemplate({'rules': ['segwit']}) for node in self.nodes]
        for template in templates:
            selected = template['transactions']
            assert 9500 < len(selected) < COUNT
            assert_equal(sum(entry['fee'] for entry in selected), len(selected) * FEE)
            expired = COUNT - len(selected)
            reward = create_coinbase(201).vout[0].nValue + len(selected) * FEE + min(COIN, expired * VALUE)
            assert_equal(template['coinbasevalue'], reward)
            assert_equal(template['height'], 201)
        template = templates[0]
        selected = template['transactions']
        included = {entry['txid'] for entry in selected}
        expired = COUNT - len(selected)
        assert expired > 0
        block = self.candidate(201, [from_hex(CTransaction(), entry['data']) for entry in selected], template['coinbasevalue'])
        assert 3990000 < block.get_weight() <= MAX_BLOCK_WEIGHT
        self.log.info('Repeated proposal validation must leave the live UTXO/index/pool state unchanged')
        for _ in range(3):
            for node in self.nodes:
                assert_equal(node.getblocktemplate({'mode': 'proposal', 'data': block.serialize().hex(), 'rules': ['segwit']}), None)
            assert_equal(self.states(count=COUNT, value=COUNT * VALUE, pool=0), before)
        self.submit(block)
        pool = max(0, expired * VALUE - COIN)
        after = self.states(count=len(selected), value=len(selected) * (VALUE - FEE), pool=pool)
        for node in self.nodes:
            assert_equal(node.getblockstats(block.hash_hex)['expired_outputs'], expired)
            # Selected transactions are mined; every leftover transaction loses
            # its input to expiry and must be removed from the mempool.
            assert_equal(node.getrawmempool(), [])
            late = node.testmempoolaccept([transactions[next(iter(set(transactions) - included))]])[0]
            assert_equal(late['allowed'], False)
            assert_equal(late['reject-reason'], 'missing-inputs')

        self.log.info('Reorg restores all funding UTXOs; resubmission and template assembly still work')
        for node in self.nodes:
            node.invalidateblock(block.hash_hex)
        assert_equal(self.states(count=COUNT, value=COUNT * VALUE, pool=0), before)
        for raw in transactions.values():
            for node in self.nodes:
                node.sendrawtransaction(raw)
        for node in self.nodes:
            assert_equal(set(node.getrawmempool()), set(transactions))
            refreshed = node.getblocktemplate({'rules': ['segwit']})
            count = len(refreshed['transactions'])
            assert 9500 < count < COUNT
            assert_equal(refreshed['coinbasevalue'], create_coinbase(201).vout[0].nValue + count * FEE + min(COIN, (COUNT - count) * VALUE))
            node.reconsiderblock(block.hash_hex)
            assert_equal(node.getrawmempool(), [])
        assert_equal(self.states(count=len(selected), value=len(selected) * (VALUE - FEE), pool=pool), after)
        for i in range(2):
            self.restart_node(i)
            assert self.nodes[i].verifychain(4, 0)
        assert_equal(self.states(count=len(selected), value=len(selected) * (VALUE - FEE), pool=pool), after)
        self.log.info('Renewed outputs expire on their own 200-block lifetime')
        for height in range(202, 401):
            claim = min(COIN, pool)
            self.submit(self.candidate(height, reward=create_coinbase(height).vout[0].nValue + claim))
            pool -= claim
        pool += len(selected) * (VALUE - FEE)
        claim = min(COIN, pool)
        renewal = self.candidate(401, reward=create_coinbase(401).vout[0].nValue + claim)
        self.submit(renewal)
        pool -= claim
        for node in self.nodes:
            assert_equal(node.getblockstats(renewal.hash_hex)['expired_outputs'], len(selected))
        self.states(count=0, value=0, pool=pool)
        result = dict(legacy=self.options.legacy_bitcoind, mempool_transactions=COUNT, selected_transactions=len(selected),
                      expired_at_full_block=expired, block_weight=block.get_weight(), block_bytes=len(block.serialize()),
                      proposal_validations=6, checkpoints=self.checkpoints, final_height=401, final_utxos=0,
                      final_pool_satoshis=pool, standard_relay_required=True, mempool_consistency_interval=1000)
        if self.options.mining_results:
            Path(self.options.mining_results).write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        self.log.info('Mining load passed: %s', result)


if __name__ == '__main__':
    RecycleMiningLoadTest(__file__).main()
