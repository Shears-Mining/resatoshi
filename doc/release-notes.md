# Resatoshi source notes

The `main` branch contains Resatoshi's independent mainnet implementation,
based on Bitcoin Core. To download the all-in-one miner with mining,
wallet, explorer, and other bundled features, visit the `download-only` channel
in the [official Resatoshi Discord](https://discord.gg/QyM3eHZZS).
Consult the package's accompanying notes for its version and source information.

## Default expiry mode for new nodes

Fresh data directories now default to `block` (Original blocks). Existing
chainstate keeps its recorded mode when no command-line/configuration mode is
specified, including with lost settings or a conflicting saved GUI preference.
Explicit options continue to take precedence; changing an existing mode still
requires full reindex with complete history. Unknown existing modes require an
explicit choice with full reindex, or synchronization in a new directory.

## Frankfurt DNS seed

The mainnet seed list now includes `seed-fra.resatoshi.org` alongside the Seoul
crawler `seed.resatoshi.org` and the existing bootstrap hostnames. Both support
service-bit queries. The two crawlers provide separate regional discovery
endpoints under the same operator; this does not establish operator or hosting
provider independence. Existing binaries need a new build to acquire the added
seed list. See [peer discovery](dnsseed-policy.md).

## Pruned-node recovery with a separate blocks folder

Automatic recycle recovery now checks the block-index database in the data
directory even when `-blocksdir` places raw blocks elsewhere. Pruned history
is refused before rebuilding can delete retained block or chainstate data.

## First GUI startup in a new data folder

The GUI defaults to English regardless of the operating system language. A saved
language preference or explicit `-lang` option takes precedence.

The GUI asks new node and wallet users to select **Index** or **Original blocks**
before starting synchronization. Selecting Original blocks also offers complete
history retention or pruning after expiry processing. Pruning retains the expiry
and reorganization windows, so actual disk use can exceed the storage target.
Accepted preferences are saved in `settings.json`. Existing folders skip this
selection and preserve the DB mode unless explicitly overridden. Explicit startup
options/configuration override saved preferences. Server-only
`bitcoind` continues to use `-recyclemode` and `-prune` options.

## Upgrade guidance for expiry modes

- **Format-5 upgrade:** stop the old node cleanly, back up wallets, and keep the
  same data directory with the matching mode. Separate `recycle-mode` and
  `recycle-format` files are no longer used. The recorded chainstate mode is
  inherited automatically unless command-line/configuration explicitly selects
  a mode; a matching saved GUI preference is no longer required.
- **Unsupported formats or mode changes:** use full `-reindex` with complete
  block history, or synchronize a fresh directory. `-reindex-chainstate` checks
  the existing format and mode before wiping and refuses mismatches. Missing
  chainstate metadata with retained block history requires full reindex with
  an explicit `-recyclemode=index` or `-recyclemode=block`.
  Back up wallets separately; rebuilding chain data does not restore wallet keys.
- **Index inconsistency:** validation and mining stop. On the next launch,
  complete block history allows automatic rebuilding of the block index and
  chainstate. If history was pruned, automatic recovery is refused before
  wiping chainstate: use a fresh directory and synchronize again, preserving
  and restoring your wallet backups as above.

The expiry rules are unchanged; healthy old and new nodes can use the same
chain. See [expiry modes and recovery](recycle-state-integrity.md#format-boundary)
for details. The periodic checks can still miss an omission until a later scan.

## Unreleased maintenance changes

- Add fixed per-directory `index` and `block` expiry modes, automatic index
  checks after unclean shutdown and during operation, and full block replay
  recovery after detected inconsistency. Block mode supports full retention or
  pruning that preserves the expiry and reorganization windows.

- Add `checkrecycleindex start|status|abort` for a full UTXO/height-index
  correspondence check at one coherent database snapshot. Report missing,
  orphaned, wrong-height and malformed records with bounded examples, without
  repairing data or claiming historical consensus verification.
  Completed inconsistent manual checks now stop validation/mining and persist
  the same restart recovery latch as automatic checks; cancellation does not.
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

- Enable automatic mainnet peer discovery using `seed.resatoshi.org`,
  `seed-fra.resatoshi.org`, the two
  documented bootstrap hostnames and upstream DNS-seed/address-fetch behavior. Retry
  failed discovery with bounded backoff when there are no established automatic
  outbound full-relay or block-relay peers, including with stale known addresses
  or inbound-only connections. Recovery or network deactivation resets the timer.
  Direct retries now repeat service-bit DNS queries through the same discovery
  function as startup; proxy retries still avoid local DNS resolution. A single
  pending batch runs on the DNS worker without blocking automatic connections.
- Set a conservative minimum chain work from verified block 8,000 for initial
  synchronization protection; keep assume-valid disabled. See the verification
  and future update policy in [node operations](node-operations.md#bootstrap-and-minimum-chain-work).
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
