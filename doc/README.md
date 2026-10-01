# Resatoshi documentation

Resatoshi runs its own proof-of-work network using a node and wallet derived
from Bitcoin Core. Start with the [project overview](../README.md),
[mainnet rules](resatoshi.md).

## Download and run

For mining, a wallet, an explorer, and the other bundled features, join the
[official Resatoshi Discord](https://discord.gg/QyM3eHZZS) and download the
all-in-one miner from the `download-only` channel. Follow the instructions
provided with that package to install and run it.

For the node and wallet built from this repository's source, the following
program names and configuration details apply.
The program names are `bitcoind` (node), `bitcoin-cli` (RPC client), and
`bitcoin-qt` (graphical wallet). On Windows, run `bitcoin-qt.exe` from the
installed package. Build-dependent tools also include the `bitcoin` wrapper;
use `bitcoin help` to list its available subcommands.

| Platform | Default data directory |
| --- | --- |
| Linux / Unix | `~/.resatoshi` |
| macOS | `~/Library/Application Support/ReSatoshi` |
| Windows | `%LOCALAPPDATA%\ReSatoshi` |

The configuration filename is `bitcoin.conf`. Mainnet uses P2P port `19333`
and RPC port `19332`. Builds from the current `main` source automatically use
`seed.resatoshi.org` alongside the existing `resatoshi-seed.freeddns.org` and
`resatoshi-seed.duckdns.org` bootstrap entries. The DNS crawler answers
service-bit queries such as `x9.seed.resatoshi.org`; existing bootstrap hosts
remain available through the upstream DNS-seed/address-fetch mechanism.
No manual peer setting is required with the default networking options.
These names do not establish independent operators or availability. The seed
list is compiled into the node; existing binaries retain their previous list
until replaced with a build containing the new seed.

For a persistent manual connection, or when automatic seeding is disabled,
these optional entries can be added to `bitcoin.conf`:

```ini
addnode=resatoshi-seed.freeddns.org:19333
addnode=resatoshi-seed.duckdns.org:19333
```

For a Linux source build:

```sh
build/bin/bitcoind -daemon
build/bin/bitcoin-cli getblockchaininfo
build/bin/bitcoin-cli getnetworkinfo
```

See [configuration](bitcoin-conf.md), [data files](files.md), and
[documentation conventions](documentation.md) for details.
For general questions and help, join the
[official Resatoshi Discord question-and-help community](https://discord.gg/QyM3eHZZS).
Report ordinary bugs through the [Resatoshi issue tracker](https://gitlab.com/rst-group4210239/RST-project/-/issues)
or email [tpa1995@naver.com](mailto:tpa1995@naver.com).
Send security vulnerability reports privately to that email address; see
[security reporting](../SECURITY.md). Do not post sensitive vulnerability
details in public issues or Discord channels.

## Network and implementation

- [Mainnet rules and chain identity](resatoshi.md)
- [UTXO recycling and state handling](recycle-state-integrity.md)
- [Explorer, wallet and snapshot integrations](recycle-integrations.md)
- [Current source notes](release-notes.md)
- [Current upstream integration](upstream-maintenance.md)
- [Security reporting](../SECURITY.md)

Building
---------------------
The following are developer notes on how to build Resatoshi on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows-msvc.md)
- [FreeBSD Build Notes](build-freebsd.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)

Development
---------------------
The Resatoshi repository's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Process](release-process.md)
- [Upstream Bitcoin Core Source Documentation (External Link)](https://doxygen.bitcoincore.org/)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Inherited BIP Implementation History](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)
- [Internal Design Docs](design/)

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [bitcoin.conf Configuration File](bitcoin-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [Managing Wallets](managing-wallets.md)
- [Multisig Tutorial](multisig-tutorial.md)
- [Offline Signing Tutorial](offline-signing-tutorial.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Transaction Relay Policy](policy/README.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
