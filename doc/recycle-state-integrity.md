# Resatoshi storage and recovery

## Format boundary

Resatoshi uses chainstate format 5, typed block undo, and snapshot version 3.
Each chainstate DB records its format in `r` and its mode in `m`. There are no
separate directory format or mode files. Older local formats require a full
`-reindex` with complete block history, or a fresh directory and synchronization.
Keep wallet backups separately; rebuilding chain data does not restore keys.

Before opening or wiping chainstate, startup checks the existing DB metadata.
An unsupported format or a mode mismatch is rejected, including with
`-reindex-chainstate`. Block history without chainstate metadata also requires
full `-reindex`; its undo format cannot be inferred safely. Full `-reindex`
rebuilds the block index, chainstate and referenced undo, so it can change modes.
Interrupted rebuilds with missing metadata can be restarted with `-reindex`
and an explicit `-recyclemode=index` or `-recyclemode=block`.

## Expiry modes

Mode precedence is explicit command-line/configuration, then the recorded mode
in existing chainstate. For a new data directory, saved `settings.json`
preferences apply, otherwise the default is `block`. The GUI preselects Original
blocks and saves the accepted choice in `settings.json`. Existing format-5 DBs
retain their mode even without saved settings; old directory marker files are
ignored and are not recreated.

With `-nosettings`, a different `-settings` file, or lost settings, existing DB
metadata still selects the mode. Explicit mode conflicts are refused without a
full reindex; the DB is never relabeled silently. Missing, unreadable or
inconsistent metadata cannot select a mode automatically. A full reindex of
such history requires an explicit mode choice.
Use a full `-reindex` with complete history or a fresh directory to switch modes.
Do not use `-reindex-chainstate` to convert storage formats or modes, because it
retains the block index and may reuse existing undo records.

Index mode maintains `(E, creation_height, outpoint)` keys for current spendable
UTXOs. The key plus placeholder value has 42 raw bytes before database overhead
and compression. Spending and expiry remove the matching key. Coin and index
updates share each database batch, including partial flushes. Cached height
lookups merge parent results with local additions, overwrites and spends.

Block mode reads the candidate's own ancestor at `height - lifetime`, verifies
its transaction Merkle root and mutation flag, and queries its outputs against
the current UTXO set. A missing or unreadable origin is a local fatal error.
It builds no height index. Snapshot activation remains unavailable in this mode.

Both support pruning. Index mode can discard historical blocks subject to normal
prune locks. Block mode retains the lifetime plus `MIN_BLOCKS_TO_KEEP` (288)
blocks, so mainnet needs approximately 100 years of history before those older
files become eligible. A disk target does not override that retention window.
Level-4 verification reports incomplete coverage if an origin has been pruned.

## Pool, undo and interrupted writes

The pool is the chainstate database's `P` key, outside the UTXO namespace.
A cached pool value follows nested cache flush/sync operations. Discarding a
child cache discards its pool changes along with its coin changes.

`CBlockUndo::vtxundo` contains exactly one entry per non-coinbase transaction.
The separate `recycle` field records whether expiry is active, `pool_before`,
and `(outpoint, Coin)` pairs for outputs actually expired. It contains no
placeholder schedule, encoded script, or synthetic input record.

A large UTXO flush may span multiple LevelDB batches. The first marks the old
and new tips in `DB_HEAD_BLOCKS`; the final publishes the pool balance and new
best-block marker together and clears the head marker. Restart recovery uses
typed undo to restore the previous state and replay the transition idempotently.
Expiry restoration checks whether each coin already exists before adding it, as
transaction-input undo does. Overwrites are reported as unclean during normal
disconnection and tolerated by interrupted-flush replay.
Because a flush can span multiple database batches, sharing a database alone does
not make the entire flush atomic; the head markers and replay provide recovery.

## Index checks and recovery

Index checks remain available through `checkrecycleindex`, after an unclean
shutdown, and periodically during operation. A pinned database snapshot compares
both directions: every spendable coin must have its correct height key, and every
height key must match a live spendable coin. Reports bound examples while retaining
complete counters. Cache resizing cancels a leased scan before replacing the DB.
Automatic checks distinguish valid results, deferred checks, shutdown interruption,
and detected corruption. A busy or cancelled scan is retried rather than treated
as damaged state.

The normal expiry lookup cannot distinguish a missing index entry from an empty
result. The background checks detect persistent discrepancies but are not a proof
that every lookup was complete at every instant. A detected failure stops validation
and mining, records `recycle-corrupt`, and triggers a full block/chainstate rebuild
on the next start. A pruned node without complete history needs fresh synchronization.
An interrupted current-format index rebuild is restartable; legacy format migration
is not supported.

## Operator checks

Use `getblockchaininfo true` to inspect mode, lifetime and pool balance.
Use `checkrecycleindex` for index correspondence and `verifychain 4 0` for historical
connect/disconnect checks with available data. Preserve wallet backups separately.
See the [test inventory](../test/functional/README.md#resatoshi-recycling) for local
regtest validation; those tests never use production data directories.
