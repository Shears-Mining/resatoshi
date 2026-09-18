# Resatoshi release preparation

Release sources, tags, and packages belong to [this repository](https://github.com/resatoshi/bitcoin-resatoshi-31.0).
This guide records the repository files used to prepare a release.

1. Select and record the source commit. Check the client version fields in
   `CMakeLists.txt` against the intended package version.
2. Build the target platforms using the [build guides](README.md#building).
3. Run the relevant unit and functional tests, including mainnet mining and
   recycle tests. Record which platforms and tests were exercised.
4. Update [source notes](release-notes.md) and document the package's actual
   changes. Regenerate help-derived files when executable help changes; see
   [developer tools](../contrib/devtools/README.md).
5. Record package checksums and attach the packages and notes to the matching
   GitHub release tag.

Use Resatoshi chain parameters and peer information for network-specific
release data. Upstream Bitcoin releases, signatures and seed lists describe
the upstream network. The historical upstream notes remain under
[upstream release history](upstream-release-history.md).
