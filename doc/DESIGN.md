# backbone-thunder: design

A C++20 engine for the LayerTwo-family sidechains that run on eCash (Thunder, BitNames,
BitAssets, zSide, Truthcoin), built to be a **drop-in replacement client**: same consensus
(bugs included), same P2P protocol, same mainchain link, same CLI/RPC surface. This phase
(S1) delivers the shared core and Thunder (slot 9). Plan and requirements:
`~/skydoge/audit/2026-10-20-ecash-sidechains-compat.md` (§4b drop-in surface, §4c keep/revert,
§5 bugs, §6–7 plan; Part II Thunder).

## Layout

```
libs/core/          libbt_core  - consensus-neutral building blocks shared by all five chains
                    libbt_net   - QUIC, gRPC, enforcer client (the only part that links TLS)
chains/thunder/     libthunder_core (rules: types, merkle, state machine, archive, P2P msgs, JSON)
                    libthunder_node (node, wallet, app/RPC), binaries thunder, thunder-cli,
                    thunder-fetch-blocks
tests/              unit + known-answer tests; tests/data/betanet = public betanet data
depends/            pinned, hash-checked third-party builds (OpenSSL 3.5.9, ngtcp2 1.25.0)
third_party/        vendored BLAKE3 C (CC0/Apache-2.0) and UniValue (MIT)
tools/              bt-enforcer-probe
```

`libs/core` knows nothing about a particular chain. A chain module (`chains/<name>`) defines
its types, encodings, merkle variant, Utreexo on/off, state machine and RPC; the other four
chains are new directories next to `chains/thunder` reusing the core (study §1: the five are
"backport-thunder-x.y" descendants that differ in tx fields, merkle variant, Utreexo and magic).

| Core module | What it is | Exactness reference |
|---|---|---|
| `codec.h` | borsh 1.x and bincode 1.3 (legacy config: LE fixint, u64 lengths, u32 tags) writers/readers, Rust-exact UTF-8 validation | borsh/bincode/serde |
| `crypto.h` | BLAKE3 (vendored C), SHA-256/512/512-256, base58/base58check | NIST/BLAKE3 vectors |
| `frost.h` | frost-ristretto255 3.0 single-signer Schnorr verify/sign on libsodium ristretto255 (canonical, non-identity points; canonical scalars; RFC 9591 challenge) | RFC 9591 vector |
| `utreexo.h` | literal port of rustreexo rev 6da503b MemForest + Proof::verify (value-based node lookup, rebuilt leaf map, Empty roots, Rust release-mode integer semantics) | rustreexo test_cases.json |
| `bitcoin.h` | rust-bitcoin 0.32 `Address<NetworkUnchecked>` parse/print/order/spk, input-less tx serialization and weight | BIP-173/350 vectors |
| `mainchain.h`, `enforcer.h`, `grpc.h`, `protobuf.h` | cusf.mainchain.v1 types; gRPC client on nghttp2 + OpenSSL (h2c or TLS/ALPN h2); hand-coded protobuf | live public enforcer |
| `quic.h` | QUIC endpoint on ngtcp2 + OpenSSL 3.5 QUIC TLS API, quinn/rustls-compatible | live public betanet peers |
| `kv.h` | ordered KV with transactions; SQLite (WAL) or memory; memcmp key order like LMDB | |
| `hd.h` | BIP-39 + "bip32ish" ristretto255 derivation (both thunder 0.18.0 and ≥0.18.1 variants) | BIP-39 vectors |
| `jsonrpc.h`, `log.h`, `parse.h` | jsonrpsee-shaped JSON-RPC over HTTP, daily-rolling logs, non-throwing parsers | |

## Decision: lean standalone node, not our Bitcoin Core base

Nothing of Bitcoin Core's consensus applies to these chains (no sha256d, no scripts, no
Bitcoin P2P, no UTXO-by-coins-cache semantics, different headers), and their node behaviour
is tightly specified by thunder-rust (LMDB table semantics, 2WPD windows, fork choice). Pulling
Core in would bring ~200k lines whose behaviour we would have to keep *out* of consensus, plus
its init/threading/logging model. So:

