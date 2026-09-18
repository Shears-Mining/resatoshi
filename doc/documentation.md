# Documentation conventions

The active guides in this repository describe Resatoshi. The project derives
from Bitcoin Core 31.0 and retains many source identifiers and tools.

- Use **Resatoshi** for this project and its mainnet.
- Use the implemented executable names: `bitcoind`, `bitcoin-cli`, `bitcoin-qt`,
  `bitcoin-wallet`, `bitcoin-tx`, `bitcoin-util`, and the `bitcoin` wrapper.
- The configuration filename remains `bitcoin.conf`. Default data folders are
  `~/.resatoshi`, `~/Library/Application Support/ReSatoshi`, and
  `%LOCALAPPDATA%\ReSatoshi`.
- Mainnet ports are `19333` (P2P) and `19332` (RPC).
- Some inherited interface labels, RPC help, examples, and GUI units still use
  Bitcoin or BTC names. These names do not select the Bitcoin mainnet; chain
  selection is controlled by the node configuration.
- BIP names, the PSBT format name, protocol specifications, upstream pull
  requests, and upstream version history retain their original attribution.
- Numbered historical release notes under `doc/release-notes/` document
  Bitcoin Core releases. They are not Resatoshi release announcements.
- Bundled third-party libraries retain their own documentation and licenses.

Inherited technical examples may use Bitcoin addresses, transactions, block
hashes, test networks, or captured command output to illustrate an interface.
For live mainnet commands, use addresses and hashes from your Resatoshi node.
The [mainnet rules](resatoshi.md) describe this chain's parameters.
