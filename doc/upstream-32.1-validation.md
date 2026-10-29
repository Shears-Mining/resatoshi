# Bitcoin Core 32.1 integration validation

This record covers ReSatoshi main
`4f0d4ebb15af9bfb468ebb2632486eb694b714e8`, tested on 2026-10-29 against previous
main `e2505c0470c283b5ddbbcd662f42d42b25509127` (31.0 with selected backports).
The follow-up changes only documentation and evidence; it does not change node
code. These are local checks, not hosted CI or a Windows/macOS package release.
Production binaries, data directories and services were not replaced or stopped.

## Historical mainnet agreement

The same exported mainnet history was fully replayed from genesis into four
fresh, isolated directories: old main/index, old main/block, new main/index and
new main/block. This is offline block import, not a live P2P initial download.
All four used `-assumevalid=0`, disabled peer discovery/connections, and passed
`verifychain 4 0`. The new nodes used `-prevoutfetchthreads=2`.

| Compared value | Identical result on all four nodes |
| --- | --- |
| Height | 13,600 |
| Best block | `0000000000001930f171182f11a1fc1b3a16899544d4acc1810894e37d5951f2` |
| Genesis | `0000000379813245fcdd4600969c6f74684f14433badf837be5b8ffa560174ec` |
| MuHash | `827edd21f68a695011abe996001c49c8e54d13b0214cab70bdb27cfbe85b7757` |
| `hash_serialized_3` | `d1049edc346be919780936eeb6f9f8b6ca5bc7801aaaffdb6f03d439ee9f7ba1` |
| UTXO outputs / transactions | 10,938 / 10,623 |
| UTXO total amount | 680,000.00000000 |
| UTXO `bogosize` | 790,005 |
| Recycle pool balance | 0.00000000 |
| Mainnet expiry lifetime | 5,256,000 blocks |
| External connections during replay | 0 |

The export SHA-256 is
`b7d9d630334ac2fa6cdd8ba11392ee6d98bf681c2764b38c779781a12de66bf8`.
Block records use mainnet message magic `47d92434`, little-endian 32-bit payload
length, then raw serialized block. Genesis is initialized by each node; records
cover heights 1 through 13,600. Blocks were read through production RPC without
changing its settings. The two versions independently validated those blocks.

See [all four results and commands](upstream-32.1-validation/chain-comparison.json).
Binary SHA-256 values:

| Binary | SHA-256 |
| --- | --- |
| Old main `bitcoind` | `c19e89f67b6ba6ff2d70b11321ded33d03382087ec759964f85a544497b04e39` |
| New main `bitcoind` | `a1412324b806322ab58d77e415025dfaee5fcf54274ead444faba115918a00b7` |

The historical chain is younger than its expiry lifetime, so its pool is zero.
This comparison establishes agreement for this history, not proof of all future
consensus behavior. Nonzero-pool expiry, renewal, rollback and restart are covered
separately below. Historical replay also does not test an existing production
directory upgrade, a disk-format downgrade, or every old package version.

## Expiry and mixed-version P2P

The final new binary and the real previous-main binary were also tested with
`feature_recycle_mixed_versions.py`, using fresh regtest directories and actual
P2P block/transaction relay, without RPC submission of blocks to the peer:

| Candidate / previous main | Transport | Result |
| --- | --- | --- |
| index / block | v2 | Passed |
| block / index | v1 | Passed |

Each run mines competing branches, reorganizes across expiry in both directions,
renews and expires outputs, and restarts both versions. MuHash, UTXO counts and
totals, block identity and pool balances agree at every checkpoint. In particular,
the final height-405 state has an empty UTXO set and a nonzero pool of
144.99700000, preserved after restart. The regtest expiry lifetime is 200 blocks;
it does not change mainnet consensus parameters.

See [index/v2 checkpoints](upstream-32.1-validation/mixed-index-v2.json) and
[block/v1 checkpoints](upstream-32.1-validation/mixed-block-v1.json).

The final binary also passed `feature_recycle_full_block.py` against previous
main: 501 accepted blocks, eight rejected candidates, 22 state checkpoints and
three complete UTXO comparisons. This includes 4,000,000-weight blocks, 99,000
spendable outputs, 16,301 transactions in a block, 50,100 outputs expiring at one
height, reorganizations, restart and reindex. See the
[full-block result](upstream-32.1-validation/full-block.json).

## Final-source test matrix

The target is the exact source commit above, rebuilt as Linux RelWithDebInfo
using GCC 15.2.0, with GUI, wallet, IPC, kernel library/tests and ZeroMQ enabled.
Qt 6.10.2 and its dependency directories were supplied locally. Test nodes use
isolated directories and loopback sockets. Hosted runners were not used.

| Check on `4f0d4eb` | Result |
| --- | --- |
| Full build, including GUI and kernel tests | Passed |
| CTest, including optional script assets | 379 passed, 0 failed, 0 skipped |
| Complete base functional run (327 entries) | 308 passed, 19 skipped, 0 failed |
| Supplemental IPC and ZMQ tests | 3 passed, 0 failed, 0 skipped |
| Previous-main mixed versions | Both mode/transport combinations passed |
| Previous-main full-block/expiry comparison | Passed |
| Mainnet historical replay | Both versions, both modes passed |
| Full lint runner, including subtree checks | Passed |

The [per-suite CTest results](upstream-32.1-validation/ctest-results.json)
include the external script-assets SHA-256. An initial execution omitted
`DIR_UNIT_TEST_DATA` and skipped that suite; the complete execution above supplied
the assets and passed all 379 suites in one run. Build/lint invocations also
needed the existing local dependency library directories on `LD_LIBRARY_PATH`;
no source or lint rules were changed to resolve these environment issues.

