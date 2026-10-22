# thunder-app: our app on the Thunder drop-in

`thunder-app` is Thunder with our sidechain app: the sidechain template's window, trusted dialog and interface kit,
with pages for Thunder's own data model (UTXOs and utreexo accumulator, its withdrawal bundles, eCash as the
mainchain). The node is `thunder`, next to the app, unchanged: the app starts it as its child and talks to its app RPC.
Consensus, P2P and the LayerTwo JSON-RPC (thunder-rust 0.18.2, 37 methods) are the drop-in's own and stay byte for byte
what they are; the app layer is off unless the node is started with `--bt-app-rpc-port`.

The design shared by every drop-in app (why a thin client, the app RPC, the privileges, one backup phrase by BIP-85,
the pack, security, tests) is [core/doc/app.md](../core/doc/app.md). This page is Thunder's part.

## Build and run

```
cmake -S . -B build-app -DBT_BUILD_APP=ON -DBT_KIT_DIR=<sidechain-template repo>   # GTK 3 + WebKitGTK 4.1, npm
cmake --build build-app -j4
build-app/chains/thunder/thunder-app                  # betanet, the enforcer at http://localhost:50051
build-app/chains/thunder/thunder-app --network regtest --mainchain-grpc-url http://127.0.0.1:50051
cmake --build build-app --target thunder-app-pack     # build-app/pack/thunder: the pack for the unified wallet
```

`app/kit.commit` pins the sidechain template (branch `dropin-kit`); the template repository must have that commit
(`git -C <template> fetch https://github.com/block-fabric/sidechain-template <commit>`). The app's own options:
`--network`, `--datadir`, `--mainchain-grpc-url`, `--rpc-port`, `--p2p-port`, `--app-rpc-port`, `--node-arg=<arg>`
(passed to the node), and the template's (`--webuiconfig`, ...). They are also in Settings → Node (a change of
network, mainchain interface or data folder is asked in the trusted dialog, then the node restarts).

## The pages

The drop-in pages (`core/app/ui`) in the kit's places: Home (balance, the network and the eCash link, Drivechains,
Thunder's throughput card, the latest activity), Overview, Send (a fee in satoshis, as Thunder takes), Receive (an
address, its QR code, its deposit address `s9_<address>_<check>`), Activity (sends, receipts, deposits, withdrawals and
their bundles, block fees; CSV), Drivechains (deposit from any eCash wallet or through the mainchain interface's
wallet; withdraw to an eCash address; the bundles and their votes; merged mining on test networks), the block
explorer (blocks, transactions, addresses), the node (peers), Settings → Node and → Wallet (the backup phrase).
Thunder's own (`app/chain`): **Batch pay** (`create_transfer_many`: typed, pasted or read from a CSV file, each line
checked, one question for all) and **Throughput** (transactions per block, how full against the body limit, fees,
the mempool).

eCash naming: amounts in ECX (eCash), slot 9 on eCash; the mainchain is eCash through an enforcer-compatible gRPC
interface: Backbone (`backboned -chain=ecash|ecashbeta|ecashtest -grpc`) or eCash's bip300301 enforcer. The node's
networks are LayerTwo's (`betanet`, `forknet`, `signet`, `regtest`); betanet is the one of eCash's betanet
(`ecashbeta`). The kit's alert banner shows the link's state (down, catching up) and the node's alerts.

## What the pages use of the node (app/manifest.json)

- **Reads**: the LayerTwo reads by name (the same handlers and JSON): `balance`, `format_deposit_address`,
  `get_best_mainchain_block_hash`, `get_best_sidechain_block_hash`, `get_block`, `get_block_hash`, `get_bmm_inclusions`,
  `get_stxos`, `get_transaction`, `get_utxos`, `get_wallet_addresses`, `get_wallet_utxos`, `get_withdrawal_bundle`,
  `getblockcount`, `latest_failed_withdrawal_bundle_height`, `list_peers`, `list_utxos`, `openapi_schema`,
  `pending_withdrawal_bundle`, `sidechain_wealth`; and `getmainchaininfo`, `getbalances`, `getblockchaininfo`,
  `getnetworkinfo`, `uptime`.
- **Confirmed** (only through `rpc.confirmed`, asked in the trusted dialog): `create_transfer [dest, value_sats,
  fee_sats]`, `create_transfer_many [{dest: sats}, fee_sats]`, `create_withdrawal [ecash_address, amount_sats,
  fee_sats, mainchain_fee_sats]`, `create_deposit [address, value_sats, fee_sats]`, `mine [fee_sats?]`.
- **Host**: the shared `dropin.*` (core/doc/app.md §2) and `thunder.throughput` (exercises `get_block`).

The tests check this file equals what the node serves (`AppLayer::manifest`).

## Tests

- `tests/test_thunder_appui.cpp`: every LayerTwo method's reply byte-identical with and without the app layer (37
  methods, bad and good parameters), the app RPC's reads equal to the LayerTwo results, the phrase made only in a
  question / restored / refused over another, the unified wallet's phrase set by its own user (and refused to the
  app's), wallet ownership only for a wallet reader, the manifest.
- The interface: `npx vitest run src/chain` and `npx svelte-check` in `build-app/thunder-app-ui/src/webui/app`.
- `tests/e2e/run_app.py` (a build with `-DBT_APP_TEST_HOOKS=ON`; xvfb-run): a local eCash regtest (backboned
  `-regtest -ecashregtest=betanet -grpc`, slot 9 proposed and acked), thunder-app runs node A, a headless Thunder node
  B is the other side: the app starts, the backup phrase is made in the trusted dialog, 10 ECX deposited, 1.5 ECX sent
  to B, 0.25 ECX received from B, 2 ECX withdrawn and paid on eCash; then A's LayerTwo replies byte-identical without
  and with the app layer. Screenshots of every page, dark and light.

```
python3 tests/e2e/run_app.py --bin build-app/chains/thunder --backboned <backboned> --tmp <dir> --shots <dir> --base-port 49800
```
