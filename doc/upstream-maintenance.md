# Resatoshi upstream maintenance

## Source baseline

The source baseline is the official **Bitcoin Core 32.1 release archive**, not
Bitcoin Core master. The initial 31.0 import is
`0bb4cdfb0fec24b0bb0d087bc665373233efd045`; the main commit before the local
maintenance changes is `8584d07bb2ce8765ffb0dcac64eeaaa9dd5241e3`.
The import has independent Git ancestry. Do not assume `git merge` can find the
correct Bitcoin Core merge base, or rewrite published ReSatoshi history to hide
this difference.

Archive checksums (SHA-256, checked against the official HTTPS SHA256SUMS):

| Archive | SHA-256 |
| --- | --- |
| [bitcoin-31.0.tar.gz](https://bitcoincore.org/bin/bitcoin-core-31.0/) | `0ba0ef5eea3aefd96cc1774be274c3d594812cfac0988809d706738bb067b3e3` |
| [bitcoin-31.1.tar.gz](https://bitcoincore.org/bin/bitcoin-core-31.1/) | `50411d5b43c7e4c90099394759eb6c2add6e7c2dbe728840893d638b6fc6afc9` |
| [bitcoin-32.1.tar.gz](https://bitcoincore.org/bin/bitcoin-core-32.1/) | `a5f2f61e32cf7f790641c9ec306a3c88ca7b5a556946956b1f35ffd7e27813f9` |

The 32.1 archive was checked against the official HTTPS SHA256SUMS on
2026-10-28. Its release tag resolves to
`f0de87f040621468b956f01f526a43a010310501`. This update does not establish
Git ancestry with Bitcoin Core. On 2026-10-29, all eight signatures on the
32.1 SHA256SUMS were verified against public builder keys obtained from the
official guix.sigs repository. The signed checksum matches the imported archive.
See the [32.1 validation record](upstream-32.1-validation.md) for fingerprints,
key provenance, historical mainnet replay, test results and the fork footprint.

The 31.0-to-32.1 source changes were integrated against ReSatoshi main
`e2505c0470c283b5ddbbcd662f42d42b25509127` in a separate worktree.
Project governance, hosted workflow configuration, branded translations,
generated manual pages and ReSatoshi release notes remain project-maintained.
The original 32.1 notes are retained as
[upstream history](release-notes/release-notes-32.1.md).

### ReSatoshi adaptations for 32.1

- Keep genesis, mainnet discovery, ASERT, expiry modes, typed recycle undo,
  pool accounting and snapshot version 3. Taproot enforcement is now
  unconditional upstream, matching ReSatoshi mainnet's activation from genesis.
- Preserve the height index and recycle-pool state across the new coins-view
  interfaces and parallel prevout fetching. Test serial and parallel fetching
  with both expiry sources, failed-block reset and successful flush.
- Preserve external input origins in the wallet's new typed metadata and
  canonical witness-transaction representation, including writes after updates
  and reload. Ignore expired-input spenders when classifying non-mempool spends.
- Retain source revision reporting, empty-span stream handling, shared database
  snapshots, cancellation during resizing and separate installation settings.
  Adapt Windows installer generation to upstream's new script while retaining
  ReSatoshi registration and user configuration preservation.
- Adapt local functional tests to the revised RPC connection helpers. Old
  reference nodes use separate directories without the new prevout-fetch option.

All files shared by the original import and the 31.0 archive matched byte for
byte. Nine omitted vendored source/build/test files are restored unchanged.
The two omitted minisketch hosted-CI configurations remain excluded. Archive
checksums were verified. The initial audit did not verify release signatures;
the follow-up record above verifies the 32.1 signatures only.
Vendored snapshot commits are local imports, not proof of upstream Git ancestry.

The restored libmultiprocess, minisketch and secp256k1 trees are recorded with
`git subtree split --rejoin`. Their split commits describe the reviewed local
snapshots, including the minisketch hosted-CI exclusions; they are not upstream
release commit IDs. Existing published history and vendor contents are preserved.
For future archive-based vendor updates, first verify the release, review and
commit the exact vendor changes, then rejoin each affected subtree and check it:

```sh
vendor_dir=src/minisketch # Set to the reviewed subtree being updated.
git subtree split --prefix="$vendor_dir" --rejoin
test/lint/git-subtree-check.sh -r "$vendor_dir"
```

The check verifies consistency with the recorded split commit, not provenance
from an upstream repository. Do not rejoin unreviewed changes just to clear lint.

## Historical selected 31.1 backports

Before the 32.1 integration this was **31.0 with selected fixes**, rather than a
complete upgrade to 31.1. These backports are now absorbed by the new baseline.
Their sources and rationale are listed
in the [official 31.1 notes](https://bitcoincore.org/en/releases/31.1/).

| Upstream PR | Change |
| --- | --- |
| [35410](https://github.com/bitcoin/bitcoin/pull/35410) | Preserve the private-broadcast proxy across v2-to-v1 retries; prohibit direct private-broadcast connections. |
| [35032](https://github.com/bitcoin/bitcoin/pull/35032) | Do not update addrman services from private-broadcast connections. |
| [35384](https://github.com/bitcoin/bitcoin/pull/35384) | Detect settings-file write/close failures before replacement. |
| [35228](https://github.com/bitcoin/bitcoin/pull/35228) | Use the actual outpoint when estimating signed input size. |
| [35316](https://github.com/bitcoin/bitcoin/pull/35316) | Reject empty MuSig key lists. |
| [35227](https://github.com/bitcoin/bitcoin/pull/35227) | Check the final BDB page LSN during wallet migration. |
| [35209](https://github.com/bitcoin/bitcoin/pull/35209) | Keep precomputed transaction data alive until script-check control is destroyed. |
| [34953](https://github.com/bitcoin/bitcoin/pull/34953) | Avoid unsupported ASan instrumentation in SHA256 SSE4 assembly. |
| [34228](https://github.com/bitcoin/bitcoin/pull/34228) | Keep SOURCE_DATE_EPOCH out of depends toolset-cache identifiers. |
| [34425](https://github.com/bitcoin/bitcoin/pull/34425) | Wait for socket disconnection and avoid reconnect races in functional tests. |
| [34991](https://github.com/bitcoin/bitcoin/pull/34991) | Correct batch requests in the pruning/index test, including CLI mode. |
| [35080](https://github.com/bitcoin/bitcoin/pull/35080) | Scale the chainstate tool test timeout with the configured timeout factor. |
| [35218](https://github.com/bitcoin/bitcoin/pull/35218) | Correct the P2SH script in the coins-cache fuzz target. |
| [35279](https://github.com/bitcoin/bitcoin/pull/35279) | Exercise default address types in the PSBT test. |
| [35289](https://github.com/bitcoin/bitcoin/pull/35289) | Bound generated block weight in the orphan-transaction fuzz target. |
| [35044](https://github.com/bitcoin/bitcoin/pull/35044) | Fix the undefined variable in the Signet miner's template-mismatch warning. |
| [35283](https://github.com/bitcoin/bitcoin/pull/35283) | Document the explicit ZeroMQ build option in BSD guides. |
| [35465](https://github.com/bitcoin/bitcoin/pull/35465), [leveldb #61](https://github.com/bitcoin-core/leveldb/pull/61) | Disable read-triggered seek compaction and periodically compact chainstate in the background. |

Associated upstream tests and private-broadcast test helpers are included,
including the SOCKS5 clean shutdown fix (34863) and the `connect_nodes`
disconnection checks (34425). A Linux `/dev/full` test additionally verifies that
settings writes fail correctly. Existing local Boost compatibility changes
use `indexed_by` type aliases rather than derived classes, addressing the same
issue as upstream 35175. Their spelling differs from upstream's inline types.

Unrelated upstream release metadata and hosted-CI infrastructure changes are not
part of these backports. No hosted CI is enabled by this procedure.

### Chainstate compaction

The paired 31.1 database changes preserve Resatoshi's current-format state and format checks. Compaction changes LevelDB's physical layout,
not expiry heights, pool accounting, ASERT, or other consensus rules. After a
completed full flush outside initial block download, a 1/320 random trigger
starts a one-shot background compaction unless shutdown is requested. This is
not a fixed two-week timer. Cache resizing is serialized with compaction, and
database destruction waits for outstanding compaction work.

The LevelDB files come from the recorded 31.1 release archive. Keep LevelDB in
the subtree checks and record its reviewed snapshot; do not copy upstream's
release-branch lint exclusion. The recycle compaction regression test checks
typed expiry undo, pool balances, all coin records, the codec marker,
cache resizing, shutdown/reopen, recovery replay, and disconnection.

## Local stream compatibility fix

`AutoFile` skips C stdio calls for empty spans in `read`, `write` and
`write_buffer`. Empty spans may have null data pointers, which UBSan rejects at
the stdio boundary on the validated GCC/glibc environment. File-handle errors,
obfuscation position checks, write tracking and file positions are preserved.
The regression test covers plain/obfuscated files, empty I/O, EOF and invalid
handles. Reconcile this small local fix with upstream during future updates.

## Update procedure

1. Start from a clean, committed ReSatoshi checkout. Preserve local changes and
   new test files first. Record its commit and the last reviewed upstream base.
2. Obtain the next stable upstream release and verify its provenance. Review
   the **previous official base -> new official base** diff against ReSatoshi
   on a separate local branch/worktree. Account for the backports above; do not
   replace whole fork-modified files with upstream files. A clean text merge is
   not proof of semantic compatibility.
3. Keep upstream fixes and ReSatoshi adaptations in separate focused commits.
   Record the upstream release/PR or original commit for each backport. Update
   this document when the base changes; retire backports absorbed by that base.
4. Review the invariants below and run the relevant local checks. Commit all
   source/tests needed by a fresh clone before preparing a release. Check the
   exported Git tree rather than relying on ignored files in the working tree.
5. Record exact source commit, build options, tests, and remaining limitations
   in the release record. Publish only the reviewed source/packages.

## Fork invariants

- Preserve ReSatoshi genesis, network magic, ports, address encodings, peer
  configuration and independent chain identity. Bitcoin chain data is not a
  replacement for ReSatoshi chain data.
- Preserve ASERT's anchor, half-life, integer arithmetic and activation rules.
- Preserve expiry height boundaries, recycle-pool accounting, reward limits,
  and connect/disconnect/replay recovery. Check wallet, mempool, mining,
  statistics, indexes, snapshots and filters when their input assumptions change.
- Recycle undo is an explicit `CBlockUndo::recycle` field. `vtxundo` contains
  transaction inputs only. Pool changes must propagate through caches, final
  flush batches, recovery replay, snapshots and snapshot commitments.
- Preserve wallet expiry handling, including explicit-input sendall, manual
  coin selection, persistent locks, restart and reorg behavior.
- Keep the ReSatoshi display release, upstream base and source revision distinct.
  Do not change numeric client/protocol versions solely to rename a release.
  Preserve the opt-in recycling status fields and the default RPC response.
  Verify CLI named/positional arguments as well as direct JSON-RPC.
- Retain BTC/mBTC/bits/sat values and unit conversions. Keep separate GUI
  settings, OS installation/autostart identifiers and `resatoshi:` registration
  so the applications can coexist. Legacy manually entered `bitcoin:` links
  remain supported.

## Local validation

Completed 32.1 checks, their exact source commits and remaining limitations are
recorded in [32.1 validation](upstream-32.1-validation.md). Requirements below
describe the maintenance procedure; they do not imply every platform was tested.

Run from the repository root using a configured build with tests and wallet:

```sh
cmake --build build --target bitcoind bitcoin-cli test_bitcoin -j 2
build/bin/test_bitcoin --report_level=short
python3 build/test/functional/test_runner.py -j 2 feature_settings p2p_private_broadcast p2p_private_broadcast_retry_v1 feature_proxy rpc_net wallet_basic wallet_sendall_recycle wallet_lockunspent_recycle feature_recycle_spend rpc_getblockstats_recycle
```

These functional tests use isolated local test nodes and may calculate easy
regtest PoW. Do not run against a production datadir. Hosted runners are not
required. Also run affected migration, index, reindex, REST, RPC and P2P tests
when those paths change. Use the existing mainnet/ASERT tests for chain changes.
A release needs a fresh-checkout build so missing ignored files cannot conceal
packaging defects.
Use the [release validation procedure](release-validation.md) to record source
identity and platform-specific build, runtime and packaging evidence.

Before distributing GUI binaries, build Qt and run the Qt tests; exercise
Windows/macOS installation, coexistence and removal on their target platforms.
Node tests do not substitute for these checks.

The optional libbitcoinkernel spent-output reader exposes transaction undo directly.
Validate with `-DBUILD_KERNEL_LIB=ON -DBUILD_KERNEL_TEST=ON` and `test_kernel`.

The kernel suite includes `btck_recycle_spent_outputs_disk_tests`, processing
real regtest fixtures across expiry, reading transaction inputs, retaining
owned copies after chain-manager destruction, and reopening disk state. Its
short expiry lifetime is configured by a test-only white-box helper; no test
parameter is added to the public library API.

For the discovery retry and external integration paths, also run:

```sh
python3 build/test/functional/test_runner.py -j 4 p2p_resatoshi_bootstrap feature_recycle_interfaces feature_recycle_spend tool_utxo_to_sqlite rpc_getdescriptoractivity rpc_scanblocks
```

For recycle-index diagnostics, run `dbwrapper_tests,recycle_tests` and
`rpc_checkrecycleindex feature_recycle_index feature_recycle_boundary`.
Preserve shared-snapshot point reads and iterators, cancellation during database
resizing, bounded examples and the distinction between completion and validity.
These checks do not replace historical block replay or independent node comparison.

The bootstrap test routes every peer through local SOCKS proxies, deliberately
fails the first attempts, and checks automatic address discovery and block
synchronization after recovery. It also checks `-dnsseed=0` and `-connect=0`.
Its `--testnet4` variant verifies that the mainnet retry does not change inherited
test-network behavior.
The interface test covers oversized snapshot state, ordinary script bounds,
opt-in expiry events, rollback and restart. Basic filter semantics stay unchanged.
