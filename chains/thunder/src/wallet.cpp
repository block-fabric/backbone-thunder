// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <thunder/wallet.h>

#include <algorithm>

namespace thunder {

using bt::kv::Txn;

enum : uint8_t { W_SEED = 1, W_INDEX_TO_ADDR = 2, W_ADDR_TO_INDEX = 3 };

Wallet::Wallet(const std::string& path, Node& node, bt::hd::Variant variant) : store_(bt::kv::open_sqlite(path)), node_(node), variant_(variant) {}

std::optional<bt::ByteArray<64>> Wallet::seed()
{
    Txn t(*store_);
    auto v = t.get(W_SEED, bt::Bytes{0});
    if (!v || v->size() != 64) return std::nullopt;
    return bt::ByteArray<64>::from_span(*v);
}

bool Wallet::has_seed()
{
    std::lock_guard<std::mutex> l(mu_);
    return seed().has_value();
}

void Wallet::set_seed(const bt::ByteArray<64>& s)
{
    std::lock_guard<std::mutex> l(mu_);
    auto cur = seed();
    if (cur) {
        if (*cur == s) return;
        throw std::runtime_error("seed already exists");
    }
    Txn t(*store_);
    t.put(W_SEED, bt::Bytes{0}, s.span());
    t.clear(W_INDEX_TO_ADDR);
    t.clear(W_ADDR_TO_INDEX);
    t.commit();
}

void Wallet::set_seed_from_mnemonic(const std::string& mnemonic)
{
    auto p = bt::hd::parse_mnemonic(mnemonic);
    // tiny-bip39's words, as their set_seed_from_mnemonic answers (code -1).
    if (!p) throw std::runtime_error(bt::hd::mnemonic_error(mnemonic));
    set_seed(bt::hd::mnemonic_to_seed(*p));
}

bt::frost::SigningKey Wallet::key(uint32_t index)
{
    auto s = seed();
    if (!s) throw std::runtime_error("no seed");
    return bt::hd::sidechain_signing_key(*s, THIS_SIDECHAIN, index, variant_);
}

static Bytes be32(uint32_t v)
{
    Writer w;
    w.u32be(v);
    return w.buf;
}

Address Wallet::get_new_address()
{
    std::lock_guard<std::mutex> l(mu_);
    Txn t(*store_);
    uint32_t last = 0;
    if (auto lv = t.last(W_INDEX_TO_ADDR)) last = uint32_t(lv->first[0]) << 24 | uint32_t(lv->first[1]) << 16 | uint32_t(lv->first[2]) << 8 | lv->first[3];
    uint32_t index = last + 1;
    Address a = address_from_vk(key(index).verifying_key());
    t.put(W_INDEX_TO_ADDR, be32(index), a.b.span());
    t.put(W_ADDR_TO_INDEX, a.b.span(), be32(index));
    t.commit();
    return a;
}

Address Wallet::get_or_generate_last_address()
{
    {
        std::lock_guard<std::mutex> l(mu_);
        Txn t(*store_);
        if (auto lv = t.last(W_INDEX_TO_ADDR)) {
            Address a;
            std::memcpy(a.b.data(), lv->second.data(), 20);
            return a;
        }
    }
    return get_new_address();
}

std::vector<Address> Wallet::addresses()
{
    std::lock_guard<std::mutex> l(mu_);
    Txn t(*store_);
    std::vector<Address> r;
    t.scan(W_INDEX_TO_ADDR, [&](bt::ByteSpan, bt::ByteSpan v) {
        Address a;
        std::memcpy(a.b.data(), v.data(), 20);
        r.push_back(a);
        return true;
    });
    // rpc_server.rs: sorted by base58 text
    std::sort(r.begin(), r.end(), [](const Address& a, const Address& b) { return a.to_base58() < b.to_base58(); });
    return r;
}

std::vector<std::pair<OutPoint, Output>> Wallet::utxos()
{
    auto a = addresses();
    return node_.utxos_by_addresses(std::set<Address>(a.begin(), a.end()));
}

Balance Wallet::balance()
{
    Balance b;
    for (auto& [op, o] : utxos()) {
        b.total += o.get_value();
        if (o.content.kind != Content::Withdrawal) b.available += o.get_value();
    }
    return b;
}

// wallet/mod.rs:362-395
std::pair<uint64_t, std::vector<std::pair<OutPoint, Output>>> Wallet::select_coins(uint64_t value)
{
    auto u = utxos();
    std::stable_sort(u.begin(), u.end(), [](auto& a, auto& b) { return a.second.get_value() < b.second.get_value(); });
    uint64_t total = 0;
    std::vector<std::pair<OutPoint, Output>> sel;
    for (auto& [op, o] : u) {
        if (o.content.kind == Content::Withdrawal) continue;
        if (total > value) break;
        total += o.get_value();
        sel.emplace_back(op, o);
    }
    if (total < value) throw std::runtime_error("not enough funds");
    return {total, sel};
}

static std::pair<std::vector<std::pair<OutPoint, Hash32>>, bt::utreexo::Proof> inputs_with_proof(Node& node, const std::vector<std::pair<OutPoint, Output>>& coins)
{
    std::vector<std::pair<OutPoint, Hash32>> ins;
    std::vector<bt::utreexo::NodeHash> targets;
    for (auto& [op, o] : coins) {
        Hash32 h = pointed_output_hash(op, o);
        ins.emplace_back(op, h);
        targets.push_back(bt::utreexo::NodeHash::some(h));
    }
    auto acc = node.tip_accumulator();
    auto p = acc.prove(targets);
    if (!p) throw std::runtime_error("utreexo: cannot prove inputs");
    return {ins, *p};
}

Transaction Wallet::create_transaction_many(const std::map<Address, uint64_t>& dests, uint64_t fee)
{
    if (dests.empty()) throw std::runtime_error("no transfer destination");
    uint64_t value = 0;
    for (auto& [a, v] : dests) {
        if (value + v < value) throw std::runtime_error("amount overflow");
        value += v;
    }
    if (value + fee < value) throw std::runtime_error("amount overflow");
    auto [total, coins] = select_coins(value + fee);
    uint64_t change = total - value - fee;
    auto [ins, proof] = inputs_with_proof(node_, coins);
    Transaction tx;
    tx.inputs = ins;
    tx.proof = proof;
    for (auto& [a, v] : dests) tx.outputs.push_back(Output{a, Content{Content::Value, v, 0, {}}});
    tx.outputs.push_back(Output{get_new_address(), Content{Content::Value, change, 0, {}}});
    return tx;
}

Transaction Wallet::create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee)
{
    auto [total, coins] = select_coins(value + fee + main_fee);
    uint64_t change = total - value - fee - main_fee;
    auto [ins, proof] = inputs_with_proof(node_, coins);
    Transaction tx;
    tx.inputs = ins;
    tx.proof = proof;
    Content w;
    w.kind = Content::Withdrawal;
    w.value = value;
    w.main_fee = main_fee;
    w.main_address = main_address;
    tx.outputs.push_back(Output{get_new_address(), w});
    tx.outputs.push_back(Output{get_new_address(), Content{Content::Value, change, 0, {}}});
    return tx;
}

AuthorizedTransaction Wallet::authorize(const Transaction& tx)
{
    AuthorizedTransaction at;
    at.transaction = tx;
    Bytes msg = tx.canonical_bytes();
    for (auto& [op, _] : tx.inputs) {
        auto all = node_.utxos();
        auto it = std::find_if(all.begin(), all.end(), [&](auto& p) { return p.first == op; });
        if (it == all.end()) throw std::runtime_error("no utxo " + op.to_string());
        uint32_t index;
        {
            std::lock_guard<std::mutex> l(mu_);
            Txn t(*store_);
            auto v = t.get(W_ADDR_TO_INDEX, it->second.address.b.span());
            if (!v) throw std::runtime_error("no index for address " + it->second.address.to_base58());
            index = uint32_t((*v)[0]) << 24 | uint32_t((*v)[1]) << 16 | uint32_t((*v)[2]) << 8 | (*v)[3];
        }
        auto sk = key(index);
        at.authorizations.push_back(Authorization{sk.verifying_key(), sk.sign(msg)});
    }
    return at;
}

} // namespace thunder
