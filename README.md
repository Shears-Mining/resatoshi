# Resatoshi

Resatoshi is an independent proof-of-work blockchain built from Bitcoin Core.
It starts from its own genesis block and uses SHA-256d mining, ASERT difficulty
adjustment, and a UTXO recycling rule defined in blocks. This repository contains
the node, wallet, optional Qt graphical interface, command-line tools, and tests.

[Documentation](doc/README.md) ·
[Mainnet rules](doc/resatoshi.md) ·
[Official Discord](https://discord.gg/QyM3eHZZS) ·
[Issues](https://gitlab.com/rst-group4210239/RST-project/-/issues)

## Project direction

Resatoshi follows Bitcoin Core's upstream development while keeping changes
specific to its own network small and understandable. Its core commitments are
a predictable development direction and a recycling cycle of approximately
100 years at the target block interval. The actual expiry rule uses block
height, so its duration varies with block production.

The same maintenance approach applies when adapting proven, Bitcoin-compatible
open-source applications: check compatibility, make the necessary changes, and
continue incorporating upstream improvements. This approach aims to reduce
maintenance costs and keep the project's technical direction clear.

The source baseline is Bitcoin Core 31.0 with selected fixes from 31.1. See
[upstream maintenance](doc/upstream-maintenance.md) for the incorporated changes
and remaining differences.

## Get started

For mining, a wallet, an explorer, and the other bundled features, join the
[official Resatoshi Discord](https://discord.gg/QyM3eHZZS) and download the
all-in-one miner from the `download-only` channel. Follow the instructions
provided with that package.

For a node or wallet built directly from this repository, use the build and
run instructions below. The [setup guide](doc/README.md) covers data directories,
configuration, and optional manual peer connections.

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

An output spent in its expiry block is not recycled. Unclaimed value stays in
the recycle pool, and an empty pool pays no recycle reward. Recycling
redistributes existing value. See [network rules](doc/resatoshi.md) for the
precise expiry behavior, genesis hash, address encodings, and source references.

## Peer discovery

Builds from the current `main` source use these mainnet seed entries:

| Hostname | Role |
| --- | --- |
| `seed.resatoshi.org` | DNS seed that crawls peers and answers service-bit queries such as `x9.seed.resatoshi.org` |
| `resatoshi-seed.freeddns.org` | Existing bootstrap node hostname |
| `resatoshi-seed.duckdns.org` | Existing bootstrap node hostname |

The existing bootstrap entries remain available. When a seed does not answer
service-bit DNS queries, the inherited networking code can connect to its base
hostname to request peer addresses. With the default networking options, fresh
nodes discover peers automatically; manual `addnode` entries are optional.

A DNS seed supplies candidate peer addresses. Each node validates the chain
using its own consensus rules. Multiple seed names alone do not establish
independent operators or independent availability. As participation grows,
Resatoshi aims to add seeds run by independent operators while preserving
Bitcoin Core's peer discovery mechanisms.

The seed list is compiled into the node. Existing binaries keep their previous
list until replaced with a build containing this change. See the
[setup guide](doc/README.md) for optional manual bootstrap settings and the
[network rules](doc/resatoshi.md) for discovery and retry behavior.

## Build and run

Install the dependencies for your platform first:
[Linux/Unix](doc/build-unix.md), [Windows](doc/build-windows.md),
[Windows/MSVC](doc/build-windows-msvc.md), or [macOS](doc/build-osx.md).

```sh
git clone https://gitlab.com/rst-group4210239/RST-project.git bitcoin-resatoshi-31.0
cd bitcoin-resatoshi-31.0
cmake -B build
cmake --build build
```

The executable names remain `bitcoind`, `bitcoin-cli`, and `bitcoin-qt`.
The configuration filename is `bitcoin.conf`. On Linux, the default data
directory is `~/.resatoshi`.

For a Linux source build:

```sh
build/bin/bitcoind -daemon
build/bin/bitcoin-cli getblockchaininfo
build/bin/bitcoin-cli getnetworkinfo
```

## Development and tests

Development takes place against `main`. See [CONTRIBUTING.md](CONTRIBUTING.md)
for patches, issue reports, and translations.

```sh
ctest --test-dir build --output-on-failure
build/test/functional/test_runner.py
```

Tests require a build with the relevant test dependencies enabled. See the
[unit test](src/test/README.md) and [functional test](test/functional/README.md)
guides. Resatoshi-specific coverage includes ASERT/mainnet mining, UTXO
recycling, expiry-block spending, wallet accounting, and reorg recovery.
Follow [release validation](doc/release-validation.md) before publishing a
binary release.

## Help and security reports

The [official Discord](https://discord.gg/QyM3eHZZS) is the question-and-help
community. Report ordinary bugs through
[GitLab Issues](https://gitlab.com/rst-group4210239/RST-project/-/issues) or email
[tpa1995@naver.com](mailto:tpa1995@naver.com).

Report security vulnerabilities privately to
[tpa1995@naver.com](mailto:tpa1995@naver.com), following [SECURITY.md](SECURITY.md).
Keep sensitive vulnerability details out of public issues and Discord channels.

## Origin and license

Resatoshi derives from Bitcoin Core and is distributed under the
[MIT license](COPYING). Upstream copyright notices, protocol references,
third-party library documentation, and historical Bitcoin Core release notes
retain their original attribution. [Documentation conventions](doc/documentation.md)
explain the executable names and inherited technical examples.
