// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/mainchain.h>

#include <bt/parse.h>

#include <univalue.h>

namespace bt::mainchain {

void TwoWayPegData::replace(const Hash32& h, const BlockInfo& bi)
{
    for (auto& [k, v] : block_info)
        if (k == h) {
            v = bi;
            return;
        }
    block_info.emplace_back(h, bi);
}

static std::optional<std::string> hexfield(const UniValue& v, const char* name)
{
    const UniValue& f = v.find_value(name);
    if (!f.isObject()) return std::nullopt;
    const UniValue& h = f.find_value("hex");
    if (!h.isStr()) return std::nullopt;
    return h.get_str();
}

static uint64_t u64field(const UniValue& v, const char* name, bool& ok)
{
    const UniValue& f = v.find_value(name);
    if (f.isNull()) return 0; // proto3 default
    try {
        if (f.isStr()) {
            if (auto v = parse_u64(f.get_str())) return *v;
            ok = false;
            return 0;
        }
        if (f.isNum()) return f.getInt<uint64_t>();
    } catch (...) {
    }
    ok = false;
    return 0;
}

std::optional<BlockHeaderInfo> header_info_from_json(const UniValue& v)
{
    BlockHeaderInfo h;
    auto bh = hexfield(v, "blockHash");
    auto ph = hexfield(v, "prevBlockHash");
    if (!bh || !ph) return std::nullopt;
    auto b = hash_from_rev_hex(*bh);
    auto p = hash_from_rev_hex(*ph);
    if (!b || !p) return std::nullopt;
    h.block_hash = *b;
    h.prev_block_hash = *p;
    bool ok = true;
    h.height = uint32_t(u64field(v, "height", ok));
    h.timestamp = u64field(v, "timestamp", ok);
    if (auto w = hexfield(v, "work")) {
        auto wb = hex_to_array<32>(*w);
        if (wb) h.work = *wb;
    }
    if (!ok) return std::nullopt;
    return h;
}

std::optional<BlockInfo> block_info_from_json(const UniValue& v)
{
    BlockInfo bi;
    if (auto c = hexfield(v, "bmmCommitment")) {
        auto h = hex_to_array<32>(*c);
        if (!h) return std::nullopt;
        bi.bmm_commitment = *h;
    }
    const UniValue& evs = v.find_value("events");
    if (evs.isArray()) {
        for (auto& e : evs.getValues()) {
            const UniValue& d = e.find_value("deposit");
            const UniValue& w = e.find_value("withdrawalBundle");
            if (d.isObject()) {
                Deposit dep;
                bool ok = true;
                dep.sequence_number = u64field(d, "sequenceNumber", ok);
                const UniValue& op = d.find_value("outpoint");
                auto txid = op.isObject() ? hexfield(op, "txid") : std::nullopt;
                if (!txid) return std::nullopt;
                auto t = hash_from_rev_hex(*txid);
                if (!t) return std::nullopt;
                dep.txid = *t;
                dep.vout = uint32_t(u64field(op, "vout", ok));
                const UniValue& out = d.find_value("output");
                if (!out.isObject()) return std::nullopt;
                auto addr = hexfield(out, "address");
                if (!addr) return std::nullopt;
                auto ab = hex_decode(*addr);
                if (!ab) return std::nullopt;
                dep.address = *ab;
                dep.value_sats = u64field(out, "valueSats", ok);
                if (!ok) return std::nullopt;
                bi.events.emplace_back(dep);
            } else if (w.isObject()) {
                WithdrawalBundleEvent we;
                auto m = hexfield(w, "m6id");
                if (!m) return std::nullopt;
                auto mb = hex_to_array<32>(*m);
                if (!mb) return std::nullopt;
                we.m6id = *mb;
                const UniValue& ev = w.find_value("event");
                if (!ev.isObject()) return std::nullopt;
                if (ev.find_value("failed").isObject()) we.status = BundleStatus::Failed;
                else if (ev.find_value("submitted").isObject()) we.status = BundleStatus::Submitted;
                else if (ev.find_value("succeeded").isObject()) {
                    we.status = BundleStatus::Confirmed;
                    const UniValue& s = ev.find_value("succeeded");
                    bool ok = true;
                    if (!s.find_value("sequenceNumber").isNull()) we.sequence_number = u64field(s, "sequenceNumber", ok);
                    if (auto tx = hexfield(s, "transaction")) {
                        if (auto tb = hex_decode(*tx)) we.transaction = *tb;
                    }
                } else {
                    return std::nullopt;
                }
                bi.events.emplace_back(we);
            } else {
                return std::nullopt; // missing event
            }
        }
    }
    return bi;
}

} // namespace bt::mainchain