* **Standalone engine**, small dependency set (libsodium, SQLite, OpenSSL 3.5, ngtcp2,
  nghttp2), all static and mingw-buildable; consensus code is free of TLS.
* **Reused from our code:** UniValue (MIT, identical to Core's) for JSON; the build
  conventions (`depends/`, static, `tools/build.sh`), and the coding style.
* **Unified app integration (later):** each compat chain is one more node kind in the app's
  supervisor (`webui/sidechainnodes.cpp` in our repos). The app talks to it over its own
  JSON-RPC (the same API BitWindow uses), so no linking into the Qt/web UI process is needed.
  The wallet kit for these chains (FROST keys, bip32ish import) lives in `libs/core/hd.h` and
  can be linked by the app if it wants local signing. Backbone (backboned) provides the
  mainchain through the same `cusf.mainchain.v1` gRPC the real enforcer serves.

## Consensus fidelity rules

* Every consensus function cites the thunder-rust v0.18.2 file:line it ports, keeps their
  branch order, and reproduces their arithmetic (wrapping where Rust release builds wrap,
  `f64::powi` as compiler-rt `__powidf2`, compiled with `-ffp-contract=off`).
* Where their node **panics** on a block, ours rejects the block (`StateError::panic`): the
  network cannot connect it either.
* LMDB-observable behaviour is reproduced through key encodings: e.g. `deposit_blocks` keys
  are little-endian u32 under memcmp order, so `last()` misbehaves after 256 entries exactly
  as theirs (test `quirk_deposit_blocks_le_key_order`).
* Consensus-neutral improvements are kept (study §4c KEEP): no panics in revert paths,
  crash-safe SQLite store, templates whose roots are computed the way validation computes
  them, hardened RPC (see thunder.md), never auto-retrying non-idempotent mainchain calls.

## Libraries and licences

| Library | Version | Licence | Use |
|---|---|---|---|
| BLAKE3 C | 1.8.5 | CC0-1.0 / Apache-2.0 | hashing (vendored, portable backend) |
| UniValue | from our Bitcoin Core v32 base | MIT | JSON |
| libsodium | 1.0.18 (system; 1.0.20 for mingw) | ISC | ristretto255 group/scalar ops, RNG |
| SQLite | 3.45 (system) | public domain | store |
| OpenSSL | 3.5.9 LTS (depends, SHA-256 checked) | Apache-2.0 | TLS 1.3 for QUIC and gRPC |
| ngtcp2 | 1.25.0 (depends, SHA-256 checked) | MIT | QUIC transport (`ngtcp2_crypto_ossl`) |
| nghttp2 | 1.59 (system) | MIT | HTTP/2 for gRPC |
| rustreexo test vectors | rev 6da503b | MIT | tests only |
| BIP-39 English wordlist | bips repo | public (BIP-39) | mnemonics |

No LayerTwo/octobocto code is vendored, built or executed. Their sources were read to
specify behaviour; thunder-rust ≥ v0.18.2 is MIT.

### Why ngtcp2 + OpenSSL 3.5 for QUIC

* Their side is quinn 0.11 + rustls 0.23 (QUIC v1, TLS 1.3, no ALPN, self-signed certs,
  client skips verification, one bidi stream per message). rustls fatally rejects an
  ALPN that is offered but not configured, so the stack must be able to omit ALPN entirely;
  ngtcp2 leaves ALPN to the application and OpenSSL's TLS layer does not enforce it.
* ngtcp2 is a portable C library with no I/O model of its own (we run it on our own
  poll loop, identical on Linux and mingw/WSAPoll), widely interop-tested (curl uses it).
* OpenSSL 3.5 LTS added the third-party QUIC TLS API ngtcp2's `ossl` backend uses, so no
  OpenSSL fork (quictls/BoringSSL) is needed, and OpenSSL cross-builds for mingw64
  officially (`Configure mingw64`). The same OpenSSL serves gRPC-over-TLS.
* Rejected: msquic (schannel/MSVC on Windows, heavy), picoquic (less mingw tooling),
  GnuTLS backend (LGPL, nettle/gmp chain for mingw), OpenSSL's own QUIC stack (less
  control over transport parameters/stream credit than ngtcp2).
