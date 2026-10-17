# Resatoshi recycle storage rationale

The consensus transition is independent of its candidate source: process
transactions, expire surviving outputs of the origin height, credit the pool,
and debit the claimed reward. Both expiry modes are retained. Callers explicitly select an index source or
supply an original block; omitting a block cannot implicitly select the index.
Normal connection and crash replay share the pure pool reward calculation, while
retaining their different state update sequences.

The index trades extra O(live UTXO) storage and writes for direct origin-height
lookup and practical pruning. It is derived from UTXOs and can be checked or
rebuilt. The block path instead derives candidates from authenticated original
blocks and needs long-term block retention. Neither avoids storing live UTXOs;
neither repairs an incorrect UTXO set merely by using a different candidate source.
Keeping both also permits direct result comparison on identical blocks.

The pool is an explicit cached chainstate value, and expiry undo is a typed
block-undo field. This makes transaction-only consumers ordinary again and removes
special script decoders, reserved outpoints, and synthetic transaction inputs.
It introduces an explicit responsibility to propagate pool changes through cache
rollback, partial flush recovery and snapshot commitments. The tests exercise
those boundaries; code size alone does not establish a reliability improvement.

Old recycle storage formats are intentionally unsupported in this experiment.
The format check remains to prevent accidental interpretation, while old schedule
migration and encoded-undo readers are removed. Published chain history, network
identity, expiry heights and reward rules remain unchanged.

Adoption requires comparison with the original main implementation on separately
initialized directories: block acceptance, UTXO MuHash/count/value, pool balances,
reorganizations, interrupted writes, pruning and resource costs. Serialized snapshot
hashes and bytes are format-specific and are not cross-version equality criteria.
