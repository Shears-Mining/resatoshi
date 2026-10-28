#!/usr/bin/env python3
# Copyright (c) 2026 The Resatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Mine and relay across storage versions, including competing expiry branches.

Every block is mined locally and exchanged by the nodes' real P2P connections.
No block is submitted to the receiving node through RPC. A reference binary,
when supplied, always keeps its own data directory and local storage format.
"""

from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import json
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class RecycleMixedVersionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [["-test=recycle", "-coinstatsindex", "-checklevel=4", "-checkblocks=0", "-acceptnonstdtxn=0"] for _ in range(2)]

    def add_options(self, parser):
        parser.add_argument("--reference-bitcoind", help="Main daemon; uses a separate, fresh directory")
        parser.add_argument("--candidate-mode", choices=("index", "block"), default="index")
        parser.add_argument("--reference-mode", choices=("index", "block"), default="index")
        parser.add_argument("--mixed-results")

    def setup_network(self):
        for args, mode in zip(self.extra_args, (self.options.candidate_mode, self.options.reference_mode)):
            args.append(f"-recyclemode={mode}")
        self.add_nodes(2, extra_args=self.extra_args)
        if self.options.reference_bitcoind:
            self.nodes[1].args[0] = str(Path(self.options.reference_bitcoind).resolve())
            self.nodes[1].replace_in_config([("prevoutfetchthreads=1\n", "")])
        self.start_nodes()
        self.connect_nodes(0, 1)
        self.sync_all()

    def peers(self):
        observed = []
        for i, node in enumerate(self.nodes):
            peers = node.getpeerinfo()
            assert_equal(len(peers), 1)
            assert_equal(peers[0]["subver"], self.nodes[1 - i].getnetworkinfo()["subversion"])
            assert_equal(peers[0]["transport_protocol_type"], "v2" if self.options.v2transport else "v1")
            observed.append({k: peers[0][k] for k in ("subver", "version", "transport_protocol_type")})
        return observed

    def state(self, label, pool=None):
        self.sync_all()
        states = []
        for node in self.nodes:
            self.wait_until(lambda: node.getindexinfo()["coinstatsindex"]["synced"] and
                            node.getindexinfo()["coinstatsindex"]["best_block_height"] == node.getblockcount())
            scanned = node.gettxoutsetinfo("muhash", use_index=False)
            indexed = node.gettxoutsetinfo("muhash")
            fields = ("height", "bestblock", "txouts", "total_amount", "muhash")
            assert_equal([scanned[k] for k in fields], [indexed[k] for k in fields])
            balance = node.getblockchaininfo(True)["recycling"]["pool_balance"]
            if pool is not None:
                assert_equal(balance, Decimal(pool))
            states.append({**{k: scanned[k] for k in fields}, "pool_balance": balance})
        assert_equal(*states)
        self.checkpoints.append({"label": label, **states[0]})
        return states[0]

    def mine(self, i, count, descriptor="raw(6a)"):
        hashes = self.generatetodescriptor(self.nodes[i], count, descriptor)
        self.mined[i] += count
        assert_equal(self.nodes[1 - i].getbestblockhash(), hashes[-1])
        return hashes

    def restart_pair(self):
        for i in range(2):
            self.restart_node(i)
        self.connect_nodes(0, 1)
        self.sync_all()
        self.peers()
        for node in self.nodes:
            assert node.verifychain(4, 0)

    def run_test(self):
        self.mined = [0, 0]
        self.checkpoints = []
        self.transfers = []
        peer_versions = self.peers()
        candidate, reference = self.nodes
        wallets = [MiniWallet(candidate, tag_name="candidate-miner"), MiniWallet(reference, tag_name="reference-miner")]
        descriptors = [wallet.get_descriptor() for wallet in wallets]

        self.log.info("Both versions mine and relay original funding blocks")
        funding_hashes = [self.mine(0, 1, descriptors[0])[0], self.mine(1, 1, descriptors[1])[0]]
        self.mine(0, 1, descriptors[0])  # An unspent output to expire on both branches at 203.
        self.mine(0, 98)
        self.mine(1, 99)
        assert_equal(candidate.getblockcount(), 200)
        self.state("before first expiry", pool=0)

        self.log.info("Standard transactions relay in both directions")
        for i, wallet in enumerate(wallets):
            wallet.rescan_utxos()
            txid = self.nodes[i].getblock(funding_hashes[i], 2)["tx"][0]["txid"]
            utxo = wallet.get_utxo(txid=txid, vout=0)
            tx = wallet.send_self_transfer(from_node=self.nodes[i], utxo_to_spend=utxo, fee=Decimal("0.001") * (i + 1))
            self.sync_all()
            assert tx["txid"] in self.nodes[1 - i].getrawmempool()
            self.transfers.append(tx)

        self.log.info("Partition peers and mine competing height-201 blocks concurrently")
        self.disconnect_nodes(0, 1)
        assert_equal([node.getpeerinfo() for node in self.nodes], [[], []])
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(node.generateblock, descriptors[i], [self.transfers[i]["hex"]], called_by_framework=True)
                       for i, node in enumerate(self.nodes)]
            competing = [future.result()["hash"] for future in futures]
        self.mined = [value + 1 for value in self.mined]
        assert competing[0] != competing[1]
        for i, node in enumerate(self.nodes):
            assert_equal(node.getblockcount(), 201)
            assert_equal(node.getblock(competing[i])["tx"][1:], [self.transfers[i]["txid"]])
        assert_equal(candidate.getblockstats(competing[0])["expired_outputs"], 0)
        assert_equal(reference.getblockstats(competing[1])["expired_outputs"], 1)
        assert_equal(candidate.getblockchaininfo(True)["recycling"]["pool_balance"], 0)
        # generateblock omits fees: the 0.002 fee reduces the pool reward claimed.
        assert_equal(reference.getblockchaininfo(True)["recycling"]["pool_balance"], Decimal("49.002"))
        for _ in range(2):
            self.generateblock(reference, "raw(6a)", [], sync_fun=self.no_op)
            self.mined[1] += 1
        assert_equal(reference.getblockchaininfo(True)["recycling"]["pool_balance"], Decimal("97.002"))
        reference_tip = reference.getbestblockhash()

        self.log.info("Reconnect: the new version reorganizes to the main miner's longer branch")
        self.connect_nodes(0, 1)
        self.state("main branch wins across expiry", pool="97.002")
        assert_equal(candidate.getbestblockhash(), reference_tip)
        assert_equal(candidate.gettxout(self.transfers[0]["txid"], 0), None)
        assert_equal(candidate.getrawmempool(), [])

        self.log.info("Reverse the winning branch: main must undo expiry and accept the new miner's chain")
        self.disconnect_nodes(0, 1)
        candidate.invalidateblock(competing[1])
        assert_equal(candidate.getbestblockhash(), competing[0])
        # Spend the main miner's original coin in its last valid block, height 202.
        self.generateblock(candidate, descriptors[1], [self.transfers[1]["hex"]], sync_fun=self.no_op)
        self.mined[0] += 1
        for _ in range(3):
            self.generateblock(candidate, "raw(6a)", [], sync_fun=self.no_op)
            self.mined[0] += 1
        candidate_tip = candidate.getbestblockhash()
        assert_equal(candidate.getblockcount(), 205)
        candidate.reconsiderblock(competing[1])
        assert_equal(candidate.getbestblockhash(), candidate_tip)
        self.connect_nodes(0, 1)
        self.state("new branch wins and restores the expiry-block spend", pool=47)
        assert_equal(reference.getbestblockhash(), candidate_tip)
        assert_equal(reference.getblock(competing[0])["confirmations"], 5)
        assert_equal(reference.gettxout(self.transfers[0]["txid"], 0)["value"], Decimal("49.999"))
        self.restart_pair()
        self.state("both formats reopen after reorganization", pool=47)

        self.log.info("Alternate real mining until renewed outputs expire")
        turn = 1
        while candidate.getblockcount() < 400:
            self.mine(turn, min(10, 400 - candidate.getblockcount()))
            self.state(f"alternating mining at {candidate.getblockcount()}")
            turn = 1 - turn
        for height in range(401, 406):
            miner = (height - 401) % 2
            blockhash = self.mine(miner, 1)[0]
            for node in self.nodes:
                assert_equal(node.getblockstats(blockhash)["expired_outputs"], 2 if height in (401, 402) else 0)
            self.state(f"renewed lifetime boundary {height}")
        # Both explicit-transaction blocks omit 0.003 in fees from their coinbases;
        # those coinbases and the renewed outputs now expire together.
        final = self.state("empty UTXO set with a nonzero pool", pool="144.997")
        assert_equal(final["txouts"], 0)
        assert_equal(final["total_amount"], 0)
        self.restart_pair()
        assert_equal(self.state("pool-only state survives restart", pool="144.997"), final)
        result = {
            "reference_binary": self.options.reference_bitcoind,
            "candidate_mode": self.options.candidate_mode,
            "reference_mode": self.options.reference_mode,
            "transport": "v2" if self.options.v2transport else "v1",
            "peers": peer_versions,
            "locally_mined_blocks": self.mined,
            "concurrent_competing_blocks": competing,
            "standard_transactions_relayed_both_directions": True,
            "reorganizations_across_first_expiry": 2,
            "restart_rounds": 2,
            "rpc_block_submission_to_peer": False,
            "checkpoints": self.checkpoints,
        }
        if self.options.mixed_results:
            Path(self.options.mixed_results).write_text(json.dumps(result, indent=2, default=str) + "\n")


if __name__ == "__main__":
    RecycleMixedVersionsTest(__file__).main()
