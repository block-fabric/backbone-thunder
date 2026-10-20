// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Thunder (eCash betanet slot 9) consensus types, byte-exact with thunder-rust v0.18.2
// (`types/`, MIT). Every hash is BLAKE3(borsh(x)); the P2P wire and their databases use
// bincode 1.3 over serde. Citations are thunder-rust paths at tag v0.18.2 (244e20f).
#pragma once

#include <bt/bitcoin.h>
#include <bt/bytes.h>
#include <bt/codec.h>
#include <bt/crypto.h>
#include <bt/frost.h>
#include <bt/utreexo.h>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace thunder {

using bt::Bytes;
using bt::append;
using bt::ByteSpan;
using bt::Hash32;
using bt::Reader;
using bt::Writer;

constexpr uint8_t THIS_SIDECHAIN = 9; // types/lib.rs:44

/** types/address.rs: 20 raw bytes, text = plain base58, no version, no checksum. */
struct Address {
    bt::Hash20 b;
    static const Address ALL_ZEROS;
    std::string to_base58() const;
    static std::optional<Address> from_base58(std::string_view s);
    /** `s9_<b58>_<hex(sha256(prefix))[..6]>` (address.rs:226-231), wallet display only. */
    std::string format_for_deposit() const;
    friend auto operator<=>(const Address&, const Address&) = default;
    friend bool operator==(const Address&, const Address&) = default;
};

/** authorization.rs:20-28: first 20 bytes of BLAKE3-XOF(compressed VK). */
Address address_from_vk(const bt::frost::Point& vk);

/** output/content.rs:120-134. */
struct Content {
    enum Kind : uint8_t { Value = 0, Withdrawal = 1 };
    Kind kind = Value;
    uint64_t value = 0;
    uint64_t main_fee = 0;        // Withdrawal
    bt::btc::Address main_address; // Withdrawal
    /** content.rs:145-157: Withdrawal = value + main_fee, saturating at u64::MAX. */
    uint64_t get_value() const;
    bool operator==(const Content& o) const
    {
        return kind == o.kind && value == o.value && (kind == Value || (main_fee == o.main_fee && main_address == o.main_address));
    }
};

struct Output {
    Address address;
    Content content;
    uint64_t get_value() const { return content.get_value(); }
    bool operator==(const Output&) const = default;
};

/** transaction/outpoint.rs:270-290. Ord = variant index, then (txid bytes, vout). */
struct OutPoint {
    enum Kind : uint8_t { Regular = 0, Coinbase = 1, Deposit = 2 };
    Kind kind = Regular;
    Hash32 txid; // Txid / CoinbaseTxid / bitcoin Txid (internal byte order)
    uint32_t vout = 0;
    friend auto operator<=>(const OutPoint&, const OutPoint&) = default;
    friend bool operator==(const OutPoint&, const OutPoint&) = default;
    /** 37-byte borsh form, also the key of their utxos/stxos tables. */
    std::array<uint8_t, 37> key() const;
    std::string to_string() const;
};

/** Reference to a spend (transaction/mod.rs:44-55). */
struct InPoint {
    enum Kind : uint8_t { Regular = 0, Withdrawal = 1 };
    Kind kind = Regular;
    Hash32 id; // txid, or m6id (internal byte order)
    uint32_t vin = 0;
    bool operator==(const InPoint&) const = default;
};

struct SpentOutput {
    Output output;
    InPoint inpoint;
};

struct Transaction {
    std::vector<std::pair<OutPoint, Hash32>> inputs; // (outpoint, utxo_hash)
    bt::utreexo::Proof proof;                        // #[borsh(skip)], serde only
    std::vector<Output> outputs;

    Bytes canonical_bytes() const; // borsh
    uint64_t canonical_size() const { return canonical_bytes().size(); }
    Hash32 txid() const;
};

struct Authorization {
    bt::frost::Point verifying_key;
    bt::frost::Signature signature;
};

struct AuthorizedTransaction {
    Transaction transaction;
    std::vector<Authorization> authorizations;
};

struct Coinbase {
    Bytes memo;
    std::vector<Output> outputs;
};

struct Body {
    Coinbase coinbase;
    std::vector<Transaction> transactions;
    std::vector<Authorization> authorizations;
    size_t inputs_len() const;
    std::vector<AuthorizedTransaction> authorized_transactions() const;
    static Body from_authorized(std::vector<AuthorizedTransaction> txs, Coinbase cb);
};

