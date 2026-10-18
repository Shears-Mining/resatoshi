#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Index and origin-block expiry agree across mining, reorg, restart and pruning."""

from hashlib import sha256

from test_framework.messages import CBlock, from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, util_xor


class RecycleModesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-test=recycle", "-recyclemode=index", "-recyclecheckinterval=1"],
            ["-test=recycle", "-recyclemode=block"],
            ["-test=recycle", "-recyclemode=block", "-prune=1", "-fastprune"],
        ]

    def check_equal(self):
        self.sync_blocks()
        states = [node.gettxoutsetinfo("muhash") for node in self.nodes]
        for state in states[1:]:
            for key in ("bestblock", "txouts", "total_amount", "muhash"):
                assert_equal(state[key], states[0][key])
        pools = [node.getblockchaininfo(True)["recycling"]["pool_balance"] for node in self.nodes]
        assert_equal(*pools)

    def run_test(self):
        indexed, archival, pruned = self.nodes
        for node, mode in zip(self.nodes, ("index", "block", "block")):
            assert_equal(node.getblockchaininfo(True)["recycling"]["mode"], mode)
            assert_equal((node.chain_path / "recycle-mode").read_text(), mode + "\n")
        assert_raises_rpc_error(-1, "disabled in block mode", archival.checkrecycleindex, "start")
        assert_raises_rpc_error(-1, "disabled in block mode", pruned.checkrecycleindex, "start")

        self.log.info("Daemon inherits its recorded block mode when no mode is supplied")
        self.restart_node(1, extra_args=["-test=recycle"])
        assert_equal(archival.getblockchaininfo(True)["recycling"]["mode"], "block")
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()

        self.generatetodescriptor(indexed, 200, "raw(51)")
        self.check_equal()
        before = indexed.gettxoutsetinfo("muhash")["muhash"]
        expiry = self.generatetodescriptor(archival, 1, "raw(51)")[0]
        self.check_equal()
        for node in self.nodes:
            node.invalidateblock(expiry)
        self.check_equal()
        assert_equal(indexed.gettxoutsetinfo("muhash")["muhash"], before)
        for node in self.nodes:
            node.reconsiderblock(expiry)
        self.check_equal()
        self.generatetodescriptor(pruned, 1, "raw(51)")
        self.check_equal()

        self.log.info("Periodic checks run independently of block mining")
        self.wait_until(lambda: "Automatic recycle index check:" in indexed.debug_log_path.read_text())
        # Flush before SIGKILL so this test isolates unclean-start checking.
        assert indexed.checkrecycleindex("start")["valid"]
        indexed.kill_process()
        assert (indexed.chain_path / "recycle-unclean").exists()
        with indexed.assert_debug_log(["Automatic recycle index check:"]):
            self.start_node(0, extra_args=["-test=recycle", "-recyclemode=index"])
        self.connect_nodes(0, 1)
        self.check_equal()

        self.log.info("Mode mismatch is rejected even with reindex requested")
        for i, mode in ((0, "block"), (1, "index")):
            self.stop_node(i)
            assert not (self.nodes[i].chain_path / "recycle-unclean").exists()
            self.nodes[i].assert_start_raises_init_error(
                extra_args=["-test=recycle", f"-recyclemode={mode}", "-reindex-chainstate"],
                expected_msg="Error: Recycle mode differs from the data directory. Use its recorded mode, or synchronize a fresh data directory.",
            )
            self.start_node(i)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()

        self.log.info("Missing format or mode records require a fresh directory")
        self.stop_node(0)
        mode_path = indexed.chain_path / "recycle-mode"
        format_path = indexed.chain_path / "recycle-format"
        mode = mode_path.read_bytes()
        storage_format = format_path.read_bytes()
        format_path.unlink()
        indexed.assert_start_raises_init_error(
            extra_args=["-test=recycle"],
            expected_msg="Error: Unsupported recycle storage format. Synchronize a fresh data directory.",
        )
        format_path.write_text("unknown\n")
        indexed.assert_start_raises_init_error(
            extra_args=["-test=recycle"],
            expected_msg="Error: Unsupported recycle storage format. Synchronize a fresh data directory.",
        )
        assert_equal(format_path.read_text(), "unknown\n")
        format_path.write_bytes(storage_format)
        mode_path.write_text("unknown\n")
        indexed.assert_start_raises_init_error(
            extra_args=["-test=recycle"],
            expected_msg="Error: Recycle mode differs from the data directory. Use its recorded mode, or synchronize a fresh data directory.",
        )
        assert_equal(mode_path.read_text(), "unknown\n")
        mode_path.unlink()
        indexed.assert_start_raises_init_error(
            extra_args=["-test=recycle", "-recyclemode=block"],
            expected_msg="Error: Recycle mode differs from the data directory. Use its recorded mode, or synchronize a fresh data directory.",
        )
        mode_path.write_bytes(mode)
        self.start_node(0)
        self.connect_nodes(0, 1)
        self.check_equal()

        self.log.info("A persistent corruption latch requires chainstate replay")
        expected_tip = archival.getbestblockhash()
        indexed.invalidateblock(expected_tip)
        self.stop_node(0)
        (indexed.chain_path / "recycle-corrupt").touch()
        with indexed.assert_debug_log(["rebuilding block index and chainstate from blocks before resuming"]):
            self.start_node(0)
        self.wait_until(lambda: not (indexed.chain_path / "recycle-corrupt").exists())
        assert_equal(indexed.getbestblockhash(), expected_tip)
        self.connect_nodes(0, 1)
        self.check_equal()

        self.log.info("Pruning preserves both expiry and the reorg window")
        self.generatetodescriptor(archival, 1100, "raw(51)")
        self.check_equal()
        height = pruned.getblockcount()
        pruned.pruneblockchain(height - 1)
        info = pruned.getblockchaininfo()
        assert info["pruneheight"] > 1
        assert info["pruneheight"] <= height - 200 - 288 + 1
        origin = pruned.getblockhash(height + 1 - 200)
        pruned.getblock(origin)
        self.restart_node(2)
        self.connect_nodes(1, 2)
        post_prune = self.generatetodescriptor(pruned, 5, "raw(51)")
        self.check_equal()
        for node in self.nodes:
            node.invalidateblock(post_prune[0])
        self.check_equal()
        for node in self.nodes:
            node.reconsiderblock(post_prune[0])
        self.check_equal()
        assert pruned.verifychain(4, 288)
        assert_equal(pruned.verifychain(4, 0), False)
        assert archival.verifychain(4, 0)
        assert indexed.checkrecycleindex("start")["valid"]

        self.log.info("Block mode authenticates origin transactions against the header")
        source_hash = archival.getblockhash(archival.getblockcount() + 1 - 200)
        raw = bytes.fromhex(archival.getblock(source_hash, 0))
        altered = from_hex(CBlock(), raw.hex())
        altered.vtx[0].vout[0].nValue += 1
        damaged = altered.serialize()
        assert_equal(len(damaged), len(raw))
        key = archival.read_xor_key()
        self.stop_node(1)
        matches = []
        for path in archival.blocks_path.glob("blk*.dat"):
            original = path.read_bytes()
            decoded = util_xor(original, key, offset=0)
            offset = decoded.find(raw)
            if offset >= 0:
                matches.append((path, original, decoded, offset))
        assert_equal(len(matches), 1)
        path, original, decoded, offset = matches[0]
        try:
            path.write_bytes(util_xor(decoded[:offset] + damaged + decoded[offset + len(raw):], key, offset=0))
            self.start_node(1)
            before = archival.getbestblockhash()
            assert_raises_rpc_error(None, "Cannot read recycle expiry origin block", archival.generatetodescriptor, 1, "raw(51)", called_by_framework=True)
            assert_equal(archival.getbestblockhash(), before)
        finally:
            if archival.running:
                self.stop_node(1)
            path.write_bytes(original)
        self.start_node(1)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()

        self.log.info("Invalid mode records cannot fall back to legacy index mode")
        self.stop_node(0)
        mode_path = indexed.chain_path / "recycle-mode"
        saved_mode = mode_path.read_bytes()
        try:
            for contents in (b"", b"\n", b"unknown\n", b"index \n"):
                mode_path.write_bytes(contents)
                indexed.assert_start_raises_init_error(
                    expected_msg="Error: Recycle mode differs from the data directory. Use its recorded mode, or synchronize a fresh data directory.",
                )
                assert_equal(mode_path.read_bytes(), contents)
        finally:
            mode_path.write_bytes(saved_mode)
        self.start_node(0)
        self.connect_nodes(0, 1)
        self.check_equal()

        self.log.info("Pruned recovery is refused before wiping chainstate")
        self.stop_node(2)
        database = pruned.chain_path / "chainstate"
        preserved = {path.name: sha256(path.read_bytes()).hexdigest() for path in database.iterdir() if path.is_file()}
        marker = pruned.chain_path / "recycle-corrupt"
        marker.touch()
        try:
            pruned.assert_start_raises_init_error(
                expected_msg="Error: Automatic recycle recovery needs all historical blocks, but this directory has pruned data. Synchronize a fresh data directory.",
            )
            assert_equal({path.name: sha256(path.read_bytes()).hexdigest() for path in database.iterdir() if path.is_file()}, preserved)
        finally:
            # This latch was injected without actual damage, solely to test refusal.
            marker.unlink()
        self.start_node(2)
        self.connect_nodes(1, 2)
        self.check_equal()


if __name__ == "__main__":
    RecycleModesTest(__file__).main()
