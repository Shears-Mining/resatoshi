# ReSatoshi header synchronization

Mainnet uses the same genesis-anchored ASERT calculation in full header validation
and in both phases of low-work header synchronization. The calculation uses the
**previous** header's height and timestamp, the genesis target, the 600-second
spacing and the existing 172,800-second half-life. The virtual parent of genesis
is one target interval before genesis. This does not change consensus targets.

`HeadersSyncState` keeps the genesis anchor from its known chain start, including
when synchronization starts above genesis. During redownload it retains the last
received timestamp independently of the lookahead queue. This is needed when a
small queue has been drained between batches. Networks without active ASERT
retargeting keep their existing transition checks; `fPowNoRetargeting` retains
precedence. Full validation still performs its own exact target check.

Presync does not become full header validation: the caller checks header PoW and
continuity, and subsequent contextual validation checks the remaining rules.
The exact ASERT check rejects wrong-but-in-range targets earlier; it does not
make low network hashpower or malicious bootstrap peers harmless.

## Buffer and commitment parameters

Mainnet uses a commitment period of **191** and a redownload buffer of **6,000**
compressed headers. These are local anti-DoS parameters, not wire-format or
consensus changes. Testnet3, testnet4, signet and regtest retain their parameters.
The minimum chainwork floor also remains unchanged, at the observed height-8000
chainwork documented in [network rules](resatoshi.md).

Reproduce the September 29, 2026 analysis with:

```sh
python3 contrib/devtools/headerssync-params.py \
  --genesis 2026-09-04 --minchainwork-headers 8001 \
  --horizon 2028-10-10 --seed 0

python3 contrib/devtools/headerssync-params.py \
  --genesis 2026-09-04 --minchainwork-headers 8001 \
  --horizon 2028-10-10 --evaluate 191 6000
```

The 8,001 headers include genesis. The optimizer selects `(191, 5437)` under its
model; the implemented buffer rounds up to 6,000 for additional margin. The tool
retains its upstream Bitcoin defaults when no arguments are supplied. Explicit
arguments and a fixed random seed make the fork profile reproducible.

Using the tool's unchanged 6 Gbit/s aggregate attacker bandwidth, 600-second
block interval, 20% permitted additional header-memory growth and 48-byte
compressed header model:

| Parameters | Main-chain memory per peer | Long low-work-chain memory per peer | Expected forged headers accepted per attempt |
|---|---:|---:|---:|
| Inherited `(641, 15218)` | 713.345 KiB | 75.720 KiB | 3.37698744498e-5 |
| Selected `(191, 6000)` | 281.255 KiB | 254.119 KiB | 3.73925343042e-8 |

For this profile the model's target is less than `2.88036e-7` forged headers per
attempt. The selected configuration is about 7.7 times below that budget. These
numbers describe the existing probabilistic model, not measured attack rates or
a guarantee against attackers with enough proof of work. In particular, its
assumption about the honest headers needed to demonstrate the work floor is not
a universal lower bound for every possible ASERT chain. The long-chain memory
bound continues to use the conservative six-blocks-per-second MTP bound, rather
than assuming ASERT eliminates all unusual timestamp histories. Reported memory
also excludes allocator, connection and other node overhead.

Re-evaluate the profile when changing the work floor, planning horizon or header
representation. Do not update these constants solely because a date looks old.

## Regression coverage

`headers_sync_chainwork_tests` compares the shared target helper with the
pre-refactor ASERT expression, exercises different known fork points, batch
sizes and lookahead buffers (including zero), rejects in-range wrong targets
in both phases, and checks no-retarget and min-difficulty flag precedence.
Existing PoW, low-work-chain, initial-header-sync and mainnet mining tests remain
part of validation. Captured real mainnet headers can additionally be replayed
over local P2P and compared byte-for-byte with the previous binary; this must
use isolated data directories and must not change a production node's state.
