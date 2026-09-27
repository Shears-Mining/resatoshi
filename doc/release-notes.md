# Resatoshi source notes

The `main` branch contains Resatoshi's independent mainnet implementation,
based on Bitcoin Core. To download the all-in-one miner with mining,
wallet, explorer, and other bundled features, visit the `download-only` channel
in the [official Resatoshi Discord](https://discord.gg/QyM3eHZZS).
Consult the package's accompanying notes for its version and source information.

## Unreleased maintenance changes

Current upstream integration includes selected fixes from Bitcoin Core 31.1;
this is not a complete upgrade to that release. See
[upstream maintenance](upstream-maintenance.md) for source provenance,
incorporated fixes, remaining differences, and local validation commands.

- Correct actual UTXO count/size changes in `getblockstats` when outputs expire,
  and expose the number removed as `expired_outputs`. Legacy transaction-only
  statistics keep their meaning.
- Reject expired explicit inputs in `sendall`, allow individually unlocking
  already expired outputs, and exclude expired selections in GUI coin control.
- Separate GUI settings, installer/autostart identifiers and macOS bundle names
  from Bitcoin. Register `resatoshi:` for OS payment links; manually entered
  legacy `bitcoin:` links remain accepted. BTC/mBTC/bits/sat units are unchanged.
- Clarify the `getnetworkhashps(-1)` sampling window without changing its result.
- Backport private-broadcast proxy retry and addrman fixes, settings-file write
  failure checks, signed-input size estimation, empty MuSig key rejection, the
  final BDB page check, script-check data lifetime ordering, and SHA256 SSE4
  sanitizer compatibility fixes from Core 31.1.
- Restore omitted vendored build/test source files and add regression tests.

These maintenance changes preserve the existing ReSatoshi genesis, ASERT,
UTXO expiry period and recycle reward rules. They do not require a consensus
activation or change BTC unit conversions.

Node tests and isolated chain replay have passed locally. Qt execution and
Windows/macOS installation tests remain outstanding. The optional kernel-library
spent-output API is not ready for recycle-enabled external integrations; see
[known limitations](upstream-maintenance.md#local-validation).

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
