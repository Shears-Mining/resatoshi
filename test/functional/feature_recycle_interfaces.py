#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check snapshot tooling and opt-in descriptor expiry across a reorg."""

from decimal import Decimal
import importlib.util
import io
from pathlib import Path
import sqlite3
import subprocess
import sys

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import COIN, CTxOut, ser_varint
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises, assert_raises_rpc_error


class RecycleInterfacesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [['-test=recycle', '-blockfilterindex']]

    def run_test(self):
        node = self.nodes[0]
        rules = {'enabled': True, 'expiry_blocks': 200, 'max_reward_per_block': 1, 'pool_balance': 0}
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
            with sqlite3.connect(database) as con:
                assert_equal(con.execute('SELECT COUNT(*), SUM(value) FROM utxos').fetchone(), (278, 278 * COIN // 10))
                states = con.execute('SELECT txid, vout, scriptpubkey FROM recycle_state').fetchall()
                assert_equal(len(states), snapshot['coins_written'] - 278)
                assert_equal(len(states), 1)
                txid, vout, spk = states[0]
                assert_equal(txid, '00' * 32 if fmt == 'hex' else bytes(32))
                assert vout >= 1_000_001
                if fmt == 'hex':
                    spk = bytes.fromhex(spk)
                assert len(spk) > 10000
                assert_equal(spk[0], 0x6a)

        spec = importlib.util.spec_from_file_location('utxo_converter', tool)
        converter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(converter)
        # Large state is valid, but must not relax ordinary script bounds.
        encoded = ser_varint(len(spk) + 6) + spk
        assert_equal(converter.decompress_script(io.BytesIO(encoded), metadata=True), spk)
        assert_raises(ValueError, converter.decompress_script, io.BytesIO(encoded))
        assert_raises(ValueError, converter.decompress_script, io.BytesIO(encoded[:-1]), metadata=True)

        assert_raises(ValueError, converter.decompress_script, io.BytesIO(ser_varint(32 * 1024 * 1024 + 7)), metadata=True)
        assert_raises(ValueError, converter.decompress_script, io.BytesIO(ser_varint(7) + b'\x51'), metadata=True)
        assert_raises(ValueError, converter.decompress_script, io.BytesIO(b'\x00'), metadata=True)

        # The record encoding is shared; exercise mainnet's distinct header
        # magic and ensure Bitcoin's namespace does not admit oversized state.
        fixture = bytearray(Path(snapshot['path']).read_bytes())
        main_snapshot = Path(self.options.tmpdir) / 'main-header.dat'
        fixture[7:11] = bytes.fromhex('47d92434')
        main_snapshot.write_bytes(fixture)
        main_db = Path(self.options.tmpdir) / 'main-header.sqlite'
        subprocess.run([sys.executable, str(tool), str(main_snapshot), str(main_db), '--txid=rawle'], check=True, capture_output=True, text=True)
        with sqlite3.connect(main_db) as con:
            assert_equal(con.execute('SELECT COUNT(*) FROM recycle_state').fetchone(), (1,))
        fixture[7:11] = bytes.fromhex('f9beb4d9')
        main_snapshot.write_bytes(fixture)
        rejected = subprocess.run([sys.executable, str(tool), str(main_snapshot), str(Path(self.options.tmpdir) / 'bitcoin-header.sqlite')], capture_output=True, text=True)
        assert rejected.returncode != 0
        assert 'too long script' in rejected.stderr

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
        with sqlite3.connect(pool_db) as con:
            assert_equal(con.execute('SELECT COUNT(*) FROM utxos').fetchone(), (0,))
            assert_equal(con.execute('SELECT COUNT(*) FROM recycle_state').fetchone(), (pool_snapshot['coins_written'],))
            assert con.execute('SELECT SUM(value) FROM recycle_state').fetchone()[0] > 0
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


if __name__ == '__main__':
    RecycleInterfacesTest(__file__).main()
