# Resatoshi network rules

This page describes the mainnet implementation in this repository.

## Chain identity

- Genesis hash: `0000000379813245fcdd4600969c6f74684f14433badf837be5b8ffa560174ec`.
- Genesis message: `ReSatoshi mainnet 04/Sep/2026 - A new chain from block zero`.
- P2P message start: `47 d9 24 34`.
- Default P2P port: `19333`; default RPC port: `19332`.
- Bech32/Bech32m human-readable prefix: `rsat`.
- Base58 public-key address version: `60`; script address version: `122`.
- WIF version: `128`; BIP32 serialization uses the standard xpub/xprv versions.

Sources: [mainnet parameters](../src/kernel/chainparams.cpp),
[RPC ports](../src/chainparamsbase.cpp).

## Mining and activation

Header presynchronization and its reproducible anti-DoS parameters are described
in [header synchronization](headers-sync.md).

SHA-256d proof of work targets one block every 600 seconds. ASERT adjusts
mainnet difficulty with a half-life of 172,800 seconds. The initial subsidy is
50 coins and halves every 210,000 blocks.

BIP34, BIP65, BIP66 and CSV activate at height 1. SegWit and Taproot are active
from genesis. Mainnet uses the documented bootstrap hostnames through Bitcoin
Core's existing DNS-seed / address-fetch path. No fixed seeds, assume-valid
block, or AssumeUTXO snapshots are configured.

Sources: [difficulty calculation](../src/pow.cpp),
[subsidy and block validation](../src/validation.cpp),
[mainnet parameters](../src/kernel/chainparams.cpp).

### Bootstrap and minimum chain work

Builds from the current `main` source discover peers through
`seed.resatoshi.org`, `resatoshi-seed.freeddns.org`, and
`resatoshi-seed.duckdns.org`. The first entry is a DNS crawler that returns
candidate peer addresses through service-bit queries, including
`x9.seed.resatoshi.org` for NODE_NETWORK and NODE_WITNESS. The other two entries
are the existing operator's bootstrap hostnames. If a hostname does not support
service-bit DNS queries, the upstream networking code falls back to connecting
to that host and requesting peer addresses.

The list is compiled into the node; older binaries retain their previous list.
DNS responses assist peer discovery, while nodes validate the chain themselves.
Normal `-dnsseed`, `-connect`, `-proxy`, and manual-peer controls still apply.
Multiple hostnames do not establish operator independence; independently
operated seeds remain desirable as the network grows.

If a fresh mainnet node has neither peers nor known addresses, it retries seed address
fetches after 60 seconds, doubling the interval up to 15 minutes after each
retry. This allows recovery after a temporary bootstrap outage without a
restart. Retries use the existing proxy-aware address-fetch path and require
network activity and automatic DNS seeding; explicit `-connect` disables them.
Inherited Bitcoin test networks retain their upstream seed retry behavior.

The default minimum chain work is
`0000000000000000000000000000000000000000000000000cfed9b27eee9a28`,
the cumulative work through height 8,000, block
`0000000000002714ba4d770189e83f6fb05d8bca671cd22849a8e52fc35d5495`.
On 2026-09-28 this block was over 1,700 blocks behind the locally validated tip.
Its 8,001 headers, including genesis, were separately checked from local block
files for ancestry, SHA-256d proof of work, ASERT targets and median time past;
the independently calculated work matched the node's RPC result. This is local
verification, not an attestation from an independent operator.

This floor uses Bitcoin Core's existing initial-sync/header anti-DoS policy.
It does not pin the block hash as a consensus checkpoint or skip transaction
or script validation. Alternative valid chains with sufficient work remain
eligible. `-minimumchainwork` retains its upstream override behavior; setting
it to zero disables this configured protection and is only useful for isolated
short-chain tests or deliberate recovery work.

For future release updates, choose a fully validated block at least 1,000
confirmations deep and at least seven days old. Record the height, hash, time,
work and validation evidence; recompute the header-chain work and compare with
independent operators when available. If observations disagree, defer the
update. Review the constant as a normal source change rather than automatically
trusting a remote service at startup. Keep `defaultAssumeValid` disabled.

`getnetworkhashps` defaults to a 120-block estimation window. Positive `nblocks`
values select an explicit window. For compatibility, `nblocks=-1` retains the
legacy window of `(height % 2016) + 1` prior blocks on mainnet, capped at the
selected height. This is not the interval since the last ASERT target change.
Use a positive window for comparisons over a consistent number of blocks.

## UTXO recycling

An output created at height `h` expires at `h + 5,256,000` if it remains
unspent after the transactions in that block have been applied. Spending an
output in its expiry block therefore prevents its recycling. At the target
block interval the lifetime is approximately 100 years; the rule uses block
height, not elapsed calendar time.

Expired value enters the recycle pool. The coinbase transaction may claim up
to the smaller of one coin and the available pool, in addition to the ordinary
subsidy and transaction fees. Unclaimed value stays in the pool. The pool
redistributes existing coins rather than creating an additional subsidy.

