// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <thunder/types.h>

#include <bt/crypto.h>

#include <cmath>
#include <deque>

namespace thunder {

using bt::DecodeError;
using bt::utreexo::NodeHash;

const Address Address::ALL_ZEROS{};

std::string Address::to_base58() const { return bt::base58_encode(b.span()); }

std::optional<Address> Address::from_base58(std::string_view s)
{
    auto d = bt::base58_decode(s);
    if (!d || d->size() != 20) return std::nullopt;
    Address a;
    std::memcpy(a.b.data(), d->data(), 20);
    return a;
}

std::string Address::format_for_deposit() const
{
    std::string prefix = "s" + std::to_string(THIS_SIDECHAIN) + "_" + to_base58() + "_";
    auto d = bt::sha256(ByteSpan((const uint8_t*)prefix.data(), prefix.size()));
    return prefix + bt::hex_encode(ByteSpan(d.data(), 3));
}

Address address_from_vk(const bt::frost::Point& vk)
{
    bt::Blake3 h;
    h.update(vk.span());
    Address a;
    h.finalize_xof(a.b.data(), 20);
    return a;
}

uint64_t Content::get_value() const
{
    if (kind == Value) return value;
    uint64_t s = value + main_fee;
    return s < value ? UINT64_MAX : s;
}

std::array<uint8_t, 41> OutPoint::key() const
{
    std::array<uint8_t, 41> k{};
    k[0] = kind;
    std::memcpy(k.data() + 1, txid.data(), 32);
    // borsh field order: (txid, vout), or (m6id, height, vout)
    const uint32_t a = kind == UnsuccessfulWithdrawal ? height : vout;
    for (int i = 0; i < 4; i++) k[33 + i] = uint8_t(a >> (8 * i));
    if (kind == UnsuccessfulWithdrawal)
        for (int i = 0; i < 4; i++) k[37 + i] = uint8_t(vout >> (8 * i));
    return k;
}

std::string OutPoint::to_string() const
{
    switch (kind) {
    case Regular: return "regular " + bt::hex_encode(txid) + " " + std::to_string(vout);
    case Coinbase: return "coinbase " + bt::hex_encode(txid) + " " + std::to_string(vout);
    case Deposit: return "deposit " + bt::hex_encode_rev(txid.span()) + " " + std::to_string(vout);
    case UnsuccessfulWithdrawal:
        return "unsuccessful withdrawal " + bt::hex_encode_rev(txid.span()) + " " + std::to_string(height) + " " + std::to_string(vout);
    }
    return {};
}

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const Address& a) { w.raw(a.b); }

void borsh(Writer& w, const Content& c)
{
    w.u8(c.kind);
    w.u64(c.value);
    if (c.kind == Content::Withdrawal) {
        w.u64(c.main_fee);
        w.borsh_bytes(c.main_address.script_pubkey()); // util.rs: bitcoin_address -> spk bytes
    }
}

void borsh(Writer& w, const Output& o)
{
    borsh(w, o.address);
    borsh(w, o.content);
}

void borsh(Writer& w, const OutPoint& o)
{
    w.u8(o.kind);
    w.raw(o.txid);
    if (o.kind == OutPoint::UnsuccessfulWithdrawal) w.u32(o.height); // { m6id, height, vout }
    w.u32(o.vout);
}

void borsh(Writer& w, const Transaction& t)
{
    w.borsh_len(t.inputs.size());
    for (auto& [op, h] : t.inputs) {
        borsh(w, op);
        w.raw(h);
    }
    w.borsh_len(t.outputs.size());
    for (auto& o : t.outputs) borsh(w, o);
}

void borsh(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key);
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void borsh(Writer& w, const Coinbase& c)
{
    w.borsh_bytes(c.memo);
    w.borsh_len(c.outputs.size());
    for (auto& o : c.outputs) borsh(w, o);
}

void borsh(Writer& w, const Body& b)
{
    borsh(w, b.coinbase);
    w.borsh_len(b.transactions.size());
    for (auto& t : b.transactions) borsh(w, t);
    w.borsh_len(b.authorizations.size());
    for (auto& a : b.authorizations) borsh(w, a);
}

