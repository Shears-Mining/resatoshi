### QoS (Quality of service) ###

This inherited Linux script uses tc to limit outbound TCP traffic on port `8333`, excluding LAN destinations. To use it for Resatoshi mainnet, adjust the port rules in `tc.sh` to `19333` and configure the interface and network values for your host.

This means one can have an always-on bitcoind instance running, and another local bitcoind/bitcoin-qt instance which connects to this node and receives blocks from it.
