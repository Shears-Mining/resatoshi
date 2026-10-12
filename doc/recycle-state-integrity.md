# Resatoshi recycle state

Recycling is enabled on mainnet. An unspent output expires 5,256,000 blocks
after its creation; outputs spent in their expiry block are excluded. Expired
value enters the recycle pool. Each block can claim up to one coin from the
available pool, in addition to its subsidy and fees. Partial claims leave the
remaining value in the pool. See [network rules](resatoshi.md).

## Expiry source and block retention

On a fresh data directory, the GUI asks node and wallet users to choose Index
or Original blocks after configuration and network selection, before node
initialization. This also applies with an explicit `-datadir`. Original blocks
reveals complete-history and pruning choices. Accepting saves editable
preferences in the folder's `settings.json`; cancelling does not start the node
or create a wallet. Existing and legacy folders skip the method selection.
Their recorded mode is honored even when GUI settings are reset. Explicit
command-line or read-only configuration mismatches remain errors in the core.

Explicit startup choices take precedence over the GUI. With settings disabled,
GUI storage changes are unavailable; supply pruning through startup options.
The recorded folder mode still lets an existing block-mode GUI launch without
repeating the mode option. Server-only `bitcoind` uses options/configuration.

Choose `-recyclemode=index` (default) or `-recyclemode=block` when starting a
fresh network data directory. The `recycle-mode` file records the choice.
Existing directories without this file are registered as index mode. Changing
mode is rejected, including with reindex options: use a fresh data directory
and synchronize again. Both modes implement the same network rules, pool coin
and undo format, and can exchange blocks with each other.

Block mode reads the ancestor block at `candidate_height - expiry_blocks` and
checks its spendable output outpoints against the current UTXO set. Only live
coins with the matching creation height expire. Template rewards also exclude
outputs spent by candidate transactions. The source block is chosen from the
candidate's own ancestry, including during a reorganization. Missing or
unreadable source data is a local error, never an empty expiry list. Transaction
contents are checked against the source header's Merkle root before use,
including the duplicate-transaction mutation check.

Block mode does not create or update the derived height index, including its
lazy in-memory index. Its chainstate uses codec marker 4, which older index-only
binaries refuse to open. UTXO snapshot activation is unavailable in block mode;
synchronize from blocks instead. Transaction and other optional indexes remain
separate settings.

Use `-prune=0` to retain all blocks. Existing automatic (`-prune=<MiB>`, above
the supported minimum) and manual (`-prune=1`) pruning are also available.
In block mode both preserve the expiry lifetime plus `MIN_BLOCKS_TO_KEEP`
(currently 288) blocks, respecting other prune locks and snapshot boundaries.
Level-4 verification stops when its expiry origin has been pruned; an all-history
`verifychain(4, 0)` then reports incomplete verification rather than a successful
full audit. The retained reorg window remains verifiable.
Files are deleted only when their whole height range is eligible. Consequently
the disk target is not a guarantee: retaining mainnet's 5,256,000-block expiry
window may require substantially more space. Reorganizations deeper than the
retained window can require retrieving old blocks. Returning to full retention
after pruning requires downloading the deleted history again.

## Index state and recovery

In index mode, the chainstate database maintains a derived index with one key per spendable
UTXO: `(E, creation_height, outpoint)`, with a one-byte placeholder value.
Creation height is equivalent to expiry height minus the network's fixed
lifetime. Only exact-height prefix lookups are used, so the fixed-width height
encoding does not need numeric sort order. Spending or expiry removes the key.
There is no historical block-output list. Live index storage is O(UTXO), although
LevelDB may retain deleted bytes until compaction and each key has overhead.

Coin changes and their index changes are written in the same LevelDB batch,
including partial flushes. The existing head-block markers and undo replay
handle interrupted flushes. Deletion reads the previous on-disk coin's height;
overwriting an outpoint at a different height removes the previous key first.
In-memory views merge matching parent entries with their own additions and
spends. A lazily allocated height index avoids scanning the entire cache on
every expiry query, and its memory is included in cache accounting.

