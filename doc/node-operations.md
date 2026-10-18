# Resatoshi node operations

This storage format requires a fresh network data directory.
Keep the existing node and wallet backups separate. See [storage and recovery](recycle-state-integrity.md)
for format boundaries and [network rules](resatoshi.md) for consensus.

## Mining and activation

Header presynchronization and its reproducible anti-DoS parameters are described
in [header synchronization](headers-sync.md).

SHA-256d proof of work targets one block every 600 seconds. ASERT adjusts
mainnet difficulty with a half-life of 172,800 seconds. The initial subsidy is
50 coins and halves every 210,000 blocks.

ASERT uses the previous block's height and timestamp to set the next target.
While waiting for that next block, wall-clock time alone does not lower its
difficulty. A two-day half-life is not a two-day outage recovery guarantee.
If hashrate falls to a fraction `f` of the amount that produced a ten-minute
mean at the current target, the next block's mean wait is `600 / f` seconds.
Under constant independent Poisson hashing, its p95 wait is
`-ln(0.05) * 600 / f` seconds:

| Remaining hashrate | Mean wait | p95 wait |
| --- | ---: | ---: |
| 100% | 10 minutes | 30 minutes |
| 10% | 100 minutes | 5.0 hours |
| 1% | 16.7 hours | 2.1 days |
| 0.1% | 6.9 days | 20.8 days |
| 0.01% | 69.4 days | 208.0 days |

These are conditional estimates for the **next block**, not measurements of
this network, guaranteed deadlines, or a simulation of subsequent ASERT targets.
Record tip age, target, observed block intervals and participating miners when
evaluating an outage. An online full node supplies validation and connectivity;
it does not replace departed hashing capacity. Prioritize independent sustained
mining and operational monitoring. Emergency difficulty changes would require
separate consensus design, simulation and coordinated activation; do not change
local difficulty parameters as an operational repair.

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

If a mainnet node has no established automatic outbound full-relay or block-relay
peer for 60 seconds, it retries seed discovery, doubling the interval up to
15 minutes after each retry. Stale known addresses, inbound peers, and temporary
address-fetch or feeler connections do not suppress recovery. An established
automatic outbound peer resets the backoff and postpones further retries.
Network deactivation also resets the timer, so reactivation allows a fresh
60-second recovery window. Initial discovery and retries share the same policy:
without a name proxy they query service-bit DNS (up to 32 addresses per seed),
falling back to a base-host address-fetch connection if DNS returns no addresses.
With a name proxy they request the base host through the proxy without a local
DNS lookup. The existing DNS worker handles one pending retry batch at a time;
slow DNS does not block the automatic connection worker. Retries require
network activity and automatic DNS seeding; explicit `-connect`
disables them. Manual `-addnode` connections alone do not suppress automatic
discovery; use `-dnsseed=0` to disable seed queries.
Inherited Bitcoin test networks retain their upstream seed retry behavior.

The default minimum chain work is
`0000000000000000000000000000000000000000000000000cfed9b27eee9a28`,
the cumulative work through height 8,000, block
`0000000000002714ba4d770189e83f6fb05d8bca671cd22849a8e52fc35d5495`.

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

## Version identity and node status

The display release is `31.0.0-rsat.1`, based on Bitcoin Core 31.0 with the
selected maintenance backports documented separately. `bitcoind --version`,
`bitcoin-cli --version`, startup logs and the GUI full version identify
Resatoshi and include the upstream base and source revision. A `-dirty` suffix
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

The GUI defaults to English. Select another language in the settings or use
`-lang=ko` for Korean; an existing language preference is preserved.

On a fresh data directory, the GUI asks both node and wallet users to choose
Index or Original blocks before synchronization starts. Original blocks also
lets users choose complete history or pruning after expiry processing. Existing
data directories continue with their recorded method. Startup options and
configuration take precedence. The server-only node uses command-line options.

For an ordinary expiry-mode upgrade, stop the old node cleanly, back up the
wallets, and continue with the same data directory. It remains in `index` mode;
the first upgrade checks the index and resumes from the existing tip if healthy.
Choosing `block` mode or recovering inconsistent index data after pruning
requires a fresh directory and blockchain synchronization. Preserve the original
directory and restore/load wallet backups into the new one: synchronization
does not restore wallet keys. See [upgrade guidance](release-notes.md#upgrade-guidance-for-expiry-modes).

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

On first GUI startup with the new settings, select the existing Resatoshi data
directory in the directory chooser, even if it is already present. If you used
a custom directory, choose that same directory to retain your wallets and chain
data. An explicit `-datadir` continues to bypass this chooser. GUI preferences
such as window geometry must be set again; no wallet or chain data is moved.

BTC, mBTC, bits and sat units and their conversion factors remain unchanged.
OS installation, autostart and payment-link registrations use Resatoshi-specific
identifiers to coexist with Bitcoin Core. New payment links use `resatoshi:`;
manually supplied legacy `bitcoin:` links remain accepted, with addresses checked
against the selected network. Resatoshi does not register the `bitcoin:` OS handler.
The executable names (`bitcoind`, `bitcoin-cli`, `bitcoin-qt`) remain unchanged.
When installing from source alongside Bitcoin Core, use a separate installation
prefix (for example `-DCMAKE_INSTALL_PREFIX=/opt/resatoshi`) so those executable
names do not overwrite an existing Bitcoin installation.

## Test modes

`-regtest` provides a local regression-test chain. `-regtest -test=recycle`
enables recycling with a 200-block lifetime for tests. The inherited testnet3,
testnet4 and signet configurations retain their Bitcoin test-network parameters;
they are not Resatoshi mainnet. The default `-chain=main` selects Resatoshi.
