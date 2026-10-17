# Recycling: explorer and wallet integrations

Recycling removes unspent outputs at the end of their expiry block without a
spending transaction. Mainnet lifetime is 5,256,000 blocks, approximately 100
years at the target interval. Integrations must track this state transition as
well as ordinary transactions. The consensus rules are unchanged by these APIs.

## Descriptor activity and compact filters

`getdescriptoractivity blockhashes scanobjects include_mempool include_expired`
accepts an optional fourth boolean, `include_expired`, defaulting to `false`.
Existing calls keep their transaction-only response. With `true` on a recycling
network:

- Confirmed, spendable `receive` events include `expiry_height`.
- Outputs actually removed in a requested block produce `type: "expire"`, with
  `amount`, `blockhash`, `height`, `creation_height`, `prevout_txid`,
  `prevout_vout`, and `prevout_spk`. There is no spending transaction ID.
- Outputs spent in their expiry block produce a normal `spend`, not an `expire`.
  Unconfirmed receives have no fixed expiry height.

For example, query known creation or expiry blocks with:

```sh
bitcoin-cli -named getdescriptoractivity \
  blockhashes='["BLOCK_HASH"]' scanobjects='["addr(ADDRESS)"]' \
  include_mempool=false include_expired=true
```

BIP158 basic filters retain their transaction-based semantics. Neither filter
contents nor filter headers include expiry events. Therefore `scanblocks` can
omit a block whose only relevant change is expiry. Enabling `include_expired`
does not change which block hashes `scanblocks` returns.

An indexer using filters should:

1. Discover receives and spends through the usual filter scan and query their
   blocks with `include_expired=true`. Track each live outpoint's expiry height.
2. Additionally query active-chain blocks at the tracked expiry heights, even
   if their filters do not match. Process transactions first, then actual expiry
   events. Do not treat an expiry schedule as proof that an output expired: it
   may have been spent earlier or in that same block.
3. Persist changes with their block hashes. On a reorganization, undo disconnected
   receives, spends and expiries, restore the previous schedule, and process the
   replacement branch. A receive's height can change when it is reconfirmed.
   Resolve heights to hashes and verify chain continuity before committing each
   batch; the RPC is not an atomic snapshot across a concurrent reorganization.
4. Keep unconfirmed activity separate. It is provisional and cannot prevent a
   confirmed expiry unless the spending transaction is included in time.

Historical queries need both block and undo data. Use an unpruned node or index
and retain the results before pruning. This procedure assumes the indexer has
scanned the full relevant history; a partial scan cannot discover unknown
outputs merely from their expiry heights.

## Core wallet pending transactions

An unconfirmed transaction whose input has expired cannot reserve its other,
still-live wallet inputs. This also applies to its unconfirmed descendants.
The wallet derives this from its processed chain tip; it does not permanently
abandon the transactions. A rollback before expiry restores their reservations.

For jointly funded transactions, an input's parent may belong to another wallet.
The wallet stores its confirmed creation height and block hash as optional
transaction metadata (`recycle_input_origins`), gathered from available UTXOs or
observed parent blocks. Before using it, the wallet checks that the block belongs
to its processed chain. Missing UTXOs alone are never treated as expiry evidence.
This metadata does not change consensus, transaction encoding or chainstate.
It is not exposed in `gettransaction` results.

A wallet upgraded after such an external input has already disappeared may lack
this evidence. It conservatively retains the pending transaction's reservations.
A full `rescanblockchain` on a node retaining the parent blocks can recover it;
the wallet bypasses basic-filter skipping when external parents need inspection.
Pruned, unavailable history cannot be reconstructed by this change. Upgrading
while the external inputs are still unspent captures their origins on wallet load.

Balance and coin-selection queries share ancestry results only for that read
operation under the wallet lock. No expiry-result cache survives a tip change,
transaction update, rescan or wallet reload.

## UTXO snapshot conversion

Snapshot version 3 contains only real transaction outputs. Its header adds the
explicit recycle pool balance after the coin count. The count excludes the pool.
`hash_serialized_3` binds a nonzero pool with a domain-separated suffix; MuHash
remains a commitment to spendable UTXOs. Old snapshot commitments and bytes must
not be reused for this format. No mainnet AssumeUTXO snapshot is configured.

`contrib/utxo-tools/utxo_to_sqlite.py` converts this version to `utxos` and a
single-row `recycle_pool(balance)` table, in satoshis. There is no reserved output
namespace or oversized internal script exception. Ordinary script bounds apply.

## Kernel and REST consumers

The kernel spent-output API and REST spent-output results contain transaction
inputs only. `CBlockUndo::vtxundo` no longer needs stripping or interpretation of
a synthetic final entry. The separate expiry payload remains available to local
recovery and expiry-aware RPC consumers. An expiry-only block has no non-coinbase
transaction undo entries; expiry events remain available through descriptor RPC.
