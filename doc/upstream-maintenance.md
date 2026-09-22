# ReSatoshi upstream maintenance

## Source baseline

The source baseline is the official **Bitcoin Core 31.0 release archive**, not
Bitcoin Core master. The initial import is
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

All files shared by the original import and the 31.0 archive matched byte for
byte. Nine omitted vendored source/build/test files are restored unchanged.
The two omitted minisketch hosted-CI configurations remain excluded. Archive
checksums were verified; release signatures were not verified in this audit.
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

## Selected 31.1 backports

This is **31.0 with selected fixes**, not a complete upgrade to 31.1. Keep the
version distinction when publishing packages. Sources and rationale are listed
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

Associated upstream tests and private-broadcast test helpers are included,
including the SOCKS5 clean shutdown fix (34863). The unrelated `connect_nodes`
API change is not included. A Linux `/dev/full` test additionally verifies that
settings writes fail correctly. Existing local Boost compatibility changes
remain; reconcile them with upstream 35175 during the next full update.

The 31.1 chainstate-compaction changes and unrelated release/CI changes are not
part of these backports. No hosted CI is enabled by this procedure.

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
- Recycle undo appends an internal entry to `CBlockUndo::vtxundo`. Consumers must
  distinguish this from actual non-coinbase transactions. Do not assume that
  its size always equals the number of non-coinbase transactions.
- Preserve wallet expiry handling, including explicit-input sendall, manual
  coin selection, persistent locks, restart and reorg behavior.
- Retain BTC/mBTC/bits/sat values and unit conversions. Keep separate GUI
  settings, OS installation/autostart identifiers and `resatoshi:` registration
  so the applications can coexist. Legacy manually entered `bitcoin:` links
  remain supported.

## Local validation

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

Before distributing GUI binaries, build Qt and run the Qt tests; exercise
Windows/macOS installation, coexistence and removal on their target platforms.
Node tests do not substitute for these checks.

Known optional limitation: `BUILD_KERNEL_LIB=OFF` in the validated build.
The public libbitcoinkernel spent-output API still exposes the internal recycle
undo entry as a transaction entry. Fix and test that contract before enabling
or shipping external kernel-library integrations. Do not advertise that API as
ReSatoshi-ready based on the node tests.
