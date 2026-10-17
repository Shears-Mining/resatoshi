# Resatoshi

Resatoshi is an independent proof-of-work blockchain built from Bitcoin Core.
It starts from its own genesis block and uses SHA-256d mining, ASERT difficulty
adjustment, and a UTXO recycling rule defined in blocks. This repository contains
the node, wallet, optional Qt graphical interface, command-line tools, and tests.

[Documentation](doc/README.md) ·
[Mainnet rules](doc/resatoshi.md) ·
[Official Discord](https://discord.gg/QyM3eHZZS) ·
[Issues](https://gitlab.com/rst-group4210239/RST-project/-/issues)

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

## Run a node

Index mode is the default and supports pruning. Block mode reads original origin
blocks and retains the expiry window. Both enforce the same rules; see
[storage and recovery](doc/recycle-state-integrity.md).

This experimental branch requires a fresh data directory; old local formats
are unsupported. Install the [platform build dependencies](doc/build-unix.md),
and create the new data directory before starting. Build the node and CLI:

```sh
cmake -S . -B build -DBUILD_GUI=OFF
cmake --build build --target bitcoind bitcoin-cli -j 2
build/bin/bitcoind -daemon -datadir=/path/to/new-resatoshi-data
build/bin/bitcoin-cli -datadir=/path/to/new-resatoshi-data getblockchaininfo true
```

[Operations](doc/node-operations.md) · [Design](doc/recycle-design.md) ·
[Integrations](doc/recycle-integrations.md) · [Upstream maintenance](doc/upstream-maintenance.md)
