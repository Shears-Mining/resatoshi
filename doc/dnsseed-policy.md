# Resatoshi peer discovery

To connect to the Resatoshi bootstrap node, add these hostnames to
`bitcoin.conf`:

```ini
addnode=resatoshi-seed.freeddns.org:19333
addnode=resatoshi-seed.duckdns.org:19333
```

Both hostnames use the mainnet P2P port `19333`. Additional Resatoshi peers
can be supplied with `-addnode` or `addnode=` in `bitcoin.conf`.

The current `main` source includes two DNS crawlers, `seed.resatoshi.org`
(Seoul) and `seed-fra.resatoshi.org` (Frankfurt), plus both bootstrap hostnames
above in its mainnet DNS seed list. Both crawlers support service-bit queries,
including `x9.seed.resatoshi.org` and `x9.seed-fra.resatoshi.org`.
The upstream address-fetch fallback handles bootstrap hostnames that do not
support those queries. No mainnet fixed seeds are configured.
The `addnode` settings above are an optional explicit connection method.
Older binaries retain the seed lists compiled into their own versions.

These hostnames do not by themselves establish independent operators. See the
[bootstrap documentation](node-operations.md#bootstrap-and-minimum-chain-work) for
the discovery controls and the conditions under which retries occur.

The inherited testnet and signet configurations have separate parameters.
See [network rules](resatoshi.md) and
[chain parameters](../src/kernel/chainparams.cpp) for the current configuration.
