# Contributing to Resatoshi

Contributions to the node, wallet, GUI, tests, translations, and documentation
are handled in [this repository](https://github.com/resatoshi/bitcoin-resatoshi-31.0). The development branch is `main`.

## Issues

Search [existing issues](https://github.com/resatoshi/bitcoin-resatoshi-31.0/issues) before filing a report. Include the
version or commit, operating system, relevant configuration, reproduction
steps, expected result, and observed result. See [SECURITY.md](SECURITY.md)
for security reports.

## Patches

1. Create a branch from `main`.
2. Make a focused change and follow the [developer notes](doc/developer-notes.md).
3. Run checks appropriate to the change and include the results in the pull request.
4. Open a pull request against this repository's `main`, including GUI changes.

Explain what changes and why. Keep unrelated formatting or refactoring in
separate commits. Add tests for behavior changes, including consensus, wallet,
and recycle-state changes. Mark unfinished proposals as draft pull requests.

## Tests and review

After building with test dependencies enabled:

```sh
ctest --test-dir build
build/test/functional/test_runner.py
```

See the [unit test guide](src/test/README.md),
[functional test guide](test/functional/README.md), and [lint guide](test/lint/README.md).
For documentation changes, check commands, file paths, links, and agreement
with the source. Reviewers should describe the revision and checks they reviewed.

## Translations

Submit Resatoshi translation changes here. See the
[translation guide](doc/translation_process.md) for file names and extraction.

## Attribution

Keep existing copyright notices and credit upstream sources. Bitcoin protocol
names, BIP references, third-party library names, executable names, and
historical upstream records retain their established names; see the
[documentation conventions](doc/documentation.md).
