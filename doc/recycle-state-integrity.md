# Resatoshi recycle state

Recycling is enabled on mainnet. An unspent output expires 5,256,000 blocks
after its creation; outputs spent in their expiry block are excluded. Expired
value enters the recycle pool. Each block can claim up to one coin from the
available pool, in addition to its subsidy and fees. Partial claims leave the
remaining value in the pool. See [network rules](resatoshi.md).

## State and recovery

The chainstate database maintains a derived index with one key per spendable
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

## Upgrade and downgrade

Before block replay, startup rebuilds the index from persisted UTXOs and deletes
legacy schedule records without decoding them. This does not require historical
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

The [September 11 development record](history/recycle-state-integrity-20260911.md)
is historical validation information, not the current storage design.
