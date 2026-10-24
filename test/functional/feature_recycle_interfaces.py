#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check snapshot tooling, REST undo, and opt-in descriptor expiry across a reorg."""

from contextlib import closing
from decimal import Decimal
import http.client
import importlib.util
import io
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import urllib.parse

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COIN, CTxOut, MAGIC_BYTES, deser_block_spent_outputs, hash256, ser_varint
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises, assert_raises_rpc_error, util_xor
from test_framework.wallet import MiniWallet


class RecycleInterfacesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [['-test=recycle', '-blockfilterindex', '-rest']]

    def spent_outputs(self, blockhash, fmt, status=200):
        url = urllib.parse.urlparse(self.nodes[0].url)
        conn = http.client.HTTPConnection(url.hostname, url.port, timeout=self.nodes[0].rpc_timeout)
        try:
            conn.request('GET', f'/rest/spenttxouts/{blockhash}.{fmt}')
            response = conn.getresponse()
            body = response.read()
            assert_equal(response.status, status)
            return body
        finally:
            conn.close()

    def check_spent_outputs(self, blockhash):
        block = self.nodes[0].getblock(blockhash, 3)
        binary = self.spent_outputs(blockhash, 'bin')
        assert_equal(bytes.fromhex(self.spent_outputs(blockhash, 'hex').decode()), binary)
        stream = io.BytesIO(binary)
        decoded = deser_block_spent_outputs(stream)
        assert_equal(stream.read(), b'')
        decoded_json = json.loads(self.spent_outputs(blockhash, 'json'), parse_float=Decimal)
        assert_equal(len(decoded), len(decoded_json), len(block['tx']))
        for tx, inputs, inputs_json in zip(block['tx'], decoded, decoded_json):
            expected = [(vin['prevout']['scriptPubKey']['hex'], vin['prevout']['value'])
                        for vin in tx['vin'] if 'coinbase' not in vin]
            assert_equal([(txout.scriptPubKey.hex(), Decimal(txout.nValue) / COIN) for txout in inputs], expected)
            assert_equal([(prevout['scriptPubKey']['hex'], prevout['value']) for prevout in inputs_json], expected)

    def check_inconsistent_undo(self, blockhash):
        node = self.nodes[0]
        previous = bytes.fromhex(node.getblockheader(blockhash)['previousblockhash'])[::-1]
        self.wait_until(lambda: node.getindexinfo()['basic block filter index']['synced'])
        self.stop_node(0)
        path = node.blocks_path / 'rev00000.dat'
        original = path.read_bytes()
        key = node.read_xor_key()
        data = bytearray(util_xor(original, key, offset=0))
        offset = 0
        matches = []
        while data[offset:offset + 4] == MAGIC_BYTES['regtest']:
            start = offset + 8
            size = int.from_bytes(data[offset + 4:start], 'little')
            end = start + size
            if hash256(previous + data[start:end]) == data[end:end + 32]:
                matches.append(offset)
            offset = end + 32
        assert_equal(len(matches), 1)
        start = matches[0] + 8
        # A valid checksum with no transaction/state undo records: readable,
        # but inconsistent with this post-expiry block. Keep other offsets intact.
        payload = b'\x00\x00'
        data[start - 4:start] = len(payload).to_bytes(4, 'little')
        data[start:start + len(payload) + 32] = payload + hash256(previous + payload)
        try:
            path.write_bytes(util_xor(data, key, offset=0))
            self.start_node(0, extra_args=self.extra_args[0] + ['-checklevel=0'])
            for fmt in ('json', 'hex', 'bin'):
                assert b'undo data inconsistent with block' in self.spent_outputs(blockhash, fmt, status=500)
        finally:
            if node.running:
                self.stop_node(0)
            path.write_bytes(original)
        self.start_node(0)
        self.check_spent_outputs(blockhash)
        assert node.verifychain(4, 0)

    def run_test(self):
        node = self.nodes[0]
        rules = {'enabled': True, 'mode': 'block', 'expiry_blocks': 200, 'max_reward_per_block': 1, 'pool_balance': 0}
        assert_equal(node.getblockchaininfo(True)['recycling'], rules)
        assert 'recycling' not in node.getblockchaininfo()
        assert 'recycling' not in node.getblockchaininfo(False)
        if self.is_cli_compiled():
            assert_equal(node.cli.getblockchaininfo(True)['recycling'], rules)
            assert_equal(node.cli.getblockchaininfo(include_recycling=True)['recycling'], rules)
            assert 'recycling' not in node.cli.getblockchaininfo(include_recycling=False)
        script = bytes.fromhex('0014' + '12' * 20)
        desc = f'raw({script.hex()})'
        coinbase = create_coinbase(1)
        coinbase.vout = [CTxOut(COIN // 10, script) for _ in range(278)]
        prev = node.getblockheader(node.getbestblockhash())
        block = create_block(int(prev['hash'], 16), coinbase, prev['time'] + 1)
        block.solve()
        assert_equal(node.submitblock(block.serialize().hex()), None)

        tool = Path(__file__).resolve().parents[2] / 'contrib/utxo-tools/utxo_to_sqlite.py'
        snapshot = node.dumptxoutset(str(Path(self.options.tmpdir) / 'snapshot.dat'), 'latest')
        for fmt in ('hex', 'raw'):
            database = Path(self.options.tmpdir) / f'snapshot-{fmt}.sqlite'
            subprocess.run([sys.executable, str(tool), snapshot['path'], str(database), f'--spk={fmt}', f'--txid={fmt}'], check=True, capture_output=True, text=True)
            with closing(sqlite3.connect(database)) as con:
                assert_equal(con.execute('SELECT COUNT(*), SUM(value) FROM utxos').fetchone(), (278, 278 * COIN // 10))
                assert_equal(snapshot['coins_written'], 278)
                assert_equal(con.execute('SELECT balance FROM recycle_pool').fetchall(), [(0,)])

        spec = importlib.util.spec_from_file_location('utxo_converter', tool)
        converter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(converter)
        oversized = ser_varint(10007) + b'\x51' * 10001
        assert_raises(ValueError, converter.decompress_script, io.BytesIO(oversized))
        fixture = Path(snapshot['path']).read_bytes()
        assert_equal(int.from_bytes(fixture[5:7], 'little'), 3)
        assert_equal(int.from_bytes(fixture[51:59], 'little'), 0)

        self.generatetodescriptor(node, 200, 'raw(6a)')
        self.wait_until(lambda: node.getindexinfo()['basic block filter index']['synced'])
        expiry = node.getbestblockhash()
        state = node.getblockchaininfo(True)
        assert_equal(state['bestblockhash'], expiry)
        rules['pool_balance'] = Decimal('26.8')
        assert_equal(state['recycling'], rules)
        hashes = [block.hash_hex, expiry]
        legacy = node.getdescriptoractivity(hashes, [desc], False)
        assert_equal(legacy, node.getdescriptoractivity(hashes, [desc], False, False))
        assert_equal(len(legacy['activity']), 278)
        assert all(event['type'] == 'receive' and 'expiry_height' not in event for event in legacy['activity'])
        events = node.getdescriptoractivity(hashes, [desc], False, True)['activity']
        if self.is_cli_compiled():
            self.log.info('Check CLI positional and named expiry arguments against RPC results')
            assert_equal(node.cli.getdescriptoractivity(hashes, [desc], False), legacy)
            for include_expired in (False, True):
                expected = {'activity': events} if include_expired else legacy
                assert_equal(node.cli.getdescriptoractivity(hashes, [desc], False, include_expired), expected)
                assert_equal(node.cli.getdescriptoractivity(
                    blockhashes=hashes, scanobjects=[desc],
                    include_mempool=False, include_expired=include_expired,
                ), expected)
        received = [event for event in events if event['type'] == 'receive']
        expired = [event for event in events if event['type'] == 'expire']
        assert_equal(len(received), 278)
        assert all(event['expiry_height'] == 201 for event in received)
        assert_equal(len(expired), 278)
        assert_equal({event['prevout_vout'] for event in expired}, set(range(278)))
        for event in expired:
            assert_equal(event['height'], 201)
            assert_equal(event['creation_height'], 1)
            assert_equal(event['blockhash'], expiry)
            assert_equal(event['prevout_txid'], coinbase.txid_hex)
            assert 'spend_txid' not in event
        assert_equal(node.gettxout(coinbase.txid_hex, 0), None)
        pool_snapshot = node.dumptxoutset(str(Path(self.options.tmpdir) / 'pool.dat'), 'latest')
        pool_db = Path(self.options.tmpdir) / 'pool.sqlite'
        subprocess.run([sys.executable, str(tool), pool_snapshot['path'], str(pool_db)], check=True, capture_output=True, text=True)
        with closing(sqlite3.connect(pool_db)) as con:
            assert_equal(con.execute('SELECT COUNT(*) FROM utxos').fetchone(), (0,))
            assert_equal(pool_snapshot['coins_written'], 0)
            assert_equal(con.execute('SELECT balance FROM recycle_pool').fetchone(), (2_680_000_000,))
        assert_equal(node.scanblocks('start', [desc], 201, 201)['relevant_blocks'], [])
        node.invalidateblock(expiry)
        assert_equal(node.getblockchaininfo(True)['recycling']['pool_balance'], 0)
        assert node.gettxout(coinbase.txid_hex, 0) is not None
        assert_raises_rpc_error(-8, 'Block is not in main chain', node.getdescriptoractivity, [expiry], [desc], False, True)
        node.reconsiderblock(expiry)
        assert_equal(node.getdescriptoractivity(hashes, [desc], False, True)['activity'], events)
        self.restart_node(0)
        assert_equal(node.getblockchaininfo(True)['recycling'], rules)
        assert_equal(node.getdescriptoractivity(hashes, [desc], False, True)['activity'], events)

        self.log.info('Check REST undo at genesis and before/at/after first expiry')
        self.generatetodescriptor(node, 1, 'raw(6a)')
        for height in (0, 1, 200, 201, 202):
            self.check_spent_outputs(node.getblockhash(height))
        self.log.info('Check real transaction inputs survive recycle undo filtering')
        wallet = MiniWallet(node)
        self.generate(wallet, 101)
        wallet.send_self_transfer(from_node=node)
        tip = self.generate(wallet, 1)[0]
        assert_equal(len(node.getblock(tip)['tx']), 2)
        self.check_spent_outputs(tip)
        self.check_inconsistent_undo(tip)


if __name__ == '__main__':
    RecycleInterfacesTest(__file__).main()
