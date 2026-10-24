#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Index and origin-block expiry agree across mining, reorg, restart and pruning."""

from hashlib import sha256
import json

from test_framework.messages import CBlock, from_hex
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, util_xor


class RecycleModesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 3
        self.setup_clean_chain = True
        self.extra_args = [
            ["-test=recycle", "-recyclemode=index", "-recyclecheckinterval=1"],
            ["-test=recycle"],  # Fresh data defaults to block mode.
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
            assert not (node.chain_path / "recycle-mode").exists()
            assert not (node.chain_path / "recycle-format").exists()
        assert_raises_rpc_error(-1, "disabled in block mode", archival.checkrecycleindex, "start")
        assert_raises_rpc_error(-1, "disabled in block mode", pruned.checkrecycleindex, "start")

        self.log.info("Existing DB modes survive missing settings and conflicting GUI preferences")
        for i, mode in ((0, "index"), (1, "block")):
            node = self.nodes[i]
            self.stop_node(i)
            settings = node.chain_path / "settings.json"
            settings.write_text(json.dumps({"recyclemode": "block" if mode == "index" else "index"}))
            for extra in ([], ["-nosettings"], ["-reindex-chainstate"], ["-reindex"]):
                self.start_node(i, extra_args=["-test=recycle", "-recyclecheckinterval=1", *extra])
                assert_equal(node.getblockchaininfo(True)["recycling"]["mode"], mode)
                self.stop_node(i)
            settings.write_text(json.dumps({"recyclemode": "unknown"}))
            self.start_node(i, extra_args=["-test=recycle", "-recyclecheckinterval=1"])
            assert_equal(node.getblockchaininfo(True)["recycling"]["mode"], mode)
            self.stop_node(i)
            node.assert_start_raises_init_error(
                extra_args=["-test=recycle", "-recyclemode=unknown"],
                expected_msg="Error: Invalid -recyclemode; use index or block",
            )
            settings.unlink(missing_ok=True)
            self.start_node(i, extra_args=["-test=recycle", "-recyclecheckinterval=1"])
            assert_equal(node.getblockchaininfo(True)["recycling"]["mode"], mode)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()

        self.log.info("Daemon inherits its recorded block mode when no mode is supplied")
        self.stop_node(1)
        settings_path = archival.chain_path / "settings.json"
        settings_path.write_text(json.dumps({"recyclemode": "block"}))
        self.start_node(1, extra_args=["-test=recycle"])
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
                expected_msg=f"Error: This chainstate uses recycle mode {'index' if i == 0 else 'block'}. "
                f"Start with -recyclemode={'index' if i == 0 else 'block'}, or use -reindex to rebuild in another mode.",
            )
            assert not (self.nodes[i].chain_path / "recycle-unclean").exists()
            self.start_node(i)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()

        self.log.info("Missing or disabled GUI settings preserve the recorded block mode")
        self.stop_node(1)
        for extra, missing_settings in (
            ([], True),
            (["-reindex-chainstate"], True),
            (["-nosettings"], False),
            (["-settings=missing.json"], False),
            (["-reindex-chainstate", "-nosettings"], False),
        ):
            if missing_settings:
                settings_path.rename(settings_path.with_suffix(".saved"))
            self.start_node(1, extra_args=["-test=recycle", *extra])
            assert_equal(archival.getblockchaininfo(True)["recycling"]["mode"], "block")
            self.stop_node(1)
            if missing_settings:
                settings_path.with_suffix(".saved").replace(settings_path)
        (archival.chain_path / "custom-mode.json").write_text(json.dumps({"recyclemode": "block"}))
        self.start_node(1, extra_args=["-test=recycle", "-settings=custom-mode.json"])
        assert_equal(archival.getblockchaininfo(True)["recycling"]["mode"], "block")
        self.stop_node(1)
        latch = archival.chain_path / "recycle-corrupt"
        latch.touch()
        try:
            self.start_node(1, extra_args=["-test=recycle", "-nosettings"])
            assert_equal(archival.getblockchaininfo(True)["recycling"]["mode"], "block")
            self.wait_until(lambda: not latch.exists())
            self.stop_node(1)
        finally:
            latch.unlink(missing_ok=True)
        self.start_node(1, extra_args=["-test=recycle", "-nosettings", "-recyclemode=block", "-reindex-chainstate"])
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()
        assert archival.verifychain(4, 0)

        self.log.info("Explicit configuration overrides saved mode preferences")
        self.stop_node(1)
        config = archival.bitcoinconf.read_text()
        try:
            with archival.bitcoinconf.open("a") as stream:
                stream.write("\nrecyclemode=index\n")
            archival.assert_start_raises_init_error(
                extra_args=["-test=recycle"],
                expected_msg="Error: This chainstate uses recycle mode block. Start with -recyclemode=block, or use -reindex to rebuild in another mode.",
            )
            self.start_node(1, extra_args=["-test=recycle", "-recyclemode=block"])
            self.stop_node(1)
        finally:
            archival.bitcoinconf.write_text(config)

        self.log.info("Full reindex can change modes and regenerates usable undo")
        for mode in ("index", "block"):
            previous_mode = "block" if mode == "index" else "index"
            options = [f"-recyclemode={mode}"]
            with archival.assert_debug_log([f"Rebuilding in {mode} mode; previous chainstate was {previous_mode} mode"]):
                self.start_node(1, extra_args=["-test=recycle", "-reindex", *options])
            self.connect_nodes(0, 1)
            self.connect_nodes(1, 2)
            self.check_equal()
            assert_equal(archival.getblockchaininfo(True)["recycling"]["mode"], mode)
            assert archival.verifychain(4, 0)
            self.stop_node(1)
        self.start_node(1)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        self.check_equal()

        self.log.info("Missing chainstate metadata requires full reindex, not chainstate-only rebuild")
        self.stop_node(0)
        database = indexed.chain_path / "chainstate"
        saved_database = indexed.chain_path / "saved-chainstate"
        database.rename(saved_database)
        for extra in ([], ["-reindex-chainstate"]):
            indexed.assert_start_raises_init_error(
                extra_args=["-test=recycle", *extra],
                expected_msg="Error: Cannot verify the format of existing block history without chainstate metadata. Use -reindex, or synchronize a fresh data directory.",
            )
            assert not database.exists()
        indexed.assert_start_raises_init_error(
            extra_args=["-test=recycle", "-reindex"],
            expected_msg="Error: Cannot determine the existing recycle mode. Specify -recyclemode=index or -recyclemode=block with -reindex, or synchronize a fresh data directory.",
        )
        self.start_node(0, extra_args=["-test=recycle", "-recyclemode=index", "-reindex"])
        self.connect_nodes(0, 1)
        self.check_equal()
        assert indexed.verifychain(4, 0)

        self.log.info("Unreadable old metadata cannot prevent an explicit full reindex")
        self.stop_node(0)
        (database / "CURRENT").write_text("missing-manifest\n")
        indexed.assert_start_raises_init_error(
            extra_args=["-test=recycle", "-reindex"],
            expected_msg="Error: Cannot determine the existing recycle mode. Specify -recyclemode=index or -recyclemode=block with -reindex, or synchronize a fresh data directory.",
        )
        with indexed.assert_debug_log(["Cannot read previous recycle mode; continuing full reindex"]):
            self.start_node(0, extra_args=["-test=recycle", "-recyclemode=index", "-reindex"])
        self.connect_nodes(0, 1)
        self.check_equal()
        assert indexed.verifychain(4, 0)

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