The pool balance remains a single constant-size internal coin record. Undo
retains its existing encoding and includes expired coins and the preceding
pool balance. The legacy schedule field is a constant-size placeholder in new
undo records; old undo is still decoded so pre-upgrade blocks can disconnect.
Disconnecting restores UTXOs and thereby restores their index entries.

## Integrity limits and recovery

The index is derived but participates in consensus calculations: a missing key
can hide a real UTXO from expiry processing. Actual coin lookups reject phantom
or mismatched-height entries, but cannot discover omitted entries. The ready
marker certifies migration completion; it is not a checksum or a full audit on
every startup. Atomic writes and interrupted-flush replay protect the supported
write path, not arbitrary disk corruption or external database edits.

`index_corruption_and_rebuild` injects missing, phantom and wrong-height keys
into closed test databases. It checks the resulting lookup/reward discrepancy,
and verifies that an interrupted-migration rebuild recreates the index from
unchanged coins. Its marker editing is deliberately test-only, **not a repair
procedure**. Rebuilding only the index after incorrect expiry has already been
processed cannot reconstruct the correct UTXOs or pool balance.

Index mode automatically runs the same snapshot-based full correspondence
check used by `checkrecycleindex` after an unclean shutdown, before loaded-block
verification. Legacy directories are checked once when their mode is first
registered. A durable `recycle-unclean` session marker is removed only after
shutdown flushes have reached the persisted chain tips. Block mode does not
run these scans.

During operation a dedicated worker checks periodically, outside `cs_main`
except for flushing and acquiring the snapshot. The default minimum interval is
one hour (`-recyclecheckinterval=3600`). A measured full RPC check with 8,867
UTXOs took 0.04 seconds on the development node. The worker measures flush plus
scan time and increases subsequent intervals to at least 1,000 times that
cost, limiting scan duty cycle to 0.1%. Each completed check logs duration and
record counts. An overlapping manual scan or a cache-resize cancellation is
retried on a later tick; shutdown cancels the scan and joins the worker.

A failed check latches validation/mining off and requests shutdown. A durable
`recycle-corrupt` marker forces automatic block-index and chainstate replay on the next start
when complete history is available. The marker is cleared only after block
import and activation complete without interruption. A directory that has
actually pruned history is refused before its chainstate is wiped; synchronize
a fresh directory instead. Full block reindexing also clears locally recorded invalid-block flags, which
may have been produced by incorrect reward calculations. Repairing only index
entries is insufficient because
incorrect expiry may already have changed the UTXOs and pool.

These checks do not prevent a silent omission between scans or prove historical
expiry correctness. Both expiry modes still rely on correct UTXO state and
storage. Periodic checking is a detection and recovery measure.

For suspected chainstate corruption, stop the node, preserve diagnostic logs
and a copy of the affected database, and protect wallet backups. With complete
block files and pruning disabled, the supported `-reindex-chainstate` option
replays blocks to reconstruct chainstate and its index together. Pruned nodes
require `-reindex` and retrieval of missing history; `-reindex-chainstate` is
incompatible with prune mode. Compare the resulting tip, UTXO statistics and
recycle pool against an independently validated node at the same block hash.
Successful startup or `verifychain` alone must not be advertised as an exhaustive
index audit. Use the read-only correspondence check below for this purpose.

### Full index correspondence check

```sh
bitcoin-cli checkrecycleindex start
bitcoin-cli checkrecycleindex status
bitcoin-cli checkrecycleindex abort
```

`start` first flushes the active coin cache through its normal persistence path,
then captures a single LevelDB snapshot with its block hash and height. The
two passes and all point lookups use that same snapshot, even if new blocks or
reorganizations occur concurrently. After capture the scan does not hold
`cs_main`. It streams coins and index entries instead of keeping the whole UTXO
set in memory, bypasses LevelDB's read cache, checks iterator storage errors,
and retains at most 20 example issues. Storage I/O and snapshot retention can
temporarily increase disk use and affect throughput.

