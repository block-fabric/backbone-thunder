// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/enforcer.h>
#include <bt/protobuf.h>

namespace bt::enforcer {

using pb::Encoder;
using pb::Message;

static const std::string V = "/cusf.mainchain.v1.ValidatorService/";
static const std::string W = "/cusf.mainchain.v1.WalletService/";
static const std::string M = "/cusf.mainchain.v1.MiningService/";
static const std::string BP = "/cusf.mainchain.v1.BlockProducerService/";

Client::Client(const std::string& url, uint8_t sidechain, std::string ca_file)
    : url_(url), ca_(std::move(ca_file)), slot_(sidechain), ch_(std::make_unique<grpc::Channel>(url, ca_))
{
}

static std::string rev_hex(const Hash32& h) { return hex_encode_rev(h.span()); }

static std::optional<Hash32> parse_rev(const Message& m, uint32_t f)
{
    auto h = m.hex_msg(f);
    if (!h) return std::nullopt;
    return hash_from_rev_hex(*h);
}

static std::optional<Hash32> parse_consensus32(const Message& m, uint32_t f)
{
    auto h = m.hex_msg(f);
    if (!h) return std::nullopt;
    return hex_to_array<32>(*h);
}

std::optional<mainchain::BlockHeaderInfo> parse_header_info(ByteSpan b)
{
    auto m = Message::parse(b);
    if (!m) return std::nullopt;
    mainchain::BlockHeaderInfo h;
    auto bh = parse_rev(*m, 1), ph = parse_rev(*m, 2);
    if (!bh || !ph) return std::nullopt;
    h.block_hash = *bh;
    h.prev_block_hash = *ph;
    h.height = uint32_t(m->u64(3).value_or(0));
    auto w = parse_consensus32(*m, 4);
    if (!w) return std::nullopt; // missing work is an error in theirs
    h.work = *w;
    h.timestamp = m->u64(5).value_or(0);
    return h;
}

std::optional<mainchain::BlockInfo> parse_block_info(ByteSpan b)
{
    auto m = Message::parse(b);
    if (!m) return std::nullopt;
    mainchain::BlockInfo bi;
    if (m->has(1)) {
        auto c = parse_consensus32(*m, 1);
        if (!c) return std::nullopt;
        bi.bmm_commitment = *c;
    }
    for (auto& ev : m->msgs(2)) {
        if (auto d = ev.msg(1)) {
            mainchain::Deposit dep;
            auto seq = d->u64_value(1);
            auto op = d->msg(2);
            auto out = d->msg(3);
            if (!seq || !op || !out) return std::nullopt; // missing_field
            dep.sequence_number = *seq;
            auto txid = parse_rev(*op, 1);
            auto vout = op->u64_value(2);
            if (!txid || !vout) return std::nullopt;
            dep.txid = *txid;
            dep.vout = uint32_t(*vout);
            auto addr = out->hex_msg(2);
            auto value = out->u64_value(3);
            if (!addr || !value) return std::nullopt;
            auto ab = hex_decode(*addr);
            if (!ab) return std::nullopt;
            dep.address = *ab;
            dep.value_sats = *value;
            bi.events.emplace_back(dep);
        } else if (auto w = ev.msg(2)) {
            mainchain::WithdrawalBundleEvent we;
            auto m6 = parse_consensus32(*w, 1);
            auto e = w->msg(2);
            if (!m6 || !e) return std::nullopt;
            we.m6id = *m6;
            if (e->has(1)) we.status = mainchain::BundleStatus::Failed;
            else if (e->has(2)) {
                we.status = mainchain::BundleStatus::Confirmed;
                auto s = e->msg(2);
                if (s) {
                    if (auto sq = s->u64_value(1)) we.sequence_number = *sq;
                    if (auto tx = s->hex_msg(2))
                        if (auto tb = hex_decode(*tx)) we.transaction = *tb;
                }
            } else if (e->has(3)) we.status = mainchain::BundleStatus::Submitted;
            else return std::nullopt;
            bi.events.emplace_back(we);
        } else {
            return std::nullopt;
        }
    }
    return bi;
}

static Error E(const grpc::Status& s) { return Error{s}; }

std::optional<bool> Client::health(const std::string& service)
{
    Encoder e;
    e.str(1, service);
    Bytes resp;
    auto s = ch_->call("/grpc.health.v1.Health/Check", e.buf, resp, 30000);
    if (!s.ok()) return std::nullopt;
    auto m = Message::parse(resp);
    if (!m) return std::nullopt;
    return m->u64(1).value_or(0) == 1; // SERVING
}

Result<mainchain::BlockHeaderInfo> Client::get_chain_tip()
{
    Bytes resp;
    auto s = ch_->call(V + "GetChainTip", {}, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GetChainTipResponse"});
    auto hb = m->bytes(1);
    if (!hb) return E({-1, "missing block_header_info"});
    auto h = parse_header_info(*hb);
    if (!h) return E({-1, "bad BlockHeaderInfo"});
    return *h;
}

Result<ChainInfo> Client::get_chain_info()
{
    Bytes resp;
    auto s = ch_->call(V + "GetChainInfo", {}, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GetChainInfoResponse"});
    ChainInfo ci;
    ci.network = int(m->u64(1).value_or(0));
    if (auto c = m->msg(2)) ci.activation_height = uint32_t(c->u64(7).value_or(0));
    return ci;
}

Result<std::vector<mainchain::BlockHeaderInfo>> Client::get_block_header_infos(const Hash32& block, uint32_t max_ancestors)
{
    Encoder e;
    e.hex_msg(1, rev_hex(block));
    e.u64(2, max_ancestors);
    Bytes resp;
    auto s = ch_->call(V + "GetBlockHeaderInfo", e.buf, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GetBlockHeaderInfoResponse"});
    std::vector<mainchain::BlockHeaderInfo> r;
    if (auto a = m->all(1))
        for (auto& v : *a) {
            auto h = parse_header_info(v.b);
            if (!h) return E({-1, "bad header info"});
            r.push_back(*h);
        }
    for (size_t i = 1; i < r.size(); i++)
        if (r[i].block_hash != r[i - 1].prev_block_hash) return E({-1, "invalid repeated value header_infos"});
    return r;
}

Result<std::vector<std::pair<mainchain::BlockHeaderInfo, mainchain::BlockInfo>>> Client::get_block_infos(const Hash32& block, uint32_t max_ancestors)
{
    Encoder e;
    e.hex_msg(1, rev_hex(block));
    e.u32_value(2, slot_);
    e.u64(3, max_ancestors);
    Bytes resp;
    auto s = ch_->call(V + "GetBlockInfo", e.buf, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GetBlockInfoResponse"});
    std::vector<std::pair<mainchain::BlockHeaderInfo, mainchain::BlockInfo>> r;
    for (auto& info : m->msgs(1)) {
        auto hb = info.bytes(1), bb = info.bytes(2);
        if (!hb || !bb) return E({-1, "missing header_info/block_info"});
        auto h = parse_header_info(*hb);
        auto b = parse_block_info(*bb);
        if (!h || !b) return E({-1, "bad block info"});
        r.emplace_back(*h, *b);
    }
    for (size_t i = 1; i < r.size(); i++)
        if (r[i].first.block_hash != r[i - 1].first.prev_block_hash) return E({-1, "invalid repeated value infos"});
    return r;
}

Result<std::vector<std::optional<Hash32>>> Client::get_bmm_hstar_commitments(const Hash32& block, uint32_t max_ancestors)
{
    Encoder e;
    e.hex_msg(1, rev_hex(block));
    e.u32_value(2, slot_);
    e.u64(3, max_ancestors);
    Bytes resp;
    auto s = ch_->call(V + "GetBmmHStarCommitment", e.buf, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GetBmmHStarCommitmentResponse"});
    if (m->has(1)) return E({grpc::Status::NOT_FOUND, "block not found"});
    auto c = m->msg(2);
    if (!c) return E({-1, "missing result"});
    std::vector<std::optional<Hash32>> r;
    r.push_back(c->has(1) ? parse_consensus32(*c, 1) : std::nullopt);
    for (auto& a : c->msgs(2)) r.push_back(a.has(1) ? parse_consensus32(a, 1) : std::nullopt);
    return r;
}

Result<std::vector<std::pair<mainchain::BlockHeaderInfo, mainchain::BlockInfo>>> Client::get_two_way_peg_data(const std::optional<Hash32>& start,
                                                                                                             const Hash32& end)
{
    Encoder e;
    e.u32_value(1, slot_);
    if (start) e.hex_msg(2, rev_hex(*start));
    e.hex_msg(3, rev_hex(end));
    Bytes resp;
    auto s = ch_->call(V + "GetTwoWayPegData", e.buf, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GetTwoWayPegDataResponse"});
    std::vector<std::pair<mainchain::BlockHeaderInfo, mainchain::BlockInfo>> r;
    for (auto& item : m->msgs(1)) {
        auto hb = item.bytes(1), bb = item.bytes(2);
        if (!hb || !bb) return E({-1, "missing header/block info"});
        auto h = parse_header_info(*hb);
        auto b = parse_block_info(*bb);
        if (!h || !b) return E({-1, "bad item"});
        r.emplace_back(*h, *b);
    }
    return r;
}

grpc::Status Client::subscribe_events(const std::function<bool(const SubscribeEvent&)>& on_event, const std::atomic<bool>* cancel)
{
    // A dedicated channel: the stream holds its connection for its whole life.
    grpc::Channel ch(url_, ca_);
    Encoder e;
    e.u32_value(1, slot_);
    std::function<bool(const Bytes&)> f = [&](const Bytes& msg) {
        auto m = Message::parse(msg);
        if (!m) return false;
        auto ev = m->msg(1);
        if (!ev) return false;
        SubscribeEvent se;
        if (auto c = ev->msg(1)) {
            se.kind = SubscribeEvent::Connect;
            auto hb = c->bytes(1), bb = c->bytes(2);
            if (!hb || !bb) return false;
            auto h = parse_header_info(*hb);
            auto b = parse_block_info(*bb);
            if (!h || !b) return false;
            se.header = *h;
            se.info = *b;
        } else if (auto d = ev->msg(2)) {
            se.kind = SubscribeEvent::Disconnect;
            auto h = parse_rev(*d, 1);
            if (!h) return false;
            se.disconnected = *h;
        } else {
            return false;
        }
        return on_event(se);
    };
    return ch.stream(V + "SubscribeEvents", e.buf, f, cancel);
}

Result<Hash32> Client::create_bmm_critical_data_tx(uint64_t value_sats, uint32_t height, const Hash32& critical_hash, const Hash32& prev_bytes)
{
    Encoder e;
    e.u32_value(1, slot_);
    e.u64_value(2, value_sats);
    e.u32_value(3, height);
    e.hex_msg(4, hex_encode(critical_hash)); // ConsensusHex of [u8; 32] = raw order
    e.hex_msg(5, rev_hex(prev_bytes));
    Bytes resp;
    auto s = ch_->call(W + "CreateBmmCriticalDataTransaction", e.buf, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    auto t = m ? parse_rev(*m, 1) : std::nullopt;
    if (!t) return E({-1, "missing txid"});
    return *t;
}

Result<Hash32> Client::create_deposit_tx(const std::string& address, uint64_t value_sats, uint64_t fee_sats)
{
    Encoder e;
    e.u32_value(1, slot_);
    e.string_value(2, address);
    e.u64_value(3, value_sats);
    e.u64_value(4, fee_sats);
    Bytes resp;
    auto s = ch_->call(W + "CreateDepositTransaction", e.buf, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    auto t = m ? parse_rev(*m, 1) : std::nullopt;
    if (!t) return E({-1, "missing txid"});
    return *t;
}

Result<std::string> Client::create_new_address()
{
    Bytes resp;
    auto s = ch_->call(W + "CreateNewAddress", {}, resp);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad CreateNewAddressResponse"});
    return m->str(1).value_or("");
}

Result<std::vector<Hash32>> Client::generate_to_address(uint32_t blocks, const std::string& address)
{
    Encoder e;
    e.u32_value(1, blocks);
    e.str(2, address);
    Bytes resp;
    auto s = ch_->call(M + "GenerateToAddress", e.buf, resp, 600000);
    if (!s.ok()) return E(s);
    auto m = Message::parse(resp);
    if (!m) return E({-1, "bad GenerateToAddressResponse"});
    std::vector<Hash32> r;
    if (auto a = m->all(1))
        for (auto& v : *a)
            if (auto mm = Message::parse(v.b))
                if (auto h = mm->string_value(1))
                    if (auto hh = hash_from_rev_hex(*h)) r.push_back(*hh);
    return r;
}

Result<bool> Client::propose_withdrawal_bundle(const Bytes& tx)
{
    Encoder e;
    e.u32_value(1, slot_);
    e.bytes_value(2, tx);
    Bytes resp;
    auto s = ch_->call(BP + "ProposeWithdrawalBundle", e.buf, resp);
    if (!s.ok()) return E(s);
    return true;
}

} // namespace bt::enforcer
