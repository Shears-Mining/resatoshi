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

SHA-256d proof of work targets one block every 600 seconds. ASERT adjusts
mainnet difficulty with a half-life of 172,800 seconds. The initial subsidy is
50 coins and halves every 210,000 blocks.

BIP34, BIP65, BIP66 and CSV activate at height 1. SegWit and Taproot are active
from genesis. The mainnet configuration has no DNS seeds, fixed seeds,
assume-valid block, or AssumeUTXO snapshots configured.

Sources: [difficulty calculation](../src/pow.cpp),
[subsidy and block validation](../src/validation.cpp),
[mainnet parameters](../src/kernel/chainparams.cpp).

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
UTXOs and recycle state. See [recycle state](recycle-state-integrity.md).
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

## GUI settings and upgrades

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
