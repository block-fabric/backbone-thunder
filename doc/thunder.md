# Thunder (eCash slot 9): drop-in client

`thunder` replaces thunder-rust's `thunder_app` v0.18.2 on eCash betanet. Consensus,
P2P, mainchain link, CLI flags and JSON-RPC follow thunder-rust tag v0.18.2 (244e20f), the
rules every betanet block was made under (study Part II Thunder §1).

## Status (S1)

| Milestone | State | Evidence |
|---|---|---|
| M1 codecs/hashing | done | borsh+bincode for every type; BLAKE3; CKB CBMT; FROST verify; 20-byte BLAKE3-XOF addresses. Genesis `8b52259a…` and block 1 `c5bd920a…` recomputed; all 34 public betanet txs: borsh decode → re-encode identical → BLAKE3 = txid; all 2,243 public blocks: bincode re-encoding byte-identical, header hash = index id |
| M2 state machine | done | full replay of betanet slot 9 (heights 0–2242) with every check: 49 FROST signatures, 34 Utreexo proofs, roots in every header, 26 deposits, BMM in the child of `prev_main` for every block, withdrawal bundle `aa769fec…ced4` (ConsensusHex) rebuilt by us and matched to the enforcer's Submitted event at 970,438, next bundle Pending at 2191; sidechain wealth 101,206,236,589 sat = 101,206,335,589 deposited − 99,000 withdrawn; whole-history rollback to genesis and reconnect; 12 quirk tests |
| M3 mainchain link | done (client) | gRPC over h2c/TLS to `cusf.mainchain.v1`, all calls thunder-rust makes; verified live against `https://enforcer.beta.ecash.ninja` (mainchain synced from genesis, 973,887 header infos); never auto-retries non-idempotent calls |
| M4 drop-in surface | done (first version) | binary `thunder` with their flags/defaults, `thunder-cli` with their subcommands, 37 JSON-RPC methods with serde JSON shapes, QUIC P2P byte-exact (magic `85 18 95 04`, port 4009): from an empty datadir our node synced to the live tip (2245, `296e87bd…`) from the public seed peer in ~10 s after the mainchain sync, and their node keeps our connection (it checks our claimed total work) and dials us back |

## Drop-in surface

* **CLI** (app/cli.rs): `--add-peer`, `-d/--datadir` (default `<data_dir>/thunder`),
  `--headless` (accepted; no GUI), `--log-dir` (default `<datadir>/logs/v0.18.2`, daily
  `YYYY-MM-DD.log`), `--log-level` (DEBUG), `--log-level-file` (WARN),
  `--mainchain-grpc-url` (`http://localhost:50051`), `--mainchain-grpc-ca-cert`,
  `--mnemonic-seed-phrase-path`, `-n/--net-addr` (`0.0.0.0:4009`), `--network`
  (betanet|forknet|regtest|signet), `--network-magic`, `-p/--private-rpc-addr` and
  `-r/--rpc-addr` (both `127.0.0.1:6009`; different values split public and private
  methods exactly like theirs), `--server-name`, `--wallet-dir`. Unknown flags are logged
  and ignored. Ours all start with `--bt-`.
* **JSON-RPC** (rpc-api/lib.rs): openapi_schema; get_block (verbose); connect_block,
  get_block_hash, get_bmm_inclusions, get_best_mainchain_block_hash,
  get_best_sidechain_block_hash, get_stxos, get_transaction, get_utxos,
  get_withdrawal_bundle, getblockcount, latest_failed_withdrawal_bundle_height, list_peers,
  list_utxos, pending_withdrawal_bundle, sidechain_wealth, submit_transaction; private
  connect_peer, forget_peer, invalidate_block, remove_from_mempool, stop; wallet balance,
  create_deposit, create_transfer, create_transfer_many, create_withdrawal,
  format_deposit_address, generate_mnemonic, get_block_template, get_new_address,
  get_wallet_addresses, get_wallet_utxos, mine, set_seed_from_mnemonic, sign_transaction.
  Positional or named params; errors `{code:-1, message}` like their `custom_err`.
  JSON is serde's human-readable form (`thunder/json.h`): hex hashes, display-order bitcoin
  hashes, base58 addresses, externally-tagged enums, `[u8; 32]` as number arrays where serde
  writes them so (tx input `utxo_hash`, coinbase memo, `Some` node hashes).
* **P2P**: QUIC v1/TLS 1.3, no ALPN, fresh self-signed ECDSA P-256 certificate for
  `localhost` + `--server-name`s, unverified client, one bidi stream per message
  `magic || bincode`, heartbeat every second, request cap 100 KiB, block response 10 MiB,
  headers `2 KiB × (h+1)` capped at 64 MiB, tx ack 256 B; betanet seeds
  `seed.beta.ecash.{drivecha.in,eu.com,ninja}:4009`.
