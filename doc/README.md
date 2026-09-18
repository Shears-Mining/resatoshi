# Resatoshi documentation

Resatoshi runs its own proof-of-work network using a node and wallet derived
from Bitcoin Core 31.0. Start with the [project overview](../README.md),
[mainnet rules](resatoshi.md).

## Download and run

Download packages from [this repository's releases](https://github.com/resatoshi/bitcoin-resatoshi-31.0/releases).
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
and RPC port `19332`. To connect a new node to the Resatoshi bootstrap node,
add these two bootstrap hostnames to `bitcoin.conf`:

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
Questions and bug reports belong in the [Resatoshi issue tracker](https://github.com/resatoshi/bitcoin-resatoshi-31.0/issues).

## Network and implementation

- [Mainnet rules and chain identity](resatoshi.md)
- [UTXO recycling and state handling](recycle-state-integrity.md)
- [Current source notes](release-notes.md)
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
