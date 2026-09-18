# Resatoshi peer discovery

To connect to the Resatoshi bootstrap node, add these hostnames to
`bitcoin.conf`:

```ini
addnode=resatoshi-seed.freeddns.org:19333
addnode=resatoshi-seed.duckdns.org:19333
```

Both hostnames use the mainnet P2P port `19333`. Additional Resatoshi peers
can be supplied with `-addnode` or `addnode=` in `bitcoin.conf`.

The current `main` source has no built-in mainnet DNS or fixed seed entries,
so the settings above supply bootstrap peers explicitly. The published
31.0.12 one-touch miner includes these two hostnames in its mainnet seed list.

The inherited testnet and signet configurations have separate parameters.
See [network rules](resatoshi.md) and
[chain parameters](../src/kernel/chainparams.cpp) for the current configuration.