The coin pass checks that every spendable ordinary coin has its expected height
key. The index pass checks that each entry has an existing, spendable ordinary
coin at the same height and a valid one-byte zero placeholder. Internal pool
state is excluded from indexed coins. Malformed keys and values are reported.
`missing`, `orphaned`, `height_mismatches`, `unexpected`, `malformed_coins` and
`malformed_index_entries` are full counters, not limited by the example cap.
A wrong-height entry can count as both a mismatch and a missing correct key;
the categories are not a count of distinct affected outpoints.

`success` means the scan completed, not that the index is correct. Only a
completed scan returns `valid`; it is true only when every inconsistency counter
is zero. An aborted check returns partial counters without `valid`. `status`
reports the phase and examined record counts, or null when no check is running.
There is no percentage estimate for uneven key distributions. One
check runs at a time. Cancellation is cooperative; `abort` acknowledges the
request rather than waiting for resource release. A database cache resize also
cancels the check and waits for its snapshot to release before reopening the DB.

This verifies current persisted UTXO/index correspondence only. It cannot prove
that earlier expiry was processed correctly, validate the whole chain or
authenticate pool accounting. It never rewrites index entries, changes a codec
marker or repairs data. Investigate failures using the recovery procedure above;
for historical correctness, replay blocks and compare independent node state.

The serialized key plus placeholder value occupies 42 bytes per live indexed
UTXO: about 4.2 GB for 100 million entries before compression and database
overhead. This is not a disk-space forecast. Expiry work scales with the number
of entries at the expiring height, and undo retains their coin data. Performance
claims need hardware, cache size, workload, wall time, peak memory and disk-write
measurements; a successful stress test alone does not qualify a low-end VPS.

## Upgrade and downgrade

### Upgrading an existing node

Stop the old node cleanly and back up each wallet before upgrading. Start the
new version with the same data directory and existing settings. The directory
is automatically registered as `index` mode and checked on its first upgrade.
If the check succeeds, synchronization and mining continue from the existing
chain tip; no fresh synchronization or wallet migration is required. Subsequent
checks run after unclean shutdown and periodically during operation.

To choose `block` mode, select Original blocks in the first-run GUI for a
fresh directory, or use `-recyclemode=block`, and synchronize from blocks. An existing index-mode directory is refused,
even with `-reindex` or `-reindex-chainstate`. Back up and restore/load each
wallet into the new directory. A blockchain download cannot recover wallet
keys. Preserve the original directory until synchronization is complete and
the restored wallets have been verified.

If an index inconsistency is detected, validation and mining stop. With complete
history, restarting automatically replays blocks to rebuild both the block
index and chainstate, including clearing potentially incorrect invalid-block
flags. A previously pruned directory cannot perform this automatic recovery;
it is refused before chainstate is wiped. Preserve the old directory and wallet
backups, synchronize a fresh directory, and restore/load the wallets there.

### Legacy index migration and downgrade

In index mode, before block replay, startup rebuilds a legacy index when needed
from persisted UTXOs and deletes legacy schedule records without decoding them. This does not require historical
blocks or a wallet and also handles truncated legacy schedules. The pool balance
and chain tip are preserved. Migration requires a linear scan of chainstate and
additional temporary disk space for LevelDB writes. It runs before validation,
can be interrupted, and restarts its bounded-memory rebuild on the next launch.
It also supports a legacy DB interrupted partway through a chainstate flush:
normal replay subsequently updates each coin and index entry together.

The existing codec marker is 3 while building and 2 after a synchronized final
write. ReSatoshi binaries that validate this marker and support only codec 1
refuse the database. This does **not** protect against older software that never
checked the marker, including upstream Bitcoin Core. Do not open the upgraded
database with those programs. Downgrade needs a separate legacy database or a
legacy rebuild from complete blocks; do not manually change the marker.

