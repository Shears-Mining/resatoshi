# Resatoshi network rules

Resatoshi is an independent SHA-256d proof-of-work chain based on Bitcoin Core.
The rules below are shared by index and block expiry modes.

## Mainnet

| Rule | Value |
| --- | --- |
| Target block interval | 600 seconds |
| Difficulty | ASERT, half-life 172,800 seconds |
| Initial subsidy | 50 coins; halving every 210,000 blocks |
| Output lifetime | 5,256,000 blocks after creation |
| Expiry order | Apply transactions first, then remove surviving outputs due to expire |
| Recycle pool | Receives the value of expired outputs |
| Additional coinbase reward | At most one coin, bounded by the available pool |
| Unclaimed recycled value | Stays in the pool |

An output created at height `h` can be spent in block `h + 5,256,000`.
If it remains unspent after that block's transactions, it expires. The coinbase
may claim `min(1 coin, previous pool + expired value)` in addition to subsidy
and fees. Recycling redistributes existing value; it adds no subsidy.
The lifetime is approximately 100 years at the target interval, and is defined
by height rather than calendar time.

Disconnecting a block restores its expired outputs and preceding pool balance.
The expiry candidate source and local storage representation do not change these rules.

## Chain identity

- Genesis hash: `0000000379813245fcdd4600969c6f74684f14433badf837be5b8ffa560174ec`.
- Genesis message: `ReSatoshi mainnet 04/Sep/2026 - A new chain from block zero` (immutable consensus data).
- Network magic: `47 d9 24 34`; P2P/RPC ports: `19333`/`19332`.
- SegWit prefix: `rsat`; Base58 public-key/script versions: `60`/`122`.
- WIF version: `128`; standard xpub/xprv serialization.
- BIP34, BIP65, BIP66 and CSV activate at height 1; SegWit and Taproot from genesis.

Regtest recycling remains opt-in (`-regtest -test=recycle`) with a 200-block
lifetime. Existing Bitcoin test-network parameters are unchanged by this experiment.

Sources: [consensus parameters](../src/consensus/params.h),
[chain parameters](../src/kernel/chainparams.cpp),
[expiry and pool accounting](../src/node/recycle.cpp),
[block validation](../src/validation.cpp).

## Further reading

- [Node operations](node-operations.md): startup, networking, mining and wallet behavior.
- [Storage and recovery](recycle-state-integrity.md): modes, persistence and failure handling.
- [Design rationale](recycle-design.md): why the index and block paths are retained.
- [Integration APIs](recycle-integrations.md): expiry events, snapshots and kernel consumers.
- [Test inventory](../test/functional/README.md#resatoshi-recycling).
