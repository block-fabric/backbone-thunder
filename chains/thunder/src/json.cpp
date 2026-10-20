// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <thunder/json.h>

namespace thunder::json {

static UniValue obj() { return UniValue(UniValue::VOBJ); }
static UniValue arr() { return UniValue(UniValue::VARR); }

UniValue hash(const Hash32& h) { return UniValue(bt::hex_encode(h)); }
UniValue btc_hash(const Hash32& h) { return UniValue(bt::hex_encode_rev(h.span())); }
UniValue bytes_array(ByteSpan b)
{
    UniValue a = arr();
    for (uint8_t x : b) a.push_back(UniValue(int(x)));
    return a;
}
UniValue address(const Address& a) { return UniValue(a.to_base58()); }

UniValue outpoint(const OutPoint& o)
{
    UniValue r = obj();
    if (o.kind == OutPoint::Deposit) {
        r.pushKV("Deposit", bt::hex_encode_rev(o.txid.span()) + ":" + std::to_string(o.vout));
        return r;
    }
    UniValue inner = obj();
    inner.pushKV("txid", hash(o.txid));
    inner.pushKV("vout", UniValue(uint64_t(o.vout)));
    r.pushKV(o.kind == OutPoint::Regular ? "Regular" : "Coinbase", inner);
    return r;
}

UniValue content(const Content& c)
{
    UniValue r = obj();
    if (c.kind == Content::Value) {
        r.pushKV("Value", UniValue(c.value));
    } else {
        UniValue w = obj();
        w.pushKV("value_sats", UniValue(c.value));
        w.pushKV("main_fee_sats", UniValue(c.main_fee));
        w.pushKV("main_address", c.main_address.to_string());
        r.pushKV("Withdrawal", w);
    }
    return r;
}

UniValue output(const Output& o)
{
    UniValue r = obj();
    r.pushKV("address", address(o.address));
    r.pushKV("content", content(o.content));
    return r;
}

UniValue pointed_output(const OutPoint& op, const Output& o)
{
    UniValue r = obj();
    r.pushKV("outpoint", outpoint(op));
    r.pushKV("output", output(o));
    return r;
}

UniValue spent_output(const SpentOutput& s)
{
    UniValue r = obj();
    r.pushKV("output", output(s.output));
    UniValue ip = obj();
    if (s.inpoint.kind == InPoint::Regular) {
        UniValue i = obj();
        i.pushKV("txid", hash(s.inpoint.id));
        i.pushKV("vin", UniValue(uint64_t(s.inpoint.vin)));
        ip.pushKV("Regular", i);
    } else {
        UniValue i = obj();
        i.pushKV("m6id", btc_hash(s.inpoint.id));
        ip.pushKV("Withdrawal", i);
    }
    r.pushKV("inpoint", ip);
    return r;
}

UniValue pointed_spent_output(const OutPoint& op, const SpentOutput& s)
{
    UniValue r = obj();
    r.pushKV("outpoint", outpoint(op));
    r.pushKV("output", spent_output(s));
    return r;
}

UniValue node_hash(const bt::utreexo::NodeHash& n)
{
    if (n.tag == bt::utreexo::NodeHash::Empty) return UniValue("Empty");
    if (n.tag == bt::utreexo::NodeHash::Placeholder) return UniValue("Placeholder");
    UniValue r = obj();
    r.pushKV("Some", bytes_array(n.h.span()));
    return r;
}

UniValue proof(const bt::utreexo::Proof& p)
{
    UniValue r = obj();
    UniValue t = arr(), h = arr();
    for (auto x : p.targets) t.push_back(UniValue(x));
    for (auto& x : p.hashes) h.push_back(node_hash(x));
    r.pushKV("targets", t);
    r.pushKV("hashes", h);
    return r;
}

static void tx_fields(UniValue& r, const Transaction& t)
{
    UniValue ins = arr();
    for (auto& [op, h] : t.inputs) {
        UniValue pair = arr();
        pair.push_back(outpoint(op));
        pair.push_back(bytes_array(h.span())); // Hash = [u8; 32]
        ins.push_back(pair);
    }
    r.pushKV("inputs", ins);
    r.pushKV("proof", proof(t.proof));
    UniValue outs = arr();
    for (auto& o : t.outputs) outs.push_back(output(o));
    r.pushKV("outputs", outs);
}

UniValue transaction(const Transaction& t)
{
    UniValue r = obj();
    tx_fields(r, t);
    return r;
}

UniValue authorization(const Authorization& a)
{
    UniValue r = obj();
    r.pushKV("verifying_key", bt::hex_encode(a.verifying_key));
    bt::Bytes sig(a.signature.R.begin(), a.signature.R.end());
    sig.insert(sig.end(), a.signature.z.begin(), a.signature.z.end());
    r.pushKV("signature", bt::hex_encode(sig));
    return r;
}

UniValue authorized_transaction(const AuthorizedTransaction& t)
{
    UniValue r = obj();
    r.pushKV("transaction", transaction(t.transaction));
    UniValue a = arr();
    for (auto& x : t.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    return r;
}

UniValue coinbase(const Coinbase& c)
{
    UniValue r = obj();
    r.pushKV("memo", bytes_array(c.memo));
    UniValue o = arr();
    for (auto& x : c.outputs) o.push_back(output(x));
    r.pushKV("outputs", o);
    return r;
}

UniValue body(const Body& b, bool verbose)
{
    UniValue r = obj();
    r.pushKV("coinbase", coinbase(b.coinbase));
    UniValue txs = arr();
    for (auto& t : b.transactions) {
        UniValue tv = obj();
        tx_fields(tv, t);
        if (verbose) tv.pushKV("canonical_bytes", bt::hex_encode(t.canonical_bytes())); // #[serde(flatten)]
        txs.push_back(tv);
    }
    r.pushKV("transactions", txs);
    UniValue a = arr();
    for (auto& x : b.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    return r;
}

UniValue header(const Header& h)
{
    UniValue r = obj();
    r.pushKV("merkle_root", hash(h.merkle_root));
    r.pushKV("prev_side_hash", h.prev_side_hash ? hash(*h.prev_side_hash) : UniValue());
    r.pushKV("prev_main_hash", btc_hash(h.prev_main_hash));
    UniValue roots = arr();
    for (auto& x : h.roots) roots.push_back(UniValue(x.to_string())); // DisplayFromStr
    r.pushKV("roots", roots);
    return r;
}

UniValue block(const Header& h, const Body& b, bool verbose)
{
    UniValue r = obj();
    r.pushKV("header", header(h));
    r.pushKV("body", body(b, verbose));
    return r;
}

UniValue bitcoin_tx(const std::vector<bt::btc::TxOut>& outs)
{
    UniValue r = obj();
    r.pushKV("version", 2);
    r.pushKV("lock_time", 0);
    r.pushKV("input", arr());
    UniValue o = arr();
    for (auto& x : outs) {
        UniValue t = obj();
        t.pushKV("value", UniValue(x.value));
        t.pushKV("script_pubkey", bt::hex_encode(x.script_pubkey));
        o.push_back(t);
    }
    r.pushKV("output", o);
    return r;
}

UniValue withdrawal_bundle(const WithdrawalBundle& w)
{
    UniValue r = obj();
    UniValue su = arr();
    for (auto& [op, o] : w.spend_utxos) {
        UniValue p = arr();
        p.push_back(outpoint(op));
        p.push_back(output(o));
        su.push_back(p);
    }
    r.pushKV("spend_utxos", su);
    r.pushKV("tx", bitcoin_tx(w.outputs));
    return r;
}

UniValue withdrawal_bundle_info(const WithdrawalBundleInfo& i)
{
    if (i.kind == WithdrawalBundleInfo::Unknown) return UniValue("Unknown");
    UniValue r = obj();
    if (i.kind == WithdrawalBundleInfo::Known) {
        r.pushKV("Known", withdrawal_bundle(i.bundle));
    } else {
        // serde_json cannot serialize a map with OutPoint keys; theirs fails here
        throw Error("key must be a string");
    }
    return r;
}

// ------------------------------------------------------------------ parsing
uint64_t parse_u64(const UniValue& v)
{
    if (!v.isNum()) throw Error("expected an integer");
    const std::string& s = v.getValStr();
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw Error("expected an unsigned integer");
    try {
        size_t pos;
        unsigned long long x = std::stoull(s, &pos);
        if (pos != s.size()) throw Error("bad integer");
        return x;
    } catch (const std::out_of_range&) {
        throw Error("integer out of range");
    }
}

Hash32 parse_hash(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a hex string");
    auto h = bt::hex_to_array<32>(v.get_str());
    if (!h) throw Error("invalid hash");
    return *h;
}
Hash32 parse_btc_hash(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a hex string");
    auto h = bt::hash_from_rev_hex(v.get_str());
    if (!h) throw Error("invalid hash");
    return *h;
}
Address parse_address(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected an address string");
    auto a = Address::from_base58(v.get_str());
    if (!a) throw Error("invalid address");
    return *a;
}
bt::btc::Address parse_btc_address(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a bitcoin address");
    auto a = bt::btc::Address::parse(v.get_str());
    if (!a) throw Error("invalid bitcoin address");
    return *a;
}

static Hash32 parse_byte_array32(const UniValue& v)
{
    if (v.isStr()) return parse_hash(v); // tolerate hex
    if (!v.isArray() || v.size() != 32) throw Error("expected 32 bytes");
    Hash32 h;
    for (size_t i = 0; i < 32; i++) {
        uint64_t b = parse_u64(v[i]);
        if (b > 255) throw Error("byte out of range");
        h[i] = uint8_t(b);
    }
    return h;
}

OutPoint parse_outpoint(const UniValue& v)
{
    if (!v.isObject() || v.size() != 1) throw Error("invalid OutPoint");
    const std::string& k = v.getKeys()[0];
    const UniValue& x = v.getValues()[0];
    OutPoint o;
    if (k == "Deposit") {
        if (!x.isStr()) throw Error("invalid deposit outpoint");
        auto s = x.get_str();
        auto c = s.rfind(':');
        if (c == std::string::npos) throw Error("invalid deposit outpoint");
        o.kind = OutPoint::Deposit;
        o.txid = parse_btc_hash(UniValue(s.substr(0, c)));
        o.vout = uint32_t(std::stoul(s.substr(c + 1)));
        return o;
    }
    if (k != "Regular" && k != "Coinbase") throw Error("invalid OutPoint variant");
    o.kind = k == "Regular" ? OutPoint::Regular : OutPoint::Coinbase;
    o.txid = parse_hash(x["txid"]);
    uint64_t vout = parse_u64(x["vout"]);
    if (vout > UINT32_MAX) throw Error("vout out of range");
    o.vout = uint32_t(vout);
    return o;
}

Output parse_output(const UniValue& v)
{
    Output o;
    o.address = parse_address(v["address"]);
    const UniValue& c = v["content"];
    if (!c.isObject() || c.size() != 1) throw Error("invalid content");
    const std::string& k = c.getKeys()[0];
    const UniValue& x = c.getValues()[0];
    if (k == "Value") {
        o.content.kind = Content::Value;
        o.content.value = parse_u64(x);
    } else if (k == "Withdrawal") {
        o.content.kind = Content::Withdrawal;
        o.content.value = parse_u64(x["value_sats"]);
        o.content.main_fee = parse_u64(x["main_fee_sats"]);
        o.content.main_address = parse_btc_address(x["main_address"]);
    } else {
        throw Error("invalid content variant");
    }
    return o;
}

static bt::utreexo::NodeHash parse_node_hash(const UniValue& v)
{
    if (v.isStr()) {
        if (v.get_str() == "Empty") return bt::utreexo::NodeHash::empty();
        if (v.get_str() == "Placeholder") return bt::utreexo::NodeHash{bt::utreexo::NodeHash::Placeholder, {}};
        throw Error("invalid node hash");
    }
    if (!v.isObject() || v.size() != 1 || v.getKeys()[0] != "Some") throw Error("invalid node hash");
    return bt::utreexo::NodeHash::some(parse_byte_array32(v.getValues()[0]));
}

bt::utreexo::Proof parse_proof(const UniValue& v)
{
    bt::utreexo::Proof p;
    if (v.isNull()) return p;
    for (auto& t : v["targets"].getValues()) p.targets.push_back(parse_u64(t));
    for (auto& h : v["hashes"].getValues()) p.hashes.push_back(parse_node_hash(h));
    return p;
}

Transaction parse_transaction(const UniValue& v)
{
    if (!v.isObject()) throw Error("invalid transaction");
    Transaction t;
    for (auto& in : v["inputs"].getValues()) {
        if (!in.isArray() || in.size() != 2) throw Error("invalid input");
        t.inputs.emplace_back(parse_outpoint(in[0]), parse_byte_array32(in[1]));
    }
    t.proof = parse_proof(v["proof"]);
    for (auto& o : v["outputs"].getValues()) t.outputs.push_back(parse_output(o));
    return t;
}

static Authorization parse_authorization(const UniValue& v)
{
    Authorization a;
    auto vk = bt::hex_to_array<32>(v["verifying_key"].getValStr());
    auto sig = bt::hex_decode(v["signature"].getValStr());
    if (!vk || !sig || sig->size() != 64) throw Error("invalid authorization");
    a.verifying_key = *vk;
    std::memcpy(a.signature.R.data(), sig->data(), 32);
    std::memcpy(a.signature.z.data(), sig->data() + 32, 32);
    if (!bt::frost::valid_element(a.verifying_key) || !bt::frost::valid_element(a.signature.R) || !bt::frost::canonical_scalar(a.signature.z))
        throw Error("invalid authorization encoding");
    return a;
}

AuthorizedTransaction parse_authorized_transaction(const UniValue& v)
{
    AuthorizedTransaction t;
    t.transaction = parse_transaction(v["transaction"]);
    for (auto& a : v["authorizations"].getValues()) t.authorizations.push_back(parse_authorization(a));
    return t;
}

Header parse_header(const UniValue& v)
{
    Header h;
    h.merkle_root = parse_hash(v["merkle_root"]);
    if (!v["prev_side_hash"].isNull()) h.prev_side_hash = parse_hash(v["prev_side_hash"]);
    h.prev_main_hash = parse_btc_hash(v["prev_main_hash"]);
    for (auto& r : v["roots"].getValues()) h.roots.push_back(bt::utreexo::NodeHash::some(parse_hash(r))); // FromStr: hex only
    return h;
}

Body parse_body(const UniValue& v)
{
    Body b;
    const UniValue& cb = v["coinbase"];
    for (auto& m : cb["memo"].getValues()) {
        uint64_t x = parse_u64(m);
        if (x > 255) throw Error("memo byte out of range");
        b.coinbase.memo.push_back(uint8_t(x));
    }
    for (auto& o : cb["outputs"].getValues()) b.coinbase.outputs.push_back(parse_output(o));
    for (auto& t : v["transactions"].getValues()) b.transactions.push_back(parse_transaction(t));
    for (auto& a : v["authorizations"].getValues()) b.authorizations.push_back(parse_authorization(a));
    return b;
}

} // namespace thunder::json
