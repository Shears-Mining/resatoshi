# Resatoshi source notes

The `main` branch contains Resatoshi's independent mainnet implementation,
based on Bitcoin Core. To download the all-in-one miner with mining,
wallet, explorer, and other bundled features, visit the `download-only` channel
in the [official Resatoshi Discord](https://discord.gg/QyM3eHZZS).
Consult the package's accompanying notes for its version and source information.

## Unreleased maintenance changes

- Identify display release `31.0.0-rsat.1` as ReSatoshi, with the Bitcoin Core
  base and source revision, while preserving numeric client/protocol versions.
- Add opt-in `getblockchaininfo include_recycling=true` rules and pool status;
  existing calls retain their response fields.
- Show confirmed outputs' expiry heights and remaining blocks in coin-control
  tooltips, with the synced height used for the calculation.

Current upstream integration includes selected fixes from Bitcoin Core 31.1;
this is not a complete upgrade to that release. See
[upstream maintenance](upstream-maintenance.md) for source provenance,
incorporated fixes, remaining differences, and local validation commands.

- Enable automatic mainnet peer discovery using `seed.resatoshi.org`, the two
  documented bootstrap hostnames and upstream DNS-seed/address-fetch behavior. Retry
  failed discovery with bounded backoff when there are no established automatic
  outbound full-relay or block-relay peers, including with stale known addresses
  or inbound-only connections. Recovery or network deactivation resets the timer.
- Set a conservative minimum chain work from verified block 8,000 for initial
  synchronization protection; keep assume-valid disabled. See the verification
  and future update policy in [network rules](resatoshi.md#bootstrap-and-minimum-chain-work).
- Exclude internal recycle undo from the optional kernel library's transaction
  spent-output API, preserving the on-disk records used for recovery. Test real
  expiry blocks, transaction inputs, object lifetimes and disk reopen through
  the public kernel API.
- Add opt-in `getdescriptoractivity include_expired=true` expiry events and
  confirmed output expiry heights. Keep default responses and basic compact
  filters unchanged; document how integrations find expiry-only blocks.
- Convert large snapshot metadata into a separate SQLite `recycle_state`
  table while retaining ordinary script limits and bounded write batches.
  See [integration guidance](recycle-integrations.md).

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
- Backport the paired 31.1 LevelDB and chainstate compaction changes to reduce
  read-triggered database rewrites and periodically clean up obsolete records.
  Preserve the recycle-state codec and test state across compaction, cache
  resizing, restart, recovery replay, and block disconnection.
- Backport depends cache identification, Signet helper, BSD build instructions,
  and functional/fuzz test fixes from 31.1. Hosted-CI changes and upstream
  release identifiers are not imported.

These maintenance changes preserve the existing ReSatoshi genesis, ASERT,
UTXO expiry period and recycle reward rules. They do not require a consensus
activation or change BTC unit conversions.

Node tests, isolated chain replay and Linux Qt tests have passed locally.
Windows/macOS installation tests remain outstanding. Optional kernel-library
integrations require their own build and tests; node-only testing does not
cover that API.

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
