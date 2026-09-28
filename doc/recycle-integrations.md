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

## UTXO snapshot conversion

`contrib/utxo-tools/utxo_to_sqlite.py` recognizes Resatoshi mainnet snapshots and
recycling regtest snapshots. It writes ordinary coins to `utxos` and reserved
internal state to a separate `recycle_state` table with the same columns.

State records use a zero transaction hash and output indices at or above
1,000,001. Their `scriptpubkey` contains opaque state bytes, not a payment script.
Do not count them as spendable coins or display them as user transactions. The
snapshot header's record count includes both tables. Keep the state table when
archiving a complete conversion; it is not a replacement for a validated node's
snapshot importer.

Ordinary scripts retain their 10,000-byte bound. Internal metadata permits up
to 32 MiB per record, matching the node's serialization limit, and must start
with OP_RETURN. Writes are flushed by both record count and encoded byte size
to avoid accumulating many large records in a single batch. Existing `--txid`
and `--spk` encodings apply to both tables.

## Kernel consumers

The optional kernel library's spent-output API returns one entry per
non-coinbase transaction. Internal recycle undo is excluded from that API and
retained on disk for state recovery. An expiry-only block consequently has no
transaction spent-output entries. This API does not expose expiry events;
consumers needing them can use the opt-in descriptor RPC above.
