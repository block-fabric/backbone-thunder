// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <univalue.h>

#include <thunder/betanet_fixture.h>

#include <fstream>
#include <sstream>

namespace thunder::fixture {

using bt::mainchain::TwoWayPegData;

static std::string slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

std::optional<Data> load(const std::string& dir, std::string& err)
{
    Data d;
    {
        std::istringstream in(slurp(dir + "/blocks.jsonl"));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            UniValue v;
            if (!v.read(line)) { err = "bad blocks.jsonl line"; return std::nullopt; }
            IndexBlock b;
            b.height = uint32_t(v["height"].getInt<int64_t>());
            b.id = *bt::hex_to_array<32>(v["id"].get_str());
            b.merkle_root = *bt::hex_to_array<32>(v["merkle_root"].get_str());
            if (v["previousblockhash"].isStr()) b.prev = *bt::hex_to_array<32>(v["previousblockhash"].get_str());
            b.prev_main = *bt::hash_from_rev_hex(v["mainchain_blockhash"].get_str());
            b.main_height = uint32_t(v["mainchain_height"].getInt<int64_t>());
            b.fees = uint64_t(v["fees"].getInt<int64_t>());
            if (b.height != d.blocks.size()) { err = "blocks.jsonl not contiguous"; return std::nullopt; }
            d.blocks.push_back(b);
        }
    }
    {
        std::istringstream in(slurp(dir + "/txs.jsonl"));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            UniValue v;
            if (!v.read(line)) { err = "bad txs.jsonl"; return std::nullopt; }
            d.txs[uint32_t(v["height"].getInt<int64_t>())] = v["txs"].getValues();
        }
    }
    {
        std::istringstream in(slurp(dir + "/txhex.jsonl"));
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            UniValue v;
            if (!v.read(line)) { err = "bad txhex.jsonl"; return std::nullopt; }
            d.txhex[v["txid"].get_str()] = v["hex"].get_str();
        }
    }
    {
        UniValue v;
        if (!v.read(slurp(dir + "/twpd9.json"))) { err = "bad twpd9.json"; return std::nullopt; }
        const UniValue& arr = v.isArray() ? v : v["blocks"];
        for (auto& item : arr.getValues()) {
            auto hi = bt::mainchain::header_info_from_json(item["blockHeaderInfo"]);
            auto bi = bt::mainchain::block_info_from_json(item["blockInfo"]);
            if (!hi || !bi) { err = "bad twpd item"; return std::nullopt; }
            d.main_height_of[hi->block_hash] = hi->height;
            d.main_by_height[hi->height] = MainBlock{*hi, *bi};
        }
    }
    return d;
}

namespace {
// Build a Transaction from the index's borsh hex, using the JSON for the withdrawal address
// strings (borsh only carries their scriptPubKey).
std::optional<Transaction> build_tx(const Data& d, const UniValue& j, std::string& err)
{
    const std::string& txid = j["txid"].get_str();
    auto it = d.txhex.find(txid);
    if (it == d.txhex.end()) { err = "no hex for " + txid; return std::nullopt; }
    auto raw = bt::hex_decode(it->second);
    if (!raw) { err = "bad hex"; return std::nullopt; }
    Reader r(*raw);
    Transaction t = borsh_de_transaction(r);
    if (!r.empty()) { err = "trailing bytes in borsh tx"; return std::nullopt; }
    if (t.canonical_bytes() != *raw) { err = "borsh re-encode mismatch " + txid; return std::nullopt; }
    if (bt::hex_encode(t.txid()) != txid) { err = "txid mismatch " + txid; return std::nullopt; }
    auto& vout = j["vout"].getValues();
    for (size_t i = 0; i < t.outputs.size() && i < vout.size(); i++) {
        const UniValue& c = vout[i]["content"];
        if (t.outputs[i].content.kind == Content::Withdrawal && c.isObject()) {
            const UniValue& w = c["Withdrawal"];
            auto a = bt::btc::Address::parse(w["main_address"].get_str());
            if (!a) { err = "bad main_address"; return std::nullopt; }
            if (a->script_pubkey() != t.outputs[i].content.main_address.script_pubkey()) { err = "main_address/spk mismatch"; return std::nullopt; }
            t.outputs[i].content.main_address = *a;
        }
    }
    return t;
}
} // namespace

