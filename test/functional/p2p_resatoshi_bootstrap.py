#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A fresh mainnet node automatically attempts the documented bootstrap hosts.

Capture all connections in a local SOCKS proxy; never contact public peers.
"""

import queue
import time

from mining_mainnet import BLOCK1_NONCE, BLOCK1_TIME, COINBASE_SCRIPT_PUBKEY, INITIAL_N_BITS
from test_framework.blocktools import create_coinbase
from test_framework.messages import CBlock, SEQUENCE_FINAL
from test_framework.p2p import P2PInterface
from test_framework.socks5 import AddressType, Socks5Command, Socks5Configuration, Socks5Server
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port


class ResatoshiBootstrapTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--testnet4", action="store_true", help="Check inherited seed behavior")
        parser.add_argument("--stale-inbound", action="store_true", help="Recover with stale addresses and inbound-only peers")
        parser.add_argument("--inactive", action="store_true", help="Check network reactivation starts a fresh recovery window")

    def set_test_params(self):
        self.num_nodes = 1 if self.options.testnet4 else 4
        self.setup_clean_chain = True
        self.disable_autoconnect = False
        self.chain = "testnet4" if self.options.testnet4 else ""

    def setup_nodes(self):
        conf = Socks5Configuration()
        conf.addr = ("127.0.0.1", 0)
        conf.unauth = True
        self.proxy = Socks5Server(conf)
        self.proxy.start()
        self.disabled_proxies = []
        if self.options.testnet4:
            self.extra_args = [[f"-proxy=127.0.0.1:{conf.addr[1]}", "-dnsseed=1", "-v2transport=0", "-fixedseeds=0", "-prune=550"]]
            super().setup_nodes()
            return
        for _ in range(2):
            disabled_conf = Socks5Configuration()
            disabled_conf.addr = ("127.0.0.1", 0)
            disabled_conf.unauth = True
            proxy = Socks5Server(disabled_conf)
            proxy.start()
            self.disabled_proxies.append(proxy)
        self.extra_args = [
            [f"-proxy=127.0.0.1:{conf.addr[1]}", "-dnsseed=1", "-v2transport=0", "-minimumchainwork=0", "-maxtipage=999999999"],
            ["-connect=0", "-dnsseed=0", "-v2transport=0", "-minimumchainwork=0", "-maxtipage=999999999"],
        ]
        self.extra_args.extend([
            [f"-proxy=127.0.0.1:{self.disabled_proxies[0].conf.addr[1]}", "-dnsseed=0", "-v2transport=0"],
            [f"-proxy=127.0.0.1:{self.disabled_proxies[1].conf.addr[1]}", "-dnsseed=1", "-connect=0", "-v2transport=0"],
        ])
        if self.options.inactive:
            self.extra_args[0].append("-maxconnections=1")
        super().setup_nodes()

    def setup_network(self):
        self.setup_nodes()

    def run_test(self):
        try:
            if self.options.testnet4:
                self.check_inherited_seeds()
                return
            mainnet_seeds = {"seed.resatoshi.org", "seed-fra.resatoshi.org", "resatoshi-seed.freeddns.org", "resatoshi-seed.duckdns.org"}
            pending = mainnet_seeds.copy()
            while pending:
                command = self.proxy.queue.get(timeout=30 * self.options.timeout_factor)
                assert isinstance(command, Socks5Command), command
                assert_equal(command.atyp, AddressType.DOMAINNAME)
                assert_equal(command.port, 19333)
                host = command.addr.decode("ascii")
                assert host in pending, host
                pending.remove(host)
            node, seed = self.nodes[:2]
            assert_equal(node.getblockcount(), 0)
            self.wait_until(lambda: node.getconnectioncount() == 0)
            if self.options.stale_inbound:
                assert node.addpeeraddress("44.1.1.1", 19333)["success"]
                # This inbound peer provides no blocks or addresses. Do not
                # advertise block service and make it the initial header source.
                node.add_p2p_connection(P2PInterface(), services=0)
                assert all(peer["inbound"] for peer in node.getpeerinfo())
                assert_equal(len(node.getnodeaddresses(0)), 1)
            if self.options.inactive:
                node.setnetworkactive(False)
                with node.assert_debug_log([], unexpected_msgs=["Retrying DNS seed address fetches"]):
                    time.sleep(65)
                assert_equal(node.getconnectioncount(), 0)
            block = CBlock()
            block.nVersion = 0x20000000
            block.hashPrevBlock = int(seed.getbestblockhash(), 16)
            block.nTime = BLOCK1_TIME
            block.nBits = INITIAL_N_BITS
            block.nNonce = BLOCK1_NONCE
            block.vtx = [create_coinbase(1, script_pubkey=bytes.fromhex(COINBASE_SCRIPT_PUBKEY), halving_period=210000)]
            block.vtx[0].nLockTime = 0
            block.vtx[0].vin[0].nSequence = SEQUENCE_FINAL
            block.hashMerkleRoot = block.calc_merkle_root()
            assert_equal(seed.submitblock(block.serialize().hex()), None)
            for suffix in range(2, 102):
                seed.addpeeraddress(f"44.1.1.{suffix}", 19333)
            # All destinations stay local, including addresses returned by the
            # seed. Recover without restarting or manually connecting node 0.
            self.proxy.conf.destinations_factory = lambda host, port, client: None if host == "44.1.1.1" else {
                "actual_to_addr": "127.0.0.1", "actual_to_port": p2p_port(1),
            }
            resumed = time.monotonic()
            if self.options.inactive:
                node.setnetworkactive(True)
            self.log.info("Wait for backoff retry, address discovery and block sync")
            deadline = time.monotonic() + 90 * self.options.timeout_factor
            while True:
                command = self.proxy.queue.get(timeout=max(0.01, deadline - time.monotonic()))
                assert isinstance(command, Socks5Command), command
                if command.atyp == AddressType.DOMAINNAME and command.addr.decode("ascii") in mainnet_seeds:
                    break
                assert time.monotonic() < deadline
            if self.options.inactive:
                assert time.monotonic() - resumed >= 55
            self.wait_until(lambda: len(node.getnodeaddresses(0)) > 0)
            self.wait_until(lambda: node.getbestblockhash() == block.hash_hex)
            assert any(peer["connection_type"] == "outbound-full-relay" for peer in node.getpeerinfo())
            if self.options.stale_inbound:
                # Cross the next 120-second retry deadline after recovery.
                self.log.info("Check established outbound connectivity suppresses further retries")
                with node.assert_debug_log([], unexpected_msgs=["Retrying DNS seed address fetches"]):
                    time.sleep(125)
                assert any(peer["connection_type"] == "outbound-full-relay" for peer in node.getpeerinfo())
            if self.options.inactive:
                # Fill the sole outbound slot beyond the retry window, then
                # lose it. A blocked connection thread must not retain an old
                # deadline and retry immediately when the slot becomes free.
                self.log.info("Check loss of a saturated outbound slot starts a fresh recovery window")
                with node.assert_debug_log([], unexpected_msgs=["Retrying DNS seed address fetches"]):
                    time.sleep(65)
                self.proxy.conf.destinations_factory = None
                peer = next(p for p in node.getpeerinfo() if p["connection_type"] == "outbound-full-relay")
                lost = time.monotonic()
                with node.assert_debug_log(["Retrying DNS seed address fetches"], timeout=90):
                    node.disconnectnode(nodeid=peer["id"])
                assert time.monotonic() - lost >= 55
            # These nodes ran past the first retry deadline too. Neither an
            # explicit dnsseed=0 nor connect=0 may be bypassed by retry logic.
            for proxy in self.disabled_proxies:
                assert proxy.queue.empty()
            for disabled in self.nodes[2:]:
                assert_equal(disabled.getconnectioncount(), 0)
        finally:
            self.stop_nodes()
            self.proxy.stop()
            for proxy in self.disabled_proxies:
                proxy.stop()

    def check_inherited_seeds(self):
        pending = {"seed.testnet4.bitcoin.sprovoost.nl.", "seed.testnet4.wiz.biz."}
        while pending:
            command = self.proxy.queue.get(timeout=30 * self.options.timeout_factor)
            assert isinstance(command, Socks5Command), command
            assert_equal(command.atyp, AddressType.DOMAINNAME)
            assert_equal(command.port, 48333)
            host = command.addr.decode("ascii")
            assert host in pending, host
            pending.remove(host)
        self.log.info("Check testnet4 does not acquire the mainnet seed retry")
        try:
            command = self.proxy.queue.get(timeout=65 * self.options.timeout_factor)
        except queue.Empty:
            pass
        else:
            raise AssertionError(f"Unexpected inherited-network seed retry: {command}")
        assert_equal(self.nodes[0].getconnectioncount(), 0)
        assert_equal(self.nodes[0].getnodeaddresses(0), [])


if __name__ == "__main__":
    ResatoshiBootstrapTest(__file__).main()