void borsh(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.raw(h.prev_main_hash);
    w.borsh_len(h.roots.size());
    for (auto& r : h.roots) w.raw(r.bytes()); // Deref: Empty/Placeholder -> zeros
}

Hash32 Transaction::txid() const { return bt::blake3(canonical_bytes()); }
Bytes Transaction::canonical_bytes() const { return borsh_bytes(*this); }

Hash32 Header::hash() const { return thunder::hash(*this); }
Hash32 Header::coinbase_txid() const { return compute_coinbase_txid(merkle_root, prev_main_hash, prev_side_hash); }

Hash32 compute_coinbase_txid(const Hash32& merkle_root, const Hash32& prev_main_hash, const std::optional<Hash32>& prev_side)
{
    Writer w;
    w.raw(merkle_root);
    w.raw(prev_main_hash);
    if (prev_side) {
        w.u8(1);
        w.raw(*prev_side);
    } else {
        w.u8(0);
    }
    return bt::blake3(w.buf);
}

Hash32 pointed_output_hash(const OutPoint& op, const Output& out)
{
    Writer w;
    borsh(w, op);
    borsh(w, out);
    return bt::blake3(w.buf);
}

size_t Body::inputs_len() const
{
    size_t n = 0;
    for (auto& t : transactions) n += t.inputs.size();
    return n;
}

std::vector<AuthorizedTransaction> Body::authorized_transactions() const
{
    std::vector<AuthorizedTransaction> r;
    size_t k = 0;
    for (auto& t : transactions) {
        AuthorizedTransaction at;
        at.transaction = t;
        for (size_t i = 0; i < t.inputs.size() && k < authorizations.size(); i++) at.authorizations.push_back(authorizations[k++]);
        r.push_back(std::move(at));
    }
    return r;
}

Body Body::from_authorized(std::vector<AuthorizedTransaction> txs, Coinbase cb)
{
    Body b;
    b.coinbase = std::move(cb);
    for (auto& t : txs) {
        for (auto& a : t.authorizations) b.authorizations.push_back(a);
        b.transactions.push_back(std::move(t.transaction));
    }
    return b;
}

std::optional<uint64_t> FilledTransaction::fee() const
{
    uint64_t in = 0, out = 0;
    for (auto& o : spent_utxos) {
        uint64_t v = o.get_value();
        if (in + v < in) return std::nullopt;
        in += v;
    }
    for (auto& o : tx->outputs) {
        uint64_t v = o.get_value();
        if (out + v < out) return std::nullopt;
        out += v;
    }
    if (in < out) return std::nullopt; // Underfunded
    return in - out;
}

// ---------------------------------------------------------------- bincode
void bincode(Writer& w, const Address& a) { w.raw(a.b); }

void bincode(Writer& w, const Content& c)
{
    w.u32(c.kind); // DefaultRepr enum
    w.u64(c.value);
    if (c.kind == Content::Withdrawal) {
        w.u64(c.main_fee);
        w.bincode_str(c.main_address.to_string()); // Address serde = collect_str(Display)
    }
}

void bincode(Writer& w, const Output& o)
{
    bincode(w, o.address);
    bincode(w, o.content);
}

void bincode(Writer& w, const OutPoint& o)
{
    w.u32(o.kind);
    if (o.kind == OutPoint::Deposit) {
        w.bincode_bytes(o.txid.span()); // bitcoin::Txid serde = serialize_bytes
        w.u32(o.vout);
    } else if (o.kind == OutPoint::UnsuccessfulWithdrawal) {
        w.bincode_bytes(o.txid.span()); // M6id: serde-transparent bitcoin::Txid
        w.u32(o.height);
        w.u32(o.vout);
    } else {
        w.raw(o.txid); // [u8; 32] as a tuple
        w.u32(o.vout);
    }
}

void bincode(Writer& w, const NodeHash& h)
{
    w.u32(h.tag);
    if (h.tag == NodeHash::Some) w.raw(h.h);
}

void bincode(Writer& w, const bt::utreexo::Proof& p)
{
    w.bincode_len(p.targets.size());
    for (auto t : p.targets) w.u64(t);
    w.bincode_len(p.hashes.size());
    for (auto& h : p.hashes) bincode(w, h);
}

