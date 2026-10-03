# ReSatoshi release validation

Validate one recorded source commit before publishing a release. A successful
build on one operating system does not establish support on another. Keep the
test logs, toolchain versions, configuration and artifact SHA-256 hashes with
the release record. This procedure does not enable hosted CI.

## Local validation records

Maintenance checks run locally; this procedure does not configure or start
GitLab CI. Store a reviewable JSON record under `doc/validation/` in a follow-up
commit referencing the exact tested source commit. Record whether the build was
fresh or incremental, the test commands, pass/fail/skip counts, compiler and OS,
artifact SHA-256 hashes, and checks not performed. A result record must never
claim to test its own future commit hash.

Retain the matching logs and source archive separately. List their checksums
and whether the artifacts are actually published; a checksum without the file
is not public access to evidence. Maintainer-reported local results are not an
independent audit, hosted pipeline attestation, reproducible-build proof or
verification of a separately distributed binary. Incremental maintenance
checks do not replace the fresh release build procedure below.

## Freeze the source

Include new source and test files in the reviewed commit. Check that
`git status --porcelain --untracked-files=all` is empty, then record
`git rev-parse HEAD`. Clone the repository into an empty directory and check out
that exact commit. Use a new build directory; do not copy generated headers,
objects or executables from a development build. Installed dependencies may be
reused, but record their origin and versions.

Verify that `--version` reports the selected commit without `-dirty`. Create a
source archive from that commit using `git archive`, and record its checksum.
Any subsequent source fix requires a new commit and the affected checks again.
See [upstream maintenance](upstream-maintenance.md) for the fork invariants and
the required recycling, wallet, RPC, snapshot, kernel and networking checks.

## Linux

Follow [the Unix build guide](build-unix.md). Build the node, CLI and tests from
the fresh checkout. Run CTest and the affected functional tests using isolated
test datadirs. For a GUI release, also build the GUI and Qt tests and record the
Qt version and whether QR encoding, DBus and other optional features are enabled.

## Windows

Follow [the Windows build guide](build-windows.md). A cross-build checks the
Windows compiler and linker paths, but the resulting executables must also run
on Windows. Record the Windows version and architecture.

Run the unit and Qt test executables on Windows. Start the GUI with a new,
explicit regtest datadir, `-connect=0`, `-listen=0`, `-dnsseed=0` and `-nosettings`.
Use a dedicated test account or back up and restore the application's existing
GUI preferences: `-nosettings` disables node settings, not all Qt preferences.
Create a temporary wallet, generate regtest blocks, check balances and recycling
status, shut down and reopen the same test datadir. Never use a production wallet
or datadir for this check.

If distributing an installer, test install, launch, coexistence with Bitcoin
Core and uninstall in a disposable Windows account or VM. A portable executable
test does not establish installer correctness. Preserve unrelated wallets and
settings, and record the signing status without describing an unsigned package
as signed.
Add sentinel user files under the installed `daemon` and `share` directories
before uninstall and verify they survive. Check that an edited `bitcoin.conf`
survives both reinstall and uninstall, and that only empty folders are removed.
Check Windows file properties and the installed-app listing for the ReSatoshi
display release; numeric file and protocol versions retain the upstream base.

## macOS

Follow [the macOS build guide](build-osx.md) on a Mac. Record the macOS version,
architecture, Xcode compiler and dependencies. After installing the documented
dependencies, a separate build can be configured as follows:

```sh
cmake -S . -B build-release-check -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_GUI=ON -DBUILD_GUI_TESTS=ON -DBUILD_TESTS=ON \
  -DBUILD_KERNEL_LIB=ON -DBUILD_KERNEL_TEST=ON
cmake --build build-release-check -j 2
ctest --test-dir build-release-check --output-on-failure --timeout 900
python3 build-release-check/test/functional/test_runner.py -j 2 \
  feature_recycle feature_recycle_spend feature_recycle_interfaces \
  wallet_sendall_recycle wallet_lockunspent_recycle rpc_getblockstats_recycle \
  mining_mainnet p2p_resatoshi_bootstrap
```

Run the GUI tests in a logged-in graphical session; record skipped tests. Also
exercise the actual packaged app with a temporary regtest wallet, restart,
Bitcoin Core coexistence, URI registration and removal in a disposable account.
Check the package's signing and notarization status separately. Validate each
architecture that will be advertised as supported.

If no Mac is available, report macOS as **not validated**. Documentation review,
cross-compilation and Linux Qt tests do not replace macOS runtime and packaging
checks. Do not present an untested macOS package as a validated release.
