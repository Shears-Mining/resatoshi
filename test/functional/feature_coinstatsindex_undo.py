#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reject inconsistent undo before updating coin statistics."""

from test_framework.messages import MAGIC_BYTES, hash256
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, util_xor
from test_framework.wallet import MiniWallet


class CoinStatsUndoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Let the index inspect the damaged record instead of startup VerifyDB.
        self.extra_args = [['-checklevel=0', '-coinstatsindex=0']]
        if self.options.recycling:
            self.extra_args[0].append('-test=recycle')

    def add_options(self, parser):
        parser.add_argument('--recycling', action='store_true')

    def check_bad_undo(self, blockhash, previous_hash, damaged_payloads, prefix=b'\x01\x01'):
        node = self.nodes[0]  # Stopped; only alter the final undo record.
        path = node.blocks_path / 'rev00000.dat'
        original = path.read_bytes()
        key = node.read_xor_key()
        decoded = util_xor(original, key, offset=0)
        previous = bytes.fromhex(previous_hash)[::-1]
        offset = 0
        matches = []
        while decoded[offset:offset + 4] == MAGIC_BYTES['regtest']:
            start = offset + 8
            size = int.from_bytes(decoded[offset + 4:start], 'little')
            end = start + size
            if hash256(previous + decoded[start:end]) == decoded[end:end + 32]:
                matches.append((start, end))
            offset = end + 32
        assert_equal(len(matches), 1)
        start, end = matches[0]
        assert_equal(end + 32, offset)
        payload = decoded[start:end]
        assert_equal(payload[:2], prefix)
        try:
            for damaged in damaged_payloads(payload):
                # A valid checksum ensures the index must check the contents.
                replacement = len(damaged).to_bytes(4, 'little') + damaged + hash256(previous + damaged)
                changed = decoded[:start - 4] + replacement + decoded[end + 32:]
                path.write_bytes(util_xor(changed, key, offset=0))
                with node.assert_debug_log([f'Failed to write block {blockhash} to index database']):
                    node.assert_start_raises_init_error(
                        extra_args=self.extra_args[0] + ['-coinstatsindex'],
                    )
        finally:
            if node.running:
                self.stop_node(0)
            path.write_bytes(original)

    def check_valid_undo(self, expected):
        node = self.nodes[0]
        self.start_node(0, extra_args=self.extra_args[0] + ['-coinstatsindex'])
        self.wait_until(lambda: node.getindexinfo()['coinstatsindex']['synced'])
        indexed = node.gettxoutsetinfo('muhash')
        scanned = node.gettxoutsetinfo('muhash', use_index=False)
        for field in ('height', 'bestblock', 'txouts', 'total_amount', 'muhash'):
            assert_equal(indexed[field], scanned[field], expected[field])
        assert node.verifychain(4, 0)

    def run_test(self):
        node = self.nodes[0]
        wallet = MiniWallet(node)
        self.generate(wallet, 101)
        wallet.send_self_transfer(from_node=node)
        previous_hash = node.getbestblockhash()
        blockhash = self.generate(wallet, 1)[0]
        expected = node.gettxoutsetinfo('muhash', use_index=False)
        self.stop_node(0)

        self.log.info('Reject missing/extra transaction and input undo records')
        self.check_bad_undo(blockhash, previous_hash, lambda payload: (
            b'\x00\x00',                     # Missing transaction undo.
            b'\x02' + payload[1:] + b'\x00',  # Extra transaction undo.
            b'\x01\x00\x00',                 # Missing input undo.
            b'\x01\x02' + payload[2:-1] * 2 + b'\x00',    # Extra input undo.
        ))
        self.check_valid_undo(expected)

        if self.options.recycling:
            # Leave the statistics index behind while crossing first expiry.
            self.restart_node(0, extra_args=self.extra_args[0])
            self.generate(wallet, 99)
            previous_hash = node.getbestblockhash()
            blockhash = self.generate(wallet, 1)[0]
            expected = node.gettxoutsetinfo('muhash', use_index=False)
            self.stop_node(0)
            self.log.info('Reject missing/extra typed expiry undo and invalid pool')
            self.check_bad_undo(blockhash, previous_hash, lambda payload: (
                b'\x00\x00',  # Missing required expiry state.
                b'\x01\x00' + payload[1:],  # Extra transaction undo.
                payload[:2] + (-1).to_bytes(8, 'little', signed=True) + payload[10:],
            ), prefix=b'\x00\x01')
            self.check_valid_undo(expected)


if __name__ == '__main__':
    CoinStatsUndoTest(__file__).main()