void bincode(Writer& w, const Transaction& t)
{
    w.bincode_len(t.inputs.size());
    for (auto& [op, h] : t.inputs) {
        bincode(w, op);
        w.raw(h);
    }
    bincode(w, t.proof);
    w.bincode_len(t.outputs.size());
    for (auto& o : t.outputs) bincode(w, o);
}

void bincode(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key);    // serdect::array -> tuple, 32 bytes
    w.bincode_len(64);         // serdect::slice -> bytes with u64 length
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void bincode(Writer& w, const AuthorizedTransaction& t)
{
    bincode(w, t.transaction);
    w.bincode_len(t.authorizations.size());
    for (auto& a : t.authorizations) bincode(w, a);
}

void bincode(Writer& w, const Coinbase& c)
{
    w.bincode_bytes(c.memo);
    w.bincode_len(c.outputs.size());
    for (auto& o : c.outputs) bincode(w, o);
}

void bincode(Writer& w, const Body& b)
{
    bincode(w, b.coinbase);
    w.bincode_len(b.transactions.size());
    for (auto& t : b.transactions) bincode(w, t);
    w.bincode_len(b.authorizations.size());
    for (auto& a : b.authorizations) bincode(w, a);
}

void bincode(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.bincode_bytes(h.prev_main_hash.span()); // bitcoin::BlockHash serde = bytes
    w.bincode_len(h.roots.size());
    for (auto& r : h.roots) bincode(w, r);
}

void bincode(Writer& w, const Block& b)
{
    bincode(w, b.header);
    bincode(w, b.body);
}

Address de_address(Reader& r)
{
    Address a;
    a.b = r.array<20>();
    return a;
}

Content de_content(Reader& r)
{
    Content c;
    uint32_t tag = r.u32();
    if (tag == 0) {
        c.kind = Content::Value;
        c.value = r.u64();
    } else if (tag == 1) {
        c.kind = Content::Withdrawal;
        c.value = r.u64();
        c.main_fee = r.u64();
        std::string s = r.bincode_str();
        auto a = bt::btc::Address::parse(s);
        if (!a) throw DecodeError("invalid main_address");
        c.main_address = *a;
    } else {
        throw DecodeError("invalid Content tag");
    }
    return c;
}

Output de_output(Reader& r)
{
    Output o;
    o.address = de_address(r);
    o.content = de_content(r);
    return o;
}

OutPoint de_outpoint(Reader& r)
{
    OutPoint o;
    uint32_t tag = r.u32();
    if (tag > 3) throw DecodeError("invalid OutPoint tag");
    o.kind = OutPoint::Kind(tag);
    if (o.kind == OutPoint::Deposit || o.kind == OutPoint::UnsuccessfulWithdrawal) {
        Bytes b = r.bincode_bytes();
        if (b.size() != 32) throw DecodeError("invalid txid length");
        o.txid = Hash32::from_span(b);
        if (o.kind == OutPoint::UnsuccessfulWithdrawal) o.height = r.u32();
    } else {
        o.txid = r.array<32>();
    }
    o.vout = r.u32();
    return o;
}

NodeHash de_nodehash(Reader& r)
{
    uint32_t tag = r.u32();
    if (tag == 0) return NodeHash::empty();
    if (tag == 1) return NodeHash{NodeHash::Placeholder, {}};
    if (tag == 2) return NodeHash::some(r.array<32>());
    throw DecodeError("invalid NodeHash tag");
}

bt::utreexo::Proof de_proof(Reader& r)
{
    bt::utreexo::Proof p;
    size_t n = r.bincode_len(8);
    p.targets.reserve(n);
    for (size_t i = 0; i < n; i++) p.targets.push_back(r.u64());
    size_t m = r.bincode_len(4);
    for (size_t i = 0; i < m; i++) p.hashes.push_back(de_nodehash(r));
    return p;
}

Transaction de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.bincode_len(4 + 4 + 32);
    for (size_t i = 0; i < n; i++) {
        OutPoint op = de_outpoint(r);
        Hash32 h = r.array<32>();
        t.inputs.emplace_back(op, h);
    }
    t.proof = de_proof(r);
    size_t m = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(de_output(r));
    return t;
}

