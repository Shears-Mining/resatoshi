# Resatoshi peer discovery

The current mainnet configuration has empty DNS seed and fixed seed lists.
Peer addresses can be supplied with `-addnode` or `addnode=` in `bitcoin.conf`.
Use Resatoshi mainnet peers and the default P2P port `19333`.

The inherited testnet and signet configurations have separate parameters.
See [network rules](resatoshi.md) and
[chain parameters](../src/kernel/chainparams.cpp) for the current configuration.