This changes local storage, not expiry or reward consensus. MuHash and spendable
UTXO statistics remain comparable across versions. `hash_serialized_3`, snapshot
coin counts and snapshot bytes change because legacy schedules are no longer
coin records. Existing snapshot commitments must not be reused across formats.
No mainnet AssumeUTXO snapshot is configured. Index keys are derived data and are
not exported as coins; snapshot loading maintains them from the loaded UTXOs.

## Wallet and indexes

Wallet balances and coin selection exclude expired outputs and restore them
when a reorganization makes them available again. `listunspent` reports the
expiry height and remaining blocks. Coin statistics include expiry and recycle
claims, using `indexes/coinstatsindex-recycle-v1` when recycling is enabled.
Block filters and REST spent-output results exclude the synthetic recycle
undo entry. Mempool policy rejects reserved metadata inputs before coin reads;
this does not change block validation rules.

## Tests

`src/test/recycle_tests.cpp` covers expiry, partial claims, disk/undo round trips,
compaction, cache overlays, overwrites, migration interruption and repeated
create/spend cycles without reservation accumulation. Functional tests include
`feature_recycle.py` (partial-flush crash recovery), `feature_recycle_spend.py`
(expiry-block spends), `feature_recycle_interfaces.py` (wallet/index/snapshot
interfaces) and `feature_recycle_index.py` (index upgrade and undo compatibility).
The latter accepts `--legacy-bitcoind=/path/to/codec-v1/bitcoind` to compare both
implementations on the same blocks, upgrade a legacy DB, and test downgrade
refusal. `-regtest -test=recycle` uses a 200-block lifetime.

`feature_recycle_boundary.py` additionally compares both implementations against
an independent Python UTXO/pool ledger through height 750. It covers expiry ±1,
maturity and halving boundaries, same-block parents/children, double spends,
zero-valued outputs, partial rewards, branch switching and renewed lifetimes.
Use `--small-cache --crash-height=200` (or 201/202) for partial-write recovery at
each expiry boundary, `--boundary-seed=N` for repeatable randomized workloads,
and `--legacy-bitcoind=/path/to/codec-v1/bitcoind` for cross-version comparisons.

`feature_recycle_full_block.py` (extended suite) fills blocks to exactly
4,000,000 weight units and sends them over P2P. Separate profiles contain
99,000 spendable outputs, 16,300 independent transactions, and approximately
4 MB of witness data. It rejects weight-limit violations of one and four units,
plus late failures from excessive reward, invalid script and block double spends.
It spends at the expiry boundary while tens of
thousands of outputs expire, and compares UTXO counts, values, MuHash and pool
balances with an independent ledger. Complete output identities, values, heights
and coinbase flags are also checked at peak load, after expiry and after the
last cohort disappears. Restart, chainstate reindex, old database
migration and disconnect/reconnect across mass expiry are included. Use
`--legacy-bitcoind=/path/to/codec-v1/bitcoind --small-cache --crash-expiry` to
compare implementations and interrupt a mass-expiry database batch. The small-cache
phase uses 4 MiB and level-2 startup verification; full-chain level-4 verification
then runs with the normal cache budget. These synthetic consensus workloads
deliberately exceed standard relay policy; they are correctness stress tests,
not wallet-signature or network throughput benchmarks.

`feature_recycle_mining_load.py` (extended suite) requires standard relay policy
and loads 10,000 Taproot script-path transactions into both mempools. At their
inputs' expiry height, it checks near-full mining templates, fees and recycle
rewards, repeated proposal validation without state mutation, eviction of
unmined transactions whose inputs expire, reorg/resubmission, restart, and the
renewed outputs' expiry. `--legacy-bitcoind` compares an older implementation.
The transactions use OP_TRUE Taproot scripts; this exercises standard policy
and mining assembly but does not benchmark cryptographic signature verification.

The [September 11 development record](history/recycle-state-integrity-20260911.md)
is historical validation information, not the current storage design.
