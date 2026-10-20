// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// The small slice of Bitcoin the sidechains need: addresses as rust-bitcoin 0.32 parses,
// orders and prints them (Address<NetworkUnchecked>), scriptPubKeys, and the legacy
// transaction serialization used for withdrawal bundles (M6id = txid of an input-less tx).
#pragma once

#include <bt/bytes.h>

#include <optional>
#include <string>

namespace bt::btc {

/** rust-bitcoin AddressInner, with the same derived ordering:
 *  P2pkh{hash, network} < P2sh{hash, network} < Segwit{program{version, bytes}, hrp}. */
struct Address {
    enum class Kind : uint8_t { P2pkh = 0, P2sh = 1, Segwit = 2 };
    enum class NetKind : uint8_t { Main = 0, Test = 1 };           // NetworkKind
    enum class Hrp : uint8_t { Mainnet = 0, Testnets = 1, Regtest = 2 }; // KnownHrp
    Kind kind = Kind::P2pkh;
    NetKind net = NetKind::Main;   // P2pkh / P2sh
    Hrp hrp = Hrp::Mainnet;        // Segwit
    uint8_t witver = 0;            // Segwit
    Bytes program;                 // 20-byte hash, or the witness program

    /** Parse as `Address<NetworkUnchecked>::from_str` (bech32 first, then base58). */
    static std::optional<Address> parse(std::string_view s);
    /** Display (lowercase bech32 / base58check). */
    std::string to_string() const;
    /** assume_checked().script_pubkey() */
    Bytes script_pubkey() const;
    /** Derived Ord of rust-bitcoin's Address. */
    friend std::strong_ordering operator<=>(const Address& a, const Address& b);
    friend bool operator==(const Address& a, const Address& b) { return (a <=> b) == 0; }
};

struct TxOut {
    uint64_t value = 0;
    Bytes script_pubkey;
};

void write_compact_size(Bytes& out, uint64_t n);
size_t compact_size_len(uint64_t n);

/** Legacy (non-witness) serialization of a version-2 tx with no inputs, locktime 0. */
Bytes serialize_inputless_tx(int32_t version, const std::vector<TxOut>& outs, uint32_t locktime);
/** BIP141 weight as rust-bitcoin computes it: base*3 + total, where an input-less tx is
 *  counted with the 2-byte segwit marker (uses_segwit_serialization() is true for no inputs). */
uint64_t inputless_tx_weight(const std::vector<TxOut>& outs);

/** Script builder helpers. */
Bytes op_return_push(ByteSpan data);

/** Bitcoin consensus-encoded transaction, parsed minimally (for Succeeded events). */
struct ParsedTx {
    int32_t version = 0;
    size_t n_inputs = 0;
    std::vector<TxOut> outputs;
    uint32_t locktime = 0;
};
std::optional<ParsedTx> parse_tx(ByteSpan raw);

} // namespace bt::btc
