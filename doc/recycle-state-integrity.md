# Resatoshi storage and recovery

## Format boundary

This experimental branch uses chainstate format 5, typed block undo, and snapshot
version 3. Start with a fresh network data directory and synchronize from blocks.
Old chainstate, undo and snapshots are not migrated. Keep wallet backups and the
original node's directory separate; changing a marker is not a conversion.
A `recycle-format` file identifies directories created by this storage version.
A single database format marker prevents codec-aware older binaries from opening
new state. Software that ignores format checks must not share these directories.

## Expiry modes

Choose `-recyclemode=index` (default) or `-recyclemode=block` for a fresh directory.
The `recycle-mode` file records that choice. On subsequent starts, both daemon
and GUI use the recorded mode unless command-line/config settings explicitly
request a mode. An explicit mismatch is rejected. Changing modes requires another
fresh directory. Format and mode markers are published by committing a temporary
file and renaming it; an abandoned temporary file is replaced on retry.

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
Simply sharing one database is not sufficient to guarantee these recovery semantics.

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
