# Resatoshi

Resatoshi is an independent proof-of-work blockchain based on Bitcoin Core 31.0.
This repository contains its node, wallet, optional Qt graphical interface,
command-line tools, and tests. The node validates blocks and transactions on the
Resatoshi network, which starts from its own genesis block.

[Documentation](doc/README.md) ·
[Downloads](https://github.com/resatoshi/bitcoin-resatoshi-31.0/releases) · [Issues](https://github.com/resatoshi/bitcoin-resatoshi-31.0/issues)

## Mainnet rules

| Item | Current implementation |
| --- | --- |
| Proof of work | SHA-256d |
| Target block interval | 600 seconds |
| Difficulty adjustment | ASERT, with a two-day half-life |
| Initial block subsidy | 50 coins, halved every 210,000 blocks |
| UTXO lifetime | 5,256,000 blocks after creation |
| Recycling | Expired unspent value enters a recycle pool |
| Recycle reward | Up to one coin per block from the available pool, in addition to subsidy and fees |
| Native SegWit address prefix | `rsat1` |
| Default P2P / RPC ports | `19333` / `19332` |

Expiry is measured in blocks. An output spent in its expiry block is not
recycled. A partial recycle claim leaves the remainder in the pool; an empty
pool pays no recycle reward. Recycling redistributes existing value.
See [network rules](doc/resatoshi.md) for the genesis hash, address encodings,
and source references.

## Build and run

```sh
git clone https://github.com/resatoshi/bitcoin-resatoshi-31.0.git
cd bitcoin-resatoshi-31.0
cmake -B build
cmake --build build
```

Install the dependencies for your platform first: [Linux/Unix](doc/build-unix.md),
[Windows](doc/build-windows.md), [Windows/MSVC](doc/build-windows-msvc.md),
[macOS](doc/build-osx.md).

The executable names remain `bitcoind`, `bitcoin-cli`, and `bitcoin-qt`.
The configuration file is `bitcoin.conf`. Run the node and query it with:

```sh
build/bin/bitcoind -daemon
build/bin/bitcoin-cli getblockchaininfo
```

The Linux data directory is `~/.resatoshi`. See the [setup guide](doc/README.md)
for other platforms and peer configuration.

## Development and tests

Development takes place in this repository against `main`.
See [CONTRIBUTING.md](CONTRIBUTING.md) for patches, issue reports and translations.

```sh
ctest --test-dir build
build/test/functional/test_runner.py
```

Tests require a build with the relevant test dependencies enabled.
See the [unit test](src/test/README.md) and [functional test](test/functional/README.md)
guides. Resatoshi-specific coverage includes ASERT/mainnet mining and UTXO
recycling, expiry-block spending, wallet accounting, and reorg recovery.

## Origin and license

Resatoshi derives from Bitcoin Core 31.0 and is distributed under the
[MIT license](COPYING). Upstream copyright notices, protocol references,
third-party library documentation, and historical Bitcoin Core release notes
retain their original attribution. [Documentation conventions](doc/documentation.md)
explain the executable names and inherited technical examples.