The base functional run completed in 672 seconds. Three skips were caused by the
local Python dependency directory being absent from `PYTHONPATH`: `interface_ipc`,
`interface_ipc_mining` and `interface_zmq`. All three passed in supplemental runs
on unchanged source with that directory supplied. The combined result is **311
unique functional entries passed, 16 still skipped**, not a single run with 311
passes. The remaining skips concern unavailable previous-release binaries,
non-loopback bind-test addresses, USDT instrumentation, the benchmark target and
the chainstate utility. See [every result and skip reason](upstream-32.1-validation/functional-results.json)
and the [machine-readable matrix](upstream-32.1-validation/validation.json).

The 2026-10-28 broad checks were cumulative: two initial CTest failures and three
functional failures were corrected and rerun, followed by selected tests on
`f7e2fa2`. They must not be described as one initially green final-source run.
The 2026-10-29 matrix above is a separate execution on `4f0d4eb`.

## Release authentication

GnuPG verified all eight signatures on the official
[SHA256SUMS](https://bitcoincore.org/bin/bitcoin-core-32.1/SHA256SUMS) using
[SHA256SUMS.asc](https://bitcoincore.org/bin/bitcoin-core-32.1/SHA256SUMS.asc).
Verification returned exit status 0, with no invalid, expired, revoked or unknown
signatures. The signed source-archive checksum is
`a5f2f61e32cf7f790641c9ec306a3c88ca7b5a556946956b1f35ffd7e27813f9`, matching
the archive imported for this integration.

| Signer | Primary key fingerprint |
| --- | --- |
| Ava Chow | `152812300785C96444D3334D17565732E08E5E41` |
| Ben Carman | `0AD83877C1F0CD1EE9BD660AD7CC770B81FD22A8` |
| Michael Ford | `E777299FC265DD04793070EB944D35F9AC3DB76A` |
| Oliver Gugger | `F4FC70F07310028424EFC20A8E4256593F177720` |
| Matthew Zipkin | `E61773CD6E01040E2F1BD78CE7E2984B6289C93A` |
| sedited | `A8FC55F3B04BA3146F3492E79303B33A305224CB` |
| Sjors Provoost | `ED9BDF7AD6A55E232E84524257FF9BDBCC301009` |
| Will Clark | `67AA5B46E7AF78053167FE343B8F814A784218F8` |

Keys were obtained over HTTPS from the official
[guix.sigs builder-keys](https://github.com/bitcoin-core/guix.sigs/tree/main/builder-keys),
as recommended by upstream's `contrib/verify-binaries/verify.py`. All 41 downloaded
key files matched their Git blob hashes in the captured GitHub API listing.
An isolated keyring was used; no ownertrust was assigned. This authenticates the
signatures against those official-repository keys; fingerprints were not separately
authenticated out of band. The 31.0/31.1 historical archive signatures were not
verified in this follow-up.

Evidence: [signature summary](upstream-32.1-validation/release-signatures.json),
[key manifest](upstream-32.1-validation/builder-keys-manifest.json),
[GnuPG status](upstream-32.1-validation/gpg-status.txt),
[signed checksums](upstream-32.1-validation/SHA256SUMS) and
[detached signatures](upstream-32.1-validation/SHA256SUMS.asc).
To reproduce, import the corresponding public keys into a separate GnuPG home,
then run `gpg --homedir <isolated-keyring> --batch --no-auto-key-retrieve
--status-fd 1 --verify SHA256SUMS.asc SHA256SUMS`, check every `VALIDSIG`
fingerprint, and compare the downloaded archive's SHA-256 to the signed entry.

## Fork footprint

The exact tracked/exported tree at `4f0d4eb` was compared to the official 32.1
source archive with `git diff --no-index --no-renames`. Ignored local build files
were not included. Git archive's normal `export-subst` was applied.

Excluded vendor directories: `src/crc32c`, `src/crypto/ctaes`,
`src/ipc/libmultiprocess`, `src/leveldb`, `src/minisketch`, `src/secp256k1`, and
`src/univalue`. All other files, including translations, generated manual pages,
project documents, branding and hosted workflow differences, were counted.

| Scope | Files changed | Added lines | Removed lines |
| --- | ---: | ---: | ---: |
| All non-vendor files | 412 | 181,753 | 157,037 |
| Translations | 101 | 168,651 | 149,340 |
| Other `src/` files, including GUI and tests under `src/` | 130 | 5,330 | 477 |
| `test/` files | 63 | 3,541 | 4,462 |
| Remaining files | 118 | 4,231 | 2,758 |

This is the total fork difference from upstream, not the size of the 32.1
integration and not a count of consensus changes. Evidence-only files added by
this follow-up are outside the measured source commit. See the
[diff stat](upstream-32.1-validation/fork-footprint.stat.txt),
[numstat](upstream-32.1-validation/fork-footprint.numstat.tsv) and
[per-file hash inventory](upstream-32.1-validation/fork-footprint.json).

For the next update, keep mechanical upstream changes and fork adaptations in
separate reviewable commits where feasible, and publish this comparison with
explicit exclusions. Preserve published history. Inherited testnet3/testnet4/
signet metadata remains an explicit maintenance cost; a dedicated test network
is a separate design decision and was not introduced by this validation.

## Remaining release checks

Windows/macOS full builds and installation/removal, a complete extended
functional run, new sanitizer/fuzz campaigns, in-place production upgrades and
disk-format rollback were not performed. The Windows packaging fix has separate
local Make/Ninja/NSIS evidence, not a full Windows node build or installation.
Source integration and network interoperability do not certify those release
paths or update existing all-in-one miner packages.