* **Mainchain**: the same gRPC calls (Health, GetChainTip, GetChainInfo,
  GetBlockInfo×999 with contiguity checks, GetBmmHStarCommitment, GetTwoWayPegData,
  SubscribeEvents for `mine`, CreateBmmCriticalDataTransaction, CreateDepositTransaction,
  CreateNewAddress, GenerateToAddress, ProposeWithdrawalBundle). Mainchain total work is the
  sum of the enforcer's per-block `work` from mainchain genesis, as theirs (peers ban on a
  mismatch). BMM before the reported activation height is taken as reported (betanet genesis
  BMM at 968,910 < 968,998).
* **Wallet**: BIP-39 English mnemonics, seed via PBKDF2-HMAC-SHA512; keys at
  `m/43'/1899'/0'/9'/0'/i` with bip32ish over ristretto255 (`--bt-wallet-derivation v0.18.0`
  for wallets made before their v0.18.1 bias fix); addresses from index 1; their coin
  selection. Their LMDB wallet is not read: import by mnemonic.
* **Datadir**: our files are `backbone-thunder.sqlite` and `backbone-wallet.sqlite` next to
  where theirs keep `data.mdb`/`wallet.mdb`; we resync from peers.

## Consensus quirks reproduced (tests in tests/test_thunder_quirks.cpp)

No intra-block chaining; authorizations not committed; deposits whose OP_RETURN is not
UTF-8 base58 of 20 bytes (incl. the `s9_…` form) credited to `Address::ALL_ZEROS`;
coinbase may create Withdrawal outputs; zero-input txs valid and replayable; `get_value =
value + main_fee` saturating; bundle = all withdrawal UTXOs aggregated by address (address
equality includes network), sorted by `(main_fee, value, address)` descending, `break` at
the first aggregate over 400,000 WU, inputs commitment over the BTreeMap order plus
`Regular{0³², side_height}`; pending bundle never expires; unknown bundle confirmed at side
height > 0 halts, at height 0 spends everything; 2WPD window
`(prev_main(N-1), prev_main(N)]` applied after block N, genesis window = all ancestors;
4-block failure gap with wrapping u32; limits `floor(START·1.04^month)` with `__powidf2`,
`START·111` from month 120; body size excludes Utreexo proofs (exceeds the 10 MiB P2P cap
from month 6); Empty vs `Some(0³²)` roots; rustreexo `verify` accepting proofs with no
targets and proofs whose unmatched computed roots equal the matched ones (a non-member
leaf can pass, see `quirk_utreexo_verify_accepts_half_matched_roots`); fork choice
preferring lower work at equal height in one mainchain lineage and keeping the first
genesis; `deposit_blocks` little-endian keys under memcmp order.

## Kept (consensus-neutral) and deviations

* No panics: their panic paths reject the block; crash-safe SQLite store; templates' roots
  computed by the validation path; mempool/serving off the QUIC I/O thread.
* RPC hardening (their server: permissive CORS, no auth): no CORS unless
  `--bt-rpc-allowed-origin`; Host header must be loopback/bind/`--bt-rpc-allowed-host`;
  `application/json` required (as jsonrpsee); non-loopback bind needs
  `--bt-rpc-allow-external`. Their clients (BitWindow/orchestrator, dc-hub, thunder-cli) use
  `127.0.0.1` with JSON bodies and no Origin.
* Mainchain: we poll `GetChainTip` (2 s) instead of holding a `SubscribeEvents` stream (only
  `mine` subscribes, reading the first event like `confirm_bmm`).
* Peer sync is simpler than their `handle_peer_state` decision table: we fetch any
  advertised tip that is not ours (after the same total-work and BMM checks, which ban) and
  let `better_tip` decide in `reorg_to_tip`; outcomes are the same, traffic may differ.
* Mainchain reorgs: a tip no longer BMM-verified on the active mainchain is disconnected
  with its 2WPD (v0.18.2 `4df63d3`); their `side_tips` table is not ported, so we wait for
  a peer to advertise the alternative tip instead of switching to it from the archive.
* `openapi_schema` lists the methods instead of utoipa's full schema.
* No GUI; `--headless` is implied.

## Remaining for Thunder

1. Live differential runs on the test machine (doc/differential-tests.md), especially RPC
   JSON byte-diffs, mining/BMM, bundles Succeeded/Failed and reorgs on regtest.
2. `side_tips` port and SubscribeEvents-driven mainchain task (reorg responsiveness).
3. Windows: the mingw64 cross build works (fully static `thunder.exe`, system DLLs only)
   but has not been run on Windows yet (paths, sockets, shutdown).
4. Performance work before chains grow: Txn scans copy tables; collect_withdrawal_bundle
   scans all UTXOs each block (as theirs does).
5. Known-peers learning from inbound peers; peer scoring beyond bans; gRPC GOAWAY handling.
6. Unified-app node kind and pages; seeding from our unified seed (user decision).
