#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Refuse pruned recycle recovery before deleting data, including with -blocksdir."""

from hashlib import sha256
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class RecycleRecoveryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [
            ["-test=recycle", f"-recyclemode={mode}", "-prune=1", "-fastprune"]
            for mode in ("index", "index", "block", "block")
        ]

    def setup_nodes(self):
        self.external_blocks = {}
        for i in (1, 3):
            directory = Path(self.options.tmpdir) / f"external-blocks-{i}"
            directory.mkdir()
            self.extra_args[i].append(f"-blocksdir={directory}")
            self.external_blocks[i] = directory / "regtest" / "blocks"
        super().setup_nodes()

    def setup_network(self):
        # These pruned nodes are independent fixtures, not archival sync peers.
        self.setup_nodes()

    @staticmethod
    def file_hashes(directory):
        return {path.name: sha256(path.read_bytes()).hexdigest()
                for path in directory.iterdir() if path.is_file()}

    def run_test(self):
        for i, node in enumerate(self.nodes):
            self.log.info("Checking pruned recovery for node %d (%s blocks directory)",
                          i, "external" if i in self.external_blocks else "default")
            self.generatetodescriptor(node, 1100, "raw(51)", sync_fun=lambda: None)
            node.pruneblockchain(node.getblockcount() - 1)
            info = node.getblockchaininfo()
            assert info["pruneheight"] > 1
            state = node.gettxoutsetinfo("muhash")
            blocks = self.external_blocks.get(i, node.blocks_path)
            index = node.chain_path / "blocks" / "index"
            assert (index / "CURRENT").exists()
            if i in self.external_blocks:
                assert not (blocks / "index").exists()
            self.stop_node(i)
            chainstate = node.chain_path / "chainstate"
            preserved_state = self.file_hashes(chainstate)
            preserved_blocks = self.file_hashes(blocks)
            if i == 1:
                self.log.info("External blocks with missing chainstate metadata are rejected without mutation")
                saved_chainstate = node.chain_path / "chainstate.saved"
                chainstate.rename(saved_chainstate)
                preserved_index = self.file_hashes(index)
                try:
                    node.assert_start_raises_init_error(
                        extra_args=node.extra_args + ["-recyclemode=block"],
                        expected_msg="Error: Cannot verify the format of existing block history without chainstate metadata. Use -reindex, or synchronize a fresh data directory.",
                    )
                    assert not chainstate.exists()
                    assert_equal(self.file_hashes(index), preserved_index)
                    assert_equal(self.file_hashes(blocks), preserved_blocks)
                finally:
                    saved_chainstate.rename(chainstate)
            marker = node.chain_path / "recycle-corrupt"
            marker.touch()
            try:
                for explicit_reindex in (False, True):
                    node.assert_start_raises_init_error(
                        extra_args=node.extra_args + (["-reindex"] if explicit_reindex else []),
                        expected_msg="Error: Automatic recycle recovery needs all historical blocks, but this directory has pruned data. Synchronize a fresh data directory.",
                    )
                    assert marker.exists()
                    assert_equal(self.file_hashes(chainstate), preserved_state)
                    assert_equal(self.file_hashes(blocks), preserved_blocks)
                    assert (index / "CURRENT").exists()
            finally:
                # Only the recovery latch was injected; the original data is healthy.
                marker.unlink()
            self.start_node(i)
            restored = node.gettxoutsetinfo("muhash")
            for key in ("bestblock", "txouts", "total_amount", "muhash"):
                assert_equal(restored[key], state[key])
            assert_equal(node.getblockchaininfo()["pruneheight"], info["pruneheight"])


if __name__ == "__main__":
    RecycleRecoveryTest(__file__).main()