/** block/mod.rs:28-39 */
struct Header {
    Hash32 merkle_root;
    std::optional<Hash32> prev_side_hash;
    Hash32 prev_main_hash; // bitcoin block hash, internal byte order
    std::vector<bt::utreexo::NodeHash> roots;

    Hash32 hash() const;
    Hash32 coinbase_txid() const;
    bool operator==(const Header& o) const
    {
        return merkle_root == o.merkle_root && prev_side_hash == o.prev_side_hash && prev_main_hash == o.prev_main_hash &&
               roots == o.roots;
    }
};

struct Block {
    Header header;
    Body body;
};

/** A transaction together with the outputs it spends (FilledTransaction). */
struct FilledTransaction {
    const Transaction* tx = nullptr;
    std::vector<Output> spent_utxos;
    /** get_fee(): value_in - value_out with checked sums; error -> nullopt. */
    std::optional<uint64_t> fee() const;
};

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const Address& a);
void borsh(Writer& w, const Content& c);
void borsh(Writer& w, const Output& o);
void borsh(Writer& w, const OutPoint& o);
void borsh(Writer& w, const Transaction& t);
void borsh(Writer& w, const Authorization& a);
void borsh(Writer& w, const Coinbase& c);
void borsh(Writer& w, const Body& b);
void borsh(Writer& w, const Header& h);
template <class T>
Bytes borsh_bytes(const T& x)
{
    Writer w;
    borsh(w, x);
    return w.buf;
}
template <class T>
Hash32 hash(const T& x)
{
    return bt::blake3(borsh_bytes(x));
}

/** utxo_hash / Utreexo leaf = blake3(borsh{outpoint, output}) (output/mod.rs:55-78). */
Hash32 pointed_output_hash(const OutPoint& op, const Output& out);

// ---------------------------------------------------------------- bincode (serde)
void bincode(Writer& w, const Address& a);
void bincode(Writer& w, const Content& c);
void bincode(Writer& w, const Output& o);
void bincode(Writer& w, const OutPoint& o);
void bincode(Writer& w, const bt::utreexo::NodeHash& h);
void bincode(Writer& w, const bt::utreexo::Proof& p);
void bincode(Writer& w, const Transaction& t);
void bincode(Writer& w, const Authorization& a);
void bincode(Writer& w, const AuthorizedTransaction& t);
void bincode(Writer& w, const Coinbase& c);
void bincode(Writer& w, const Body& b);
void bincode(Writer& w, const Header& h);
void bincode(Writer& w, const Block& b);

Address de_address(Reader& r);
Content de_content(Reader& r);
Output de_output(Reader& r);
OutPoint de_outpoint(Reader& r);
bt::utreexo::NodeHash de_nodehash(Reader& r);
bt::utreexo::Proof de_proof(Reader& r);
Transaction de_transaction(Reader& r);
Authorization de_authorization(Reader& r);
AuthorizedTransaction de_authorized_transaction(Reader& r);
Coinbase de_coinbase(Reader& r);
Body de_body(Reader& r);
Header de_header(Reader& r);

/** borsh decoders (used for the index's /tx/<txid>/hex = borsh(tx)). */
Output borsh_de_output(Reader& r);
OutPoint borsh_de_outpoint(Reader& r);
Transaction borsh_de_transaction(Reader& r);

// ---------------------------------------------------------------- merkle
/** block/body.rs:154-224. nullopt where theirs returns an error (block invalid). */
std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs);
/** coinbase.rs:85-91 */
std::optional<Hash32> coinbase_merkle_root(const Coinbase& cb);
std::optional<Hash32> outputs_merkle_root(const std::vector<Output>& outs);
Hash32 inputs_merkle_root(const std::vector<std::pair<OutPoint, Hash32>>& ins);

/** coinbase.rs:99-121 */
Hash32 compute_coinbase_txid(const Hash32& merkle_root, const Hash32& prev_main_hash, const std::optional<Hash32>& prev_side);

// ---------------------------------------------------------------- limits
/** lib/state/mod.rs:445-476 (f64 powi + floor, cap START*111 from month 120). */
uint64_t body_sigops_limit(uint32_t height);
uint64_t body_size_limit(uint32_t height);
/** compiler-rt __powidf2: what Rust's f64::powi lowers to for a runtime exponent. */
double powi(double a, int b);

// ---------------------------------------------------------------- authorizations
/** verify_authorizations (authorization.rs:186-230): exact count, then every signature
 *  over borsh(tx) of its transaction. */
bool verify_authorizations(const Body& body);
bool verify_authorized_transaction(const AuthorizedTransaction& tx);

} // namespace thunder