ReplayResult replay(const Data& d, State& st, bt::kv::Store& store, uint32_t max_height)
{
    ReplayResult res;
    if (d.blocks.size() < 2) { res.error = "not enough blocks"; return res; }
    // Real genesis: index height 0 is a stale row from another network (study §Thunder 1).
    Hash32 genesis_id = *d.blocks[1].prev;
    std::optional<uint32_t> genesis_bmm_height;
    for (auto& [h, mb] : d.main_by_height)
        if (mb.info.bmm_commitment == genesis_id) { genesis_bmm_height = h; break; }
    if (!genesis_bmm_height) { res.error = "genesis BMM not found"; return res; }

    // Coinbase outputs revealed by later spends: coinbase txid -> vout -> output
    std::map<Hash32, std::map<uint32_t, Output>> cb_spends;
    for (auto& [h, txs] : d.txs)
        for (auto& j : txs)
            for (auto& vin : j["vin"].getValues())
                if (vin["prevout"]["outpoint_kind"].get_str() == "coinbase") {
                    Output o;
                    o.address = *Address::from_base58(vin["prevout"]["scriptpubkey_address"].get_str());
                    o.content.value = uint64_t(vin["prevout"]["value"].getInt<int64_t>());
                    cb_spends[*bt::hex_to_array<32>(vin["txid"].get_str())][uint32_t(vin["vout"].getInt<int64_t>())] = o;
                }

    std::optional<uint32_t> prev_main_height;
    uint32_t tip = std::min<uint32_t>(uint32_t(d.blocks.size() - 1), max_height);
    for (uint32_t height = 0; height <= tip; height++) {
        IndexBlock ib = d.blocks[height];
        Header h;
        Body body;
        if (height == 0) {
            h.merkle_root = d.blocks[1].merkle_root; // empty body root (checked below)
            h.prev_main_hash = d.main_by_height.at(*genesis_bmm_height).header.prev_block_hash;
            ib.id = genesis_id;
            ib.main_height = *genesis_bmm_height - 1;
            ib.merkle_root = *compute_merkle_root(Coinbase{}, {});
        } else {
            h.merkle_root = ib.merkle_root;
            h.prev_side_hash = ib.prev;
            h.prev_main_hash = ib.prev_main;
        }
        bt::kv::Txn txn(store);
        // transactions
        auto itx = d.txs.find(height);
        if (itx != d.txs.end()) {
            for (auto& j : itx->second) {
                std::string err;
                auto t = build_tx(d, j, err);
                if (!t) { res.error = "height " + std::to_string(height) + ": " + err; return res; }
                body.transactions.push_back(std::move(*t));
            }
        }
        // coinbase inference: empty, or the outputs later spends reveal for this coinbase txid
        Hash32 cbtxid = compute_coinbase_txid(h.merkle_root, h.prev_main_hash, h.prev_side_hash);
        std::vector<Coinbase> candidates{Coinbase{}};
        if (auto c = cb_spends.find(cbtxid); c != cb_spends.end()) {
            Coinbase cb;
            uint32_t n = c->second.rbegin()->first + 1;
            bool complete = true;
            for (uint32_t v = 0; v < n; v++) {
                auto o = c->second.find(v);
                if (o == c->second.end()) { complete = false; break; }
                cb.outputs.push_back(o->second);
            }
            if (complete) candidates.insert(candidates.begin(), cb);
        }
        std::vector<FilledTransaction> filled;
        for (auto& t : body.transactions) {
            FilledTransaction ft{&t, {}};
            for (auto& [op, _] : t.inputs) {
                auto u = st.utxo(txn, op);
                if (!u) { res.error = "height " + std::to_string(height) + ": missing utxo " + op.to_string(); return res; }
                ft.spent_utxos.push_back(*u);
            }
            filled.push_back(ft);
        }
        bool found = false;
        for (auto& cb : candidates) {
            auto mr = compute_merkle_root(cb, filled);
            if (mr && *mr == ib.merkle_root) {
                body.coinbase = cb;
                found = true;
                break;
            }
        }
        if (!found) {
            res.first_unknown_coinbase = height;
            res.error = "height " + std::to_string(height) + ": coinbase not recoverable from public index data";
            res.replayed_to = height ? height - 1 : 0;
            return res;
        }
        res.verified_merkle_roots++;
        // body + roots
        PrevalidatedBlock pre;
        try {
            pre = st.prevalidate(txn, h, body, State::CheckOpts{false, false});
        } catch (const StateError& e) {
            res.error = "height " + std::to_string(height) + ": " + e.what();
            return res;
        }
        h.roots = pre.roots_after;
        if (h.hash() != ib.id) {
            res.error = "height " + std::to_string(height) + ": header hash " + bt::hex_encode(h.hash()) + " != index " + bt::hex_encode(ib.id);
            return res;
        }
        res.verified_hashes++;
        // BMM: commitment == hash in the child of prev_main (archive/mod.rs:886-1015)
        auto bmm = d.main_by_height.find(ib.main_height + 1);
        if (bmm != d.main_by_height.end() && bmm->second.info.bmm_commitment == ib.id && bmm->second.header.prev_block_hash == h.prev_main_hash)
            res.bmm_verified++;
        else
            res.notes.push_back("height " + std::to_string(height) + ": BMM block not in enforcer data");
        try {
            st.connect_prevalidated(txn, h, body, pre);
            // 2WPD window (prev_main(N-1), prev_main(N)] (net_task.rs:363-397); genesis: all ancestors
            TwoWayPegData w;
            for (auto& [mh, mb] : d.main_by_height) {
                if (mh > ib.main_height) break;
                if (prev_main_height && mh <= *prev_main_height) continue;
                w.block_info.emplace_back(mb.header.block_hash, mb.info);
            }
            st.connect_2wpd(txn, w);
        } catch (const StateError& e) {
            res.error = "height " + std::to_string(height) + ": " + e.what();
            return res;
        }
        txn.commit();
        prev_main_height = ib.main_height;
        res.replayed_to = height;
    }
    return res;
}

} // namespace thunder::fixture
