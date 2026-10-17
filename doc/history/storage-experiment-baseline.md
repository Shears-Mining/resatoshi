# Storage experiment baseline

The comparison base is main commit `441d5e7`. The original checkout's available test binary passed 21 recycle cases and 16,795
assertions before changes. Its embedded revision was older/dirty, so the exact
`441d5e7` reference daemon is built separately for comparison. That initial run is baseline evidence,
not proof of equivalence with the experimental implementation.

## Historical minimum-work verification

On 2026-09-28 this block was over 1,700 blocks behind the locally validated tip.
Its 8,001 headers, including genesis, were separately checked from local block
files for ancestry, SHA-256d proof of work, ASERT targets and median time past;
the independently calculated work matched the node's RPC result. This is local
verification, not an attestation from an independent operator.

The configured minimum work is unchanged by the storage experiment. Current
operating instructions are in [node operations](../node-operations.md).
