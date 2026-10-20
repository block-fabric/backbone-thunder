# Differential tests for the test machine

Their binaries are not built or run on the development PC. On the separate test machine,
run thunder-rust **v0.18.2** (`thunder_app`, `thunder_app_cli`) and bip300301_enforcer
(@1753fc0c, regtest preset) next to our `thunder`, and compare. Each test records both
sides' outputs; any difference stops the run and saves the block/tx.

## A. Byte-level (no network)

1. **Codec fuzz**: random `Transaction`, `AuthorizedTransaction`, `Header`, `Body`,
   `OutPoint`, `Output`, P2P `Request`/`Response`: bincode decode/encode and borsh encode
   on both sides (their `thunder_types` in a small harness); compare accept/reject,
   re-encoded bytes, txid, block hash, merkle root.
2. **FROST**: random/malformed 32-byte points and 64-byte signatures (non-canonical
   encodings, identity, z ≥ L): accept/reject must match `frost-ristretto255`.
3. **Utreexo**: random add/delete sequences including duplicate leaves and deletions of
   roots; compare roots, serialized forests, `prove` output and `verify` results (incl.
   adversarial proofs: wrong hashes, duplicate targets, out-of-range positions).
4. **Limits**: `body_size_limit`/`body_sigops_limit` for months 0–130.
5. **Bundles**: random withdrawal sets (all address kinds/networks, equal fees/values,
   >400k WU) → m6id and tx bytes from `collect_withdrawal_bundle`.
6. **Addresses**: `bitcoin::Address<NetworkUnchecked>` parse/Display/Ord for random
   strings (bech32 case, invalid checksums, unknown HRPs, 50-char base58).
7. **Wallet**: for 20 mnemonics: first 50 addresses for both derivation variants
   (`set_seed_from_mnemonic` + `get_new_address` on theirs).

## B. Regtest, side by side (one enforcer feeding both nodes)

8. **Block template / connect_block**: generate blocks from each node's
   `get_block_template`, connect them on the other with `connect_block`; compare
   `get_block` JSON, roots, `list_utxos`, `get_stxos`, `sidechain_wealth`,
   `pending_withdrawal_bundle` after every block.
9. **Deposits**: valid, `s9_…` form, non-UTF-8, 19/21-byte addresses, several per block,
   in the BMM block itself.
10. **Bundles**: withdrawal → Pending → Submitted (M3) → Succeeded after the regtest
    threshold; Failed after max age; resubmitted failed bundle; unknown bundle Submitted and
    Succeeded at height > 0 (both must halt) and at height 0.
11. **Reorgs**: sidechain reorg via `invalidate_block`; mainchain reorgs over a side tip
    (with/without deposits and bundle events in the dropped blocks); equal-height
    competing tips (lower-work rule); two genesis blocks.
12. **Mempool/relay**: `submit_transaction`, `sign_transaction`, `create_transfer(_many)`,
    `create_withdrawal` on either side, propagated over P2P to the other (PushTransaction
    accepted/rejected answers), mined by the other.
13. **Mining**: `mine` on our node against the regtest enforcer (BMM request, first-event
    confirm, block submitted, bundle proposed); their node follows.
14. **Quirk blocks** crafted on regtest: zero-input replays, coinbase withdrawals,
    authorizations swapped for re-signed ones (same block hash), proofs with no targets,
    half-matching proofs, empty-root headers, >256 deposit blocks then a disconnect.

## C. Mixed network

15. Their node syncs from ours only (theirs `--add-peer` ours, no seeds) and ours from
    theirs only, on regtest and on betanet; heartbeat total-work checks must not ban either.
16. RPC JSON byte-diff for every method (both servers, same state), including errors and
    `null` results; `thunder_app_cli` against our node and `thunder-cli` against theirs.
17. Orchestrator/BitWindow drive our binary with their usual flags (start, sync, wallet
    pages, deposits, withdrawals, mining); dc-hub and the seed index against our RPC.
18. Shadow validation on betanet for weeks: our node next to theirs, compare tip, roots,
    `list_utxos` hash and pending bundle every block.