Authorization de_authorization(Reader& r)
{
    Authorization a;
    a.verifying_key = r.array<32>();
    if (!bt::frost::valid_element(a.verifying_key)) throw DecodeError("invalid verifying key");
    Bytes sig = r.bincode_bytes();
    if (sig.size() != 64) throw DecodeError("MalformedSignature");
    std::memcpy(a.signature.R.data(), sig.data(), 32);
    std::memcpy(a.signature.z.data(), sig.data() + 32, 32);
    if (!bt::frost::valid_element(a.signature.R) || !bt::frost::canonical_scalar(a.signature.z))
        throw DecodeError("invalid signature encoding");
    return a;
}

AuthorizedTransaction de_authorized_transaction(Reader& r)
{
    AuthorizedTransaction t;
    t.transaction = de_transaction(r);
    size_t n = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < n; i++) t.authorizations.push_back(de_authorization(r));
    return t;
}

Coinbase de_coinbase(Reader& r)
{
    Coinbase c;
    c.memo = r.bincode_bytes();
    size_t n = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < n; i++) c.outputs.push_back(de_output(r));
    return c;
}

Body de_body(Reader& r)
{
    Body b;
    b.coinbase = de_coinbase(r);
    size_t n = r.bincode_len(8 + 16 + 8);
    for (size_t i = 0; i < n; i++) b.transactions.push_back(de_transaction(r));
    size_t m = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < m; i++) b.authorizations.push_back(de_authorization(r));
    return b;
}

Header de_header(Reader& r)
{
    Header h;
    h.merkle_root = r.array<32>();
    uint8_t opt = r.u8();
    if (opt == 1) h.prev_side_hash = r.array<32>();
    else if (opt != 0) throw DecodeError("invalid Option tag");
    Bytes pm = r.bincode_bytes();
    if (pm.size() != 32) throw DecodeError("invalid block hash length");
    h.prev_main_hash = Hash32::from_span(pm);
    size_t n = r.bincode_len(4);
    for (size_t i = 0; i < n; i++) h.roots.push_back(de_nodehash(r));
    return h;
}

// ---------------------------------------------------------------- borsh decode
OutPoint borsh_de_outpoint(Reader& r)
{
    OutPoint o;
    uint8_t tag = r.u8();
    if (tag > 3) throw DecodeError("invalid OutPoint tag");
    o.kind = OutPoint::Kind(tag);
    o.txid = r.array<32>();
    if (o.kind == OutPoint::UnsuccessfulWithdrawal) o.height = r.u32();
    o.vout = r.u32();
    return o;
}

Output borsh_de_output(Reader& r)
{
    Output o;
    o.address.b = r.array<20>();
    uint8_t tag = r.u8();
    if (tag == 0) {
        o.content.kind = Content::Value;
        o.content.value = r.u64();
    } else if (tag == 1) {
        o.content.kind = Content::Withdrawal;
        o.content.value = r.u64();
        o.content.main_fee = r.u64();
        Bytes spk = r.borsh_bytes();
        // borsh carries the scriptPubKey; recover an address of the same script (network
        // information is not in borsh, mainnet forms are used).
        bt::btc::Address a;
        if (spk.size() == 25 && spk[0] == 0x76 && spk[1] == 0xa9 && spk[2] == 0x14 && spk[23] == 0x88 && spk[24] == 0xac) {
            a.kind = bt::btc::Address::Kind::P2pkh;
            a.program.assign(spk.begin() + 3, spk.begin() + 23);
        } else if (spk.size() == 23 && spk[0] == 0xa9 && spk[1] == 0x14 && spk[22] == 0x87) {
            a.kind = bt::btc::Address::Kind::P2sh;
            a.program.assign(spk.begin() + 2, spk.begin() + 22);
        } else if (spk.size() >= 4 && (spk[0] == 0 || (spk[0] >= 0x51 && spk[0] <= 0x60)) && spk[1] + 2u == spk.size()) {
            a.kind = bt::btc::Address::Kind::Segwit;
            a.witver = spk[0] == 0 ? 0 : uint8_t(spk[0] - 0x50);
            a.program.assign(spk.begin() + 2, spk.end());
        } else {
            throw DecodeError("withdrawal script is not an address");
        }
        o.content.main_address = a;
    } else {
        throw DecodeError("invalid Content tag");
    }
    return o;
}