Wallet balances and coin selection exclude expired outputs. `listunspent`
reports expiry information. Disconnecting blocks restores the corresponding
UTXOs and recycle state. See [recycle state](recycle-state-integrity.md) and
[explorer, wallet and snapshot integrations](recycle-integrations.md).
`sendall` also rejects explicitly selected expired inputs before creating a
signed transaction or PSBT. Spending in the expiry block remains valid.

`getblockstats` reports the number of outputs actually removed by expiry as
`expired_outputs`, excluding outputs spent in the expiry block. Its
`utxo_increase_actual` and `utxo_size_inc_actual` fields include these removals.
For example, creating 10,000 spendable outputs and expiring 10,000 outputs
reports a net count change of zero and `expired_outputs` of 10,000. The legacy
`utxo_increase` and `utxo_size_inc` fields describe transaction outputs minus
inputs, including unspendables and excluding expiry. Size changes use the RPC's
serialized-output-plus-overhead estimate, not the physical database file size.

Sources: [consensus parameters](../src/consensus/params.h),
[recycle implementation](../src/node/recycle.cpp),
[recycle constants](../src/node/recycle.h).

## Version identity and node status

The display release is `31.0.0-rsat.1`, based on Bitcoin Core 31.0 with the
selected maintenance backports documented separately. `bitcoind --version`,
`bitcoin-cli --version`, startup logs and the GUI full version identify
ReSatoshi and include the upstream base and source revision. A `-dirty` suffix
means the checkout includes local changes, including untracked files; it is
not an exact, reproducible source revision. Git archives use their exporting
commit; builds without Git or archive provenance report `-unknown`.
The source revision and dirty marker stay in the first version token, for example
`v31.0.0-rsat.1-g<commit>-dirty (Bitcoin Core base 31.0.0)`, so upstream release
tools can recognize locally modified builds.
This display release is independent of the upstream numeric client version
and P2P user agent, which retain their existing values.

To inspect recycling rules and state at the reported `bestblockhash`:

```sh
bitcoin-cli getblockchaininfo true
bitcoin-cli -named getblockchaininfo include_recycling=true
```

The optional `recycling` object contains `enabled` and, on enabled networks,
`expiry_blocks`, `max_reward_per_block`, and `pool_balance`. Amounts are in
whole coins, using the existing RPC amount convention. The pool balance is
unclaimed value after the tip's transactions and recycling; it excludes outputs
that will expire in future blocks. It is not a quote for the next block's
reward. The state and block hash are read under the same chain lock.

Omitting `include_recycling`, or passing `false`, preserves the existing
response fields. Disabled networks return only `recycling: {"enabled": false}`
when requested. The existing REST chain-info response remains unchanged.

These display and read-only API additions do not alter consensus rules, disk
formats, wallet formats, network identifiers or numeric protocol versions.
They require no chain reset, reindex, wallet migration or activation height.

## GUI settings and upgrades

In coin control, hover over an output's amount or confirmation count to see
its expiry height and remaining blocks at the displayed synced height. This
is a snapshot when the list is populated; reopening the dialog refreshes it.
Only confirmed spendable outputs on recycling networks have a fixed expiry
height. Spending in the expiry block still prevents recycling. The display
uses block heights rather than a promised calendar date.

The GUI uses separate `ReSatoshi` / `ReSatoshi-Qt` settings, with distinct
application names for each test network. It does not import the `Bitcoin` /
`Bitcoin-Qt` settings used by earlier releases, since those may belong to
Bitcoin Core. Existing Bitcoin settings and wallet files are left untouched.

On first GUI startup with the new settings, select the existing ReSatoshi data
directory in the directory chooser, even if it is already present. If you used
a custom directory, choose that same directory to retain your wallets and chain
data. An explicit `-datadir` continues to bypass this chooser. GUI preferences
such as window geometry must be set again; no wallet or chain data is moved.

BTC, mBTC, bits and sat units and their conversion factors remain unchanged.
OS installation, autostart and payment-link registrations use ReSatoshi-specific
identifiers to coexist with Bitcoin Core. New payment links use `resatoshi:`;
manually supplied legacy `bitcoin:` links remain accepted, with addresses checked
against the selected network. ReSatoshi does not register the `bitcoin:` OS handler.
The executable names (`bitcoind`, `bitcoin-cli`, `bitcoin-qt`) remain unchanged.
When installing from source alongside Bitcoin Core, use a separate installation
prefix (for example `-DCMAKE_INSTALL_PREFIX=/opt/resatoshi`) so those executable
names do not overwrite an existing Bitcoin installation.

## Test modes

`-regtest` provides a local regression-test chain. `-regtest -test=recycle`
enables recycling with a 200-block lifetime for tests. The inherited testnet3,
testnet4 and signet configurations retain their Bitcoin test-network parameters;
they are not Resatoshi mainnet. The default `-chain=main` selects Resatoshi.
