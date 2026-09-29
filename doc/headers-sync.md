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

The tool accepts ISO dates and timestamps; missing offsets mean UTC and explicit
offsets are normalized to UTC. Invalid optimization horizons fail with a usage
error. Observed header counts must fit the model's horizon bound, including
in evaluation mode; optimization additionally requires a strictly positive gap.
Probabilities below floating-point resolution, or reciprocal attack costs beyond
its finite range, fail explicitly before printing a result rather than being
presented as zero risk or infinite attack cost. Run the CLI regression tests with
`python3 contrib/devtools/test_headerssync_params.py`.

For a more conservative attack-cost assumption, keep the observed chain length
for memory estimation and specify the attempt cost separately:

```sh
python3 contrib/devtools/headerssync-params.py \
  --genesis 2026-09-04 --minchainwork-headers 8001 \
  --horizon 2028-10-10 --attack-cost-headers 2000 --evaluate 191 6000
```

A new low-work presync from an untrusted peer starts only on a full 2,000-header
message (`TryLowWorkHeadersSync` with the default `max_headers_result`). This
provides a conservative bandwidth cost floor for that path, without assuming
all ASERT chains need 8,001 headers to reach the work threshold. It is not a
claim about trusted peers bypassing this mechanism, chains with sufficient real
PoW, or other attack paths. Changes to the batch size require re-evaluation.

The selected parameters yield the same modeled rate `3.73925343042e-8` with
this cost floor, against a tighter budget of `7.2e-8` (about 1.93 times below
budget). Memory estimates remain unchanged. A search without the optional
convexity shortcut found `(184, 5597)` under that budget, saving about 17.5 KiB
of modeled peak memory per peer but leaving only about 0.4% budget margin.
We retain `(191, 6000)` for the additional margin. This remains a model-based
tradeoff, not a real-world attack safety factor. Without `--attack-cost-headers`,
the tool preserves the original cost assumption from `--minchainwork-headers`.

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

Additional state-machine tests cover timestamp extremes and target saturation,
correct-target alternate chains, and rejection of an entire batch when its last
header has an incorrect target. These use synthetic headers to isolate difficulty
and commitment checks; they do not assert MTP validity or mine real PoW. The
alternate-chain test checks 128 commitment bits, retaining the existing random
hash salt (accidental collision probability 2^-128 under the bit model).

### Calculator resource budgets and transient memory

The calculator bounds cumulative alignment, probability-tail and candidate-scan steps with
`--max-work` (default 100,000,000), and bounds the optimizer's candidate list
with `--max-candidates` (default 1,000,000). Exceeding either budget exits with
an error and does not emit selected C++ parameters. Optimizer progress printed
before an error is provisional. Budgets can be increased explicitly for an
intentional larger local calculation; large profiles such as the upstream default
optimization may need a higher `--max-work`. These are deterministic work/allocation
bounds, not wall-clock deadlines or process RSS limits. Alignment iteration
uses a lazy range. The limits do not change the probability model or silently
truncate a search to produce a result.

The table's memory figures describe modeled commitment bits and retained
compressed headers, not a measured process RSS ceiling. During redownload,
a whole incoming batch is checked before ready headers are removed. With a
6,000-header buffer and a 2,000-header batch, the queue can temporarily hold
8,000 compressed headers (375 KiB at 48 bytes each), while still below the
work threshold. Incoming and returned full-header vectors, commitments,
container allocations and other peer state add memory beyond that figure.
This transient allowance does not change consensus or the selected parameters.

The `headers_sync_asert` fuzz target constructs exact ASERT targets with a
reachable work threshold. Every input checks presync, redownload, release
before the final batch, complete ordered replay, and rejection of a late
invalid target in either phase. The original `headers_sync_state` target
retains arbitrary malformed-header exploration. These state tests do not
replace caller-level proof-of-work, MTP or network validation tests.


`p2p_headers_presync_asert` complements the direct state target by passing
serialized `headers` messages through the connection test transport and
PeerManager. It selects one of the three peer connection types, sends 2–16
full batches of exact ASERT headers to that same peer, and checks the reported
presync height after each message. A final full batch with a wrong target in
its last header must end that presync. Proof of work uses the fuzz framework's
deterministic shortcut, including for the malformed target, so the failure
exercises difficulty-transition validation. Generated work remains below the
test floor and the permanent block index must never grow. The original
`p2p_headers_presync` keeps arbitrary headers, compact blocks and blocks.
This target intentionally stays below the work floor; the direct state target
covers redownload and buffer release separately.