Transaction borsh_de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.borsh_len(37 + 32);
    for (size_t i = 0; i < n; i++) {
        OutPoint op = borsh_de_outpoint(r);
        Hash32 h = r.array<32>();
        t.inputs.emplace_back(op, h);
    }
    size_t m = r.borsh_len(29);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(borsh_de_output(r));
    return t;
}

// ---------------------------------------------------------------- merkle (CKB CBMT, merkle-cbt 0.3.2)
namespace {
// merkle_cbt::CBMT::build_merkle_root, generic over a merge that may fail.
template <class T, class Merge>
std::optional<T> cbmt_root(const std::vector<T>& leaves, Merge merge, const T& dflt)
{
    if (leaves.empty()) return dflt;
    std::deque<T> q;
    size_t n = leaves.size();
    // rchunks_exact(2): pairs from the end
    size_t i = n;
    while (i >= 2) {
        auto m = merge(leaves[i - 2], leaves[i - 1]);
        if (!m) return std::nullopt;
        q.push_back(*m);
        i -= 2;
    }
    if (i == 1) q.push_front(leaves[0]);
    while (q.size() > 1) {
        T right = q.front();
        q.pop_front();
        T left = q.front();
        q.pop_front();
        auto m = merge(left, right);
        if (!m) return std::nullopt;
        q.push_back(*m);
    }
    return q.front();
}

struct ValueNode {
    Hash32 commitment;
    uint64_t value = 0;
    uint64_t size = 0;
};
} // namespace

Hash32 inputs_merkle_root(const std::vector<std::pair<OutPoint, Hash32>>& ins)
{
    std::vector<Hash32> leaves;
    for (auto& [op, h] : ins) {
        Writer w;
        borsh(w, op);
        w.raw(h);
        leaves.push_back(bt::blake3(w.buf));
    }
    auto r = cbmt_root<Hash32>(
        leaves,
        [](const Hash32& l, const Hash32& r) -> std::optional<Hash32> {
            Writer w;
            w.raw(l);
            w.raw(r);
            return bt::blake3(w.buf);
        },
        Hash32{});
    return *r;
}

std::optional<Hash32> outputs_merkle_root(const std::vector<Output>& outs)
{
    std::vector<ValueNode> leaves;
    for (auto& o : outs) {
        ValueNode n;
        n.value = o.get_value();
        n.size = borsh_bytes(o).size();
        Writer w;
        w.u64(n.value);
        w.u64(n.size);
        borsh(w, o);
        n.commitment = bt::blake3(w.buf);
        leaves.push_back(n);
    }
    auto r = cbmt_root<ValueNode>(
        leaves,
        [](const ValueNode& l, const ValueNode& r) -> std::optional<ValueNode> {
            ValueNode n;
            n.value = l.value + r.value;
            if (n.value < l.value) return std::nullopt; // AmountOverflow
            n.size = l.size + r.size;
            if (n.size < l.size) return std::nullopt; // SizeOverflow
            Writer w;
            w.raw(l.commitment);
            w.u64(n.value);
            w.u64(n.size);
            w.raw(r.commitment);
            n.commitment = bt::blake3(w.buf);
            return n;
        },
        ValueNode{});
    if (!r) return std::nullopt;
    return r->commitment;
}

std::optional<Hash32> coinbase_merkle_root(const Coinbase& cb)
{
    auto oc = outputs_merkle_root(cb.outputs);
    if (!oc) return std::nullopt;
    Writer w;
    w.borsh_bytes(cb.memo);
    w.raw(*oc);
    return bt::blake3(w.buf);
}

