# Resatoshi source notes

The `main` branch contains Resatoshi's independent mainnet implementation,
based on Bitcoin Core 31.0. Published package versions and their source tags
are listed under [Releases](https://github.com/resatoshi/bitcoin-resatoshi-31.0/releases).

## Mainnet

- Own genesis block, network message start, payment-address prefixes and ports.
- SHA-256d proof of work, a 600-second target interval, and ASERT difficulty
  adjustment with a two-day half-life.
- An initial 50-coin subsidy, halved every 210,000 blocks.
- UTXO expiry after 5,256,000 blocks and a recycle pool paying up to one coin
  per block from available value, alongside subsidy and transaction fees.

## Recycle state and wallet

- Dedicated serialization for expiry schedules and recycle undo records.
- Recycle state restoration on block disconnection and interrupted-flush recovery.
- Wallet balance and coin selection accounting for expired outputs.
- Expiry information in `listunspent`.
- Coin statistics using `indexes/coinstatsindex-recycle-v1` when recycling is enabled.

See [network rules](resatoshi.md) and [recycle state](recycle-state-integrity.md)
for implementation details. The [upstream 31.0 notes](release-notes/release-notes-31.0.md)
record changes inherited from Bitcoin Core.
