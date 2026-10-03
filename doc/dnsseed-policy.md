# Resatoshi peer discovery

To connect to the Resatoshi bootstrap node, add these hostnames to
`bitcoin.conf`:

```ini
addnode=resatoshi-seed.freeddns.org:19333
addnode=resatoshi-seed.duckdns.org:19333
```

Both hostnames use the mainnet P2P port `19333`. Additional Resatoshi peers
can be supplied with `-addnode` or `addnode=` in `bitcoin.conf`.

The current `main` source includes `seed.resatoshi.org` and both bootstrap
hostnames above in its mainnet DNS seed list. The crawler at `seed.resatoshi.org`
supports service-bit queries; the upstream address-fetch fallback handles
bootstrap hostnames that do not. No mainnet fixed seeds are configured.
The `addnode` settings above are an optional explicit connection method.
Older binaries retain the seed lists compiled into their own versions.

These hostnames do not by themselves establish independent operators. See the
[bootstrap documentation](resatoshi.md#bootstrap-and-minimum-chain-work) for
the discovery controls and the conditions under which retries occur.

The inherited testnet and signet configurations have separate parameters.
See [network rules](resatoshi.md) and
[chain parameters](../src/kernel/chainparams.cpp) for the current configuration.