// The CBMT commitment of the filled transactions (body.rs:154-212).
static std::optional<Hash32> txs_commitment(const std::vector<FilledTransaction>& txs)
{
    std::vector<ValueNode> leaves;
    for (auto& ft : txs) {
        auto fee = ft.fee();
        if (!fee) return std::nullopt;
        uint64_t size = ft.tx->canonical_size();
        auto outs = outputs_merkle_root(ft.tx->outputs);
        if (!outs) return std::nullopt;
        Writer tm;
        tm.raw(inputs_merkle_root(ft.tx->inputs));
        tm.raw(*outs);
        Hash32 tx_merkle_root = bt::blake3(tm.buf);
        Writer w;
        w.u64(*fee);
        w.u64(size);
        w.raw(tx_merkle_root);
        leaves.push_back(ValueNode{bt::blake3(w.buf), *fee, size});
    }
    auto r = cbmt_root<ValueNode>(
        leaves,
        [](const ValueNode& l, const ValueNode& r) -> std::optional<ValueNode> {
            ValueNode n;
            n.value = l.value + r.value;
            if (n.value < l.value) return std::nullopt; // Amount `+` panics in their node
            n.size = l.size + r.size;                  // plain u64 `+`: wraps in release
            Writer w;
            w.raw(l.commitment);
            w.u64(n.value);
            w.u64(n.size);
            w.raw(r.commitment);
            n.commitment = bt::blake3(w.buf);
            return n;
        },
        ValueNode{});
    if (!r) return std::nullopt;
    return r->commitment;
}

std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs)
{
    auto txc = txs_commitment(txs);
    if (!txc) return std::nullopt;
    auto cbr = coinbase_merkle_root(cb);
    if (!cbr) return std::nullopt;
    Writer w;
    w.raw(*cbr);
    w.raw(*txc);
    return bt::blake3(w.buf);
}

std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs,
                                          const std::vector<Authorization>& authorizations)
{
    // HashComponents { coinbase_commitment, txs_commitment, authorizations_commitment }, the last being
    // hash_with_scratch_buffer(&[Authorization]) = blake3(borsh: u32 count, then each authorization).
    auto txc = txs_commitment(txs);
    if (!txc) return std::nullopt;
    auto cbr = coinbase_merkle_root(cb);
    if (!cbr) return std::nullopt;
    Writer a;
    a.borsh_len(authorizations.size());
    for (auto& x : authorizations) borsh(a, x);
    Writer w;
    w.raw(*cbr);
    w.raw(*txc);
    w.raw(bt::blake3(a.buf));
    return bt::blake3(w.buf);
}

// ---------------------------------------------------------------- limits
double powi(double a, int b)
{
    const bool recip = b < 0;
    double r = 1;
    while (true) {
        if (b & 1) r *= a;
        b /= 2;
        if (b == 0) break;
        a *= a;
    }
    return recip ? 1 / r : r;
}

static uint64_t growing_limit(uint64_t start, uint32_t height)
{
    uint32_t month = height / (6 * 24 * 30);
    if (month < 120) {
        double v = double(start) * powi(1.04, int(month)); // START as f64 * 1.04f64.powi(month)
        return uint64_t(std::floor(v));
    }
    return start * 111;
}

uint64_t body_sigops_limit(uint32_t height) { return growing_limit(42800, height); }
uint64_t body_size_limit(uint32_t height) { return growing_limit(8ull * 1024 * 1024, height); }

// ---------------------------------------------------------------- authorizations
bool verify_authorizations(const Body& body)
{
    size_t required = body.inputs_len();
    if (body.authorizations.size() != required) return false;
    size_t k = 0;
    for (auto& tx : body.transactions) {
        Bytes msg = tx.canonical_bytes();
        for (size_t i = 0; i < tx.inputs.size(); i++, k++) {
            auto& a = body.authorizations[k];
            if (!bt::frost::verify(a.verifying_key, a.signature, msg)) return false;
        }
    }
    return true;
}

bool verify_authorized_transaction(const AuthorizedTransaction& tx)
{
    if (tx.authorizations.size() != tx.transaction.inputs.size()) return false;
    if (tx.authorizations.empty()) return false; // empty frost batch -> Err(InvalidSignature)
    Bytes msg = tx.transaction.canonical_bytes();
    for (auto& a : tx.authorizations)
        if (!bt::frost::verify(a.verifying_key, a.signature, msg)) return false;
    return true;
}

} // namespace thunder
