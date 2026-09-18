# Resatoshi recycle state

Recycling is enabled on mainnet. An unspent output expires 5,256,000 blocks
after its creation; outputs spent in their expiry block are excluded. Expired
value enters the recycle pool. Each block can claim up to one coin from the
available pool, in addition to its subsidy and fees. Partial claims leave the
remaining value in the pool. See [network rules](resatoshi.md).

## State and recovery

Expiry schedules, pool balance, and undo data are stored with chainstate.
A dedicated reader handles the internal schedule and undo records.
Block disconnection restores expired UTXOs and the preceding pool state.
Interrupted chainstate flush recovery reapplies the corresponding transition.

For chainstates without the codec marker, startup checks the schedules and
reconstructs missing records from the original blocks. Reconstruction requires
those source blocks. The codec marker identifies the upgraded chainstate
format and prevents older binaries from opening it.

## Wallet and indexes

Wallet balances and coin selection exclude expired outputs and restore them
when a reorganization makes them available again. `listunspent` reports the
expiry height and remaining blocks. Coin statistics include expiry and recycle
claims, using `indexes/coinstatsindex-recycle-v1` when recycling is enabled.
Block filters and REST spent-output results exclude the synthetic recycle
undo entry.

## Tests

The repository includes `src/test/recycle_tests.cpp`,
`test/functional/feature_recycle.py`, and
`test/functional/feature_recycle_spend.py`. They cover state records, expiry,
claims, wallet accounting, reorganization, restart, and recovery.
`-regtest -test=recycle` uses a 200-block lifetime for testing.

The [September 11 development record](history/recycle-state-integrity-20260911.md)
preserves the checks and scope recorded at that revision. It is historical
validation information, not a description of the current deployment.
