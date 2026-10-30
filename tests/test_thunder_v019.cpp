// Thunder v0.19.1 rules (Rules::V0_19, LayerTwo's mainnet): thunder-rust b44ac16 against v0.18.2 (244e20f). Each test
// pins one change: the authorizations in the merkle root (3a714ea), coinbase and deposit maturity (d99a70c), the
// bundle built after the block's transactions, dropped when not submitted in time, its outputs recreated at
// UnsuccessfulWithdrawal outpoints when it fails or is dropped, fee-density order (ec56149), the mainnet magic (685dc52).
#include "test.h"

#include <thunder/net.h>
#include <thunder/state.h>

#include <bt/crypto.h>

using namespace thunder;
using bt::kv::MemStore;
using bt::kv::Txn;

namespace {

bt::frost::SigningKey key(const std::string& label)
{
    auto h = bt::sha512(bt::ByteSpan((const uint8_t*)label.data(), label.size()));
    return bt::frost::SigningKey{bt::frost::reduce_wide(h)};
}
Address addr(const bt::frost::SigningKey& k) { return address_from_vk(k.verifying_key()); }
Output value_out(const Address& a, uint64_t v) { return Output{a, Content{Content::Value, v, 0, {}}}; }
Output withdrawal_out(const Address& a, uint64_t v, uint64_t fee, const std::string& main)
{
    Content c;
    c.kind = Content::Withdrawal;
    c.value = v;
    c.main_fee = fee;
    c.main_address = *bt::btc::Address::parse(main);
    return Output{a, c};
}
AuthorizedTransaction spend(const std::vector<std::pair<OutPoint, Output>>& ins, const std::vector<Output>& outs, const std::vector<bt::frost::SigningKey>& keys)
{
    AuthorizedTransaction at;
    for (auto& [op, o] : ins) at.transaction.inputs.emplace_back(op, pointed_output_hash(op, o));
    at.transaction.outputs = outs;
    auto msg = at.transaction.canonical_bytes();
    for (auto& k : keys) at.authorizations.push_back(Authorization{k.verifying_key(), k.sign(msg)});
    return at;
}

const char* P2WPKH = "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4";                         // 22-byte script
const char* P2TR = "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0"; // 34-byte script (P2TR, BIP-350)

/** A chain under the v0.19 rules, connected in their order (net_task.rs connect_tip_): 2WPD, transactions, header,
 *  bundle construction. Every block has a mainchain block of its own as prev_main, one height up. */
struct Chain {
    MemStore store;
    State st;
    uint32_t main_height = 1000;
    uint32_t main_counter = 1;
    struct Connected {
        Header header;
        Body body;
        bt::mainchain::TwoWayPegData d;
    };
    std::vector<Connected> blocks;

    Chain() { st.rules = Rules::V0_19; }

    Hash32 next_main()
    {
        Hash32 h;
        h[0] = uint8_t(main_counter);
        h[1] = uint8_t(main_counter >> 8);
        h[31] = 0xbb;
        main_counter++;
        return h;
    }

    /** Builds, roots and connects a block whose window holds `events` (in its own prev_main block); throws StateError. */
    Header connect(const std::vector<AuthorizedTransaction>& txs, const std::vector<bt::mainchain::BlockEvent>& events = {},
                   Coinbase cb = {}, bool commit_auths = true)
    {
        Txn t(store);
        Hash32 main = next_main();
        uint32_t prev_main_height = main_height++;
        bt::mainchain::TwoWayPegData d;
        std::vector<uint32_t> heights;
        if (!events.empty()) {
            bt::mainchain::BlockInfo bi;
            bi.events = events;
            d.block_info.emplace_back(main, bi);
            heights.push_back(prev_main_height);
        }
        st.connect_2wpd(t, d, heights);
        Body b = Body::from_authorized(txs, cb);
        Header h;
        h.prev_side_hash = st.tip(t);
        h.prev_main_hash = main;
        std::vector<FilledTransaction> filled;
        for (auto& tx : b.transactions) {
            FilledTransaction ft{&tx, {}};
            for (auto& [op, _] : tx.inputs) {
                auto u = st.utxo(t, op);
                if (!u) throw StateError("NoUtxo (test builder)");
                ft.spent_utxos.push_back(*u);
            }
            filled.push_back(ft);
        }
        auto mr = commit_auths ? compute_merkle_root(b.coinbase, filled, b.authorizations) : compute_merkle_root(b.coinbase, filled);
        if (!mr) throw StateError("merkle");
        h.merkle_root = *mr;
        auto pre = st.prevalidate(t, h, b, prev_main_height, State::CheckOpts{true, false});
        h.roots = pre.roots_after;
        pre = st.prevalidate(t, h, b, prev_main_height, State::CheckOpts{}); // every check
        st.connect_prevalidated(t, h, b, pre);
        st.connect_header(t, h);
        st.maybe_create_withdrawal_bundle(t);
        t.commit();
        blocks.push_back({h, b, d});
        return h;
    }

    /** net_task.rs disconnect_tip_ (v0.19), without the accumulator snapshot the node puts back. */
    void disconnect()
    {
        auto c = blocks.back();
        blocks.pop_back();
        Txn t(store);
        st.disconnect_withdrawal_bundle_creation(t);
        st.disconnect_tip(t, c.header, c.body);
        st.disconnect_2wpd(t, c.d);
        st.disconnect_tip_header(t, c.header);
        t.commit();
    }

    void empty_blocks(int n)
    {
        for (int i = 0; i < n; i++) connect({});
    }

    std::optional<std::pair<OutPoint, Output>> find_utxo(const Address& a)
    {
        Txn t(store);
        for (auto& [op, o] : st.utxos(t))
            if (o.address == a) return std::make_pair(op, o);
        return std::nullopt;
    }
    std::optional<Output> utxo(const OutPoint& op)
    {
        Txn t(store);
        return st.utxo(t, op);
    }
    std::optional<SpentOutput> stxo(const OutPoint& op)
    {
        Txn t(store);
        return st.stxo(t, op);
    }
    std::optional<Hash32> pending()
    {
        Txn t(store);
        return st.pending_bundle_m6id(t);
    }
    std::optional<std::pair<WithdrawalBundleInfo, RollBack<BundleStatus>>> bundle(const Hash32& m6id)
    {
        Txn t(store);
        return st.bundle(t, m6id);
    }
    uint32_t height()
    {
        Txn t(store);
        return *st.height(t);
    }
    uint64_t wealth()
    {
        Txn t(store);
        return st.sidechain_wealth(t);
    }
};

bt::mainchain::BlockEvent deposit_event(const Address& a, uint64_t value, uint8_t txid_byte)
{
    bt::mainchain::Deposit d;
    d.txid[0] = txid_byte;
    d.txid[31] = 0x5d;
    d.vout = 0;
    std::string s = a.to_base58();
    d.address.assign(s.begin(), s.end());
    d.value_sats = value;
    return d;
}

bt::mainchain::BlockEvent bundle_event(const Hash32& m6id, bt::mainchain::BundleStatus s)
{
    bt::mainchain::WithdrawalBundleEvent e;
    e.m6id = m6id;
    e.status = s;
    return e;
}

bool rejects(const std::function<void()>& f, const std::string& what)
{
    try {
        f();
    } catch (const StateError& e) {
        if (std::string(e.what()).find(what) != std::string::npos) return true;
        std::fprintf(stderr, "  rejected for another reason: %s\n", e.what());
        return false;
    }
    return false;
}

/** A deposit of `value` to `k`, matured: the chain is DEPOSIT_MATURITY mainchain blocks further. */
std::pair<OutPoint, Output> matured_deposit(Chain& c, const bt::frost::SigningKey& k, uint64_t value, uint8_t txid_byte)
{
    c.connect({}, {deposit_event(addr(k), value, txid_byte)});
    c.empty_blocks(int(DEPOSIT_MATURITY));
    return *c.find_utxo(addr(k));
}

} // namespace

TEST(v019_merkle_root_commits_to_authorizations)
{
    // block/body.rs (3a714ea): HashComponents gains blake3(borsh(authorizations))
    auto k = key("v019-auth");
    AuthorizedTransaction tx = spend({}, {value_out(addr(k), 0)}, {});
    Body b = Body::from_authorized({tx}, {});
    std::vector<FilledTransaction> filled{FilledTransaction{&b.transactions[0], {}}};
    auto two = compute_merkle_root(b.coinbase, filled);
    auto three = compute_merkle_root(b.coinbase, filled, b.authorizations);
    CHECK(two && three && *two != *three);
    // an authorization of its own changes the root, even one no input needs
    std::vector<Authorization> extra{Authorization{k.verifying_key(), k.sign(tx.transaction.canonical_bytes())}};
    CHECK(*compute_merkle_root(b.coinbase, filled, extra) != *three);
    // a block whose header root leaves the authorizations out is refused under v0.19
    Chain c;
    c.connect({});
    auto dep = matured_deposit(c, k, 5000, 1);
    auto pay = spend({dep}, {value_out(addr(k), 4000)}, {k});
    CHECK(rejects([&] { c.connect({pay}, {}, {}, false); }, "InvalidBody"));
    c.connect({pay});
}

TEST(v019_coinbase_maturity)
{
    // mod.rs validate_tx_input_maturity: spend_height - coinbase_height >= COINBASE_MATURITY
    Chain c;
    auto miner = key("v019-miner");
    auto k = key("v019-cb-payer");
    c.connect({});
    auto dep = matured_deposit(c, k, 10000, 2);
    // a block with a fee pays its coinbase 300
    auto pay = spend({dep}, {value_out(addr(k), 9700)}, {k});
    Coinbase cb;
    cb.outputs.push_back(value_out(addr(miner), 300));
    c.connect({pay}, {}, cb);
    const uint32_t cb_height = c.height();
    auto cbu = *c.find_utxo(addr(miner));
    CHECK(cbu.first.kind == OutPoint::Coinbase);
    {
        Txn t(c.store);
        CHECK(c.st.coinbase_height(t, cbu.first.txid) == std::optional<uint32_t>(cb_height));
    }
    c.empty_blocks(int(COINBASE_MATURITY) - 2); // next block at cb_height + 49
    auto use = spend({cbu}, {value_out(addr(miner), 300)}, {miner});
    CHECK(rejects([&] { c.connect({use}); }, "ImmatureOutPoint: coinbase"));
    c.connect({});
    CHECK_EQ(c.height() + 1 - cb_height, COINBASE_MATURITY);
    c.connect({use});
    // a block's coinbase height leaves with its header (disconnect_tip_header)
    Hash32 last_cb = c.blocks.back().header.coinbase_txid();
    {
        Txn t(c.store);
        CHECK(c.st.coinbase_height(t, last_cb).has_value());
    }
    c.disconnect();
    Txn t(c.store);
    CHECK(!c.st.coinbase_height(t, last_cb).has_value());
    CHECK(c.st.coinbase_height(t, cbu.first.txid).has_value());
}

TEST(v019_deposit_maturity_and_heights)
{
    // a deposit is spendable once prev_main_height - its mainchain height >= DEPOSIT_MATURITY
    Chain c;
    auto k = key("v019-dep");
    c.connect({});
    const uint32_t dep_main_height = c.main_height;
    c.connect({}, {deposit_event(addr(k), 7000, 3)});
    auto dep = *c.find_utxo(addr(k));
    {
        Txn t(c.store);
        CHECK(c.st.deposit_main_height(t, dep.first.txid) == std::optional<uint32_t>(dep_main_height));
        CHECK(!c.st.input_mature(t, dep.first, 0, dep_main_height + DEPOSIT_MATURITY - 1));
        CHECK(c.st.input_mature(t, dep.first, 0, dep_main_height + DEPOSIT_MATURITY));
    }
    auto use = spend({dep}, {value_out(addr(k), 7000)}, {k});
    c.empty_blocks(int(DEPOSIT_MATURITY) - 2); // the next block's prev_main is 49 above the deposit
    CHECK(rejects([&] { c.connect({use}); }, "ImmatureOutPoint: deposit"));
    c.connect({});
    c.connect({use});
    // the deposit's height goes with the 2WPD that brought it
    Chain d;
    d.connect({});
    d.connect({}, {deposit_event(addr(k), 7000, 3)});
    d.disconnect();
    Txn t(d.store);
    CHECK(!d.st.deposit_main_height(t, dep.first.txid).has_value());
    CHECK(!d.st.utxo(t, dep.first).has_value());
}

TEST(v019_bundle_built_after_txs_and_dropped_when_not_submitted)
{
    Chain c;
    auto k = key("v019-wd");
    c.connect({});
    auto dep = matured_deposit(c, k, 20000, 4);
    const uint64_t wealth = c.wealth();
    CHECK_EQ(wealth, uint64_t{20000});
    auto wd = spend({dep}, {withdrawal_out(addr(k), 15000, 1000, P2WPKH), value_out(addr(k), 4000)}, {k});
    c.connect({wd});
    const uint32_t built = c.height();
    OutPoint wop{OutPoint::Regular, wd.transaction.txid(), 0};
    // built after the block's transactions, at its height: the withdrawal output is spent by the bundle already
    auto m6id = c.pending();
    CHECK(m6id.has_value());
    CHECK(!c.utxo(wop).has_value());
    auto s = c.stxo(wop);
    CHECK(s && s->inpoint.kind == InPoint::Withdrawal && s->inpoint.id == *m6id);
    CHECK_EQ(c.bundle(*m6id)->second.latest().height, built);
    CHECK_EQ(c.wealth(), wealth - 16000);
    // not submitted within WITHDRAWAL_BUNDLE_FAILURE_GAP blocks: dropped in the 2WPD of block built + 4
    c.empty_blocks(int(WITHDRAWAL_BUNDLE_FAILURE_GAP) - 1);
    CHECK(c.pending() == m6id);
    c.connect({});
    const uint32_t dropped = c.height();
    CHECK_EQ(dropped, built + WITHDRAWAL_BUNDLE_FAILURE_GAP);
    CHECK(!c.pending().has_value());
    CHECK(c.bundle(*m6id)->second.latest().value == BundleStatus::Dropped);
    OutPoint rec = OutPoint::unsuccessful_withdrawal(*m6id, dropped, 0);
    auto ro = c.utxo(rec);
    CHECK(ro && ro->content.kind == Content::Withdrawal && ro->content.value == 15000);
    CHECK(c.stxo(wop).has_value()); // the original stays spent by the bundle
    CHECK_EQ(c.wealth(), wealth);   // recreated outputs are an inflow
    {
        Txn t(c.store);
        auto dr = c.st.dropped_bundles(t);
        CHECK(dr && dr->latest().value == *m6id && dr->latest().height == dropped);
    }
    // undoing the drop's block: the pending bundle back, the recreated output gone
    c.disconnect();
    CHECK(c.pending() == m6id);
    CHECK(!c.utxo(rec).has_value());
    CHECK(c.bundle(*m6id)->second.latest().value == BundleStatus::Pending);
    c.connect({});
    CHECK(c.utxo(rec).has_value());
    // a transaction may spend the recreated withdrawal output (cancel it): withdrawal outputs are spendable in v0.19
    auto cancel = spend({{rec, *ro}}, {value_out(addr(k), 16000)}, {k});
    c.connect({cancel});
    CHECK(!c.utxo(rec).has_value());
    CHECK_EQ(c.wealth(), wealth);
    // the next bundle waits for the gap after the drop; nothing to withdraw now
    c.empty_blocks(int(WITHDRAWAL_BUNDLE_FAILURE_GAP));
    CHECK(!c.pending().has_value());
}

TEST(v019_recreated_outputs_rebundled_after_gap)
{
    Chain c;
    auto k = key("v019-rebundle");
    c.connect({});
    auto dep = matured_deposit(c, k, 9000, 5);
    auto wd = spend({dep}, {withdrawal_out(addr(k), 8000, 500, P2TR)}, {k});
    c.connect({wd});
    auto first = *c.pending();
    c.empty_blocks(int(WITHDRAWAL_BUNDLE_FAILURE_GAP));
    const uint32_t dropped = c.height();
    OutPoint rec = OutPoint::unsuccessful_withdrawal(first, dropped, 0);
    CHECK(c.utxo(rec).has_value());
    // creation is due again WITHDRAWAL_BUNDLE_FAILURE_GAP blocks after the drop: the recreated output is bundled
    c.empty_blocks(int(WITHDRAWAL_BUNDLE_FAILURE_GAP) - 1);
    CHECK(!c.pending().has_value());
    c.connect({});
    auto second = c.pending();
    CHECK(second.has_value() && *second != first); // the commitment names the height
    auto b = c.bundle(*second);
    CHECK(b && b->first.bundle.spend_utxos.count(rec) == 1);
    CHECK(!c.utxo(rec).has_value());
    // disconnecting the block of construction unspends it
    c.disconnect();
    CHECK(!c.pending().has_value());
    CHECK(c.utxo(rec).has_value());
    CHECK(!c.bundle(*second).has_value());
}

TEST(v019_submitted_failed_confirmed_events)
{
    Chain c;
    auto k = key("v019-events");
    c.connect({});
    auto dep = matured_deposit(c, k, 30000, 6);
    auto wd = spend({dep}, {withdrawal_out(addr(k), 10000, 1000, P2WPKH), value_out(addr(k), 19000)}, {k});
    c.connect({wd});
    auto m6id = *c.pending();
    OutPoint wop{OutPoint::Regular, wd.transaction.txid(), 0};
    // Submitted: the status only (the outputs were spent at construction)
    c.connect({}, {bundle_event(m6id, bt::mainchain::BundleStatus::Submitted)});
    CHECK(!c.pending().has_value());
    CHECK(c.bundle(m6id)->second.latest().value == BundleStatus::Submitted);
    CHECK(c.stxo(wop).has_value());
    // Failed: recreated at this height, the original stays spent
    c.connect({}, {bundle_event(m6id, bt::mainchain::BundleStatus::Failed)});
    const uint32_t failed = c.height();
    CHECK(c.bundle(m6id)->second.latest().value == BundleStatus::Failed);
    OutPoint rec = OutPoint::unsuccessful_withdrawal(m6id, failed, 0);
    CHECK(c.utxo(rec).has_value());
    CHECK(c.stxo(wop).has_value());
    CHECK_EQ(c.wealth(), uint64_t{30000});
    {
        Txn t(c.store);
        auto lf = c.st.latest_failed_bundle(t);
        CHECK(lf && lf->first == failed && lf->second == m6id);
    }
    c.disconnect(); // Failed undone: the recreated output deleted
    CHECK(!c.utxo(rec).has_value());
    CHECK(c.bundle(m6id)->second.latest().value == BundleStatus::Submitted);
    // Confirmed instead: nothing in the UTXO set changes
    c.connect({}, {bundle_event(m6id, bt::mainchain::BundleStatus::Confirmed)});
    CHECK(c.bundle(m6id)->second.latest().value == BundleStatus::Confirmed);
    CHECK(c.stxo(wop).has_value());
    CHECK_EQ(c.wealth(), uint64_t{30000 - 11000});
    c.disconnect();
    CHECK(c.bundle(m6id)->second.latest().value == BundleStatus::Submitted);
}

TEST(v019_confirming_a_dropped_bundle_is_invalid)
{
    // two_way_peg_data.rs connect_withdrawal_bundle_confirmed: SubmittedUnexpected + Known -> error (v0.18 spent the
    // outputs again)
    Chain c;
    auto k = key("v019-unexpected");
    c.connect({});
    auto dep = matured_deposit(c, k, 9000, 7);
    c.connect({spend({dep}, {withdrawal_out(addr(k), 8000, 500, P2WPKH)}, {k})});
    auto m6id = *c.pending();
    c.empty_blocks(int(WITHDRAWAL_BUNDLE_FAILURE_GAP)); // dropped
    c.connect({}, {bundle_event(m6id, bt::mainchain::BundleStatus::Submitted)});
    CHECK(c.bundle(m6id)->second.latest().value == BundleStatus::SubmittedUnexpected);
    CHECK(rejects([&] { c.connect({}, {bundle_event(m6id, bt::mainchain::BundleStatus::Confirmed)}); }, "UnexpectedWithdrawalBundleConfirmed"));
}

TEST(v019_bundle_outputs_by_fee_density)
{
    // types/lib.rs Ord for AggregatedWithdrawal (ec56149): main_fee / script length first. A pays 1000 for a 22-byte
    // script (45.5/byte), B 1200 for a 34-byte one (35.3/byte): v0.19 puts A first, v0.18 (fee first) B.
    MemStore store;
    State st;
    auto k = key("v019-density");
    {
        Txn t(store);
        auto put = [&](const OutPoint& op, const Output& o) {
            Writer w;
            bincode(w, o);
            auto key = op.key();
            t.put(State::T_UTXOS, bt::ByteSpan(key.data(), op.key_len()), w.buf);
        };
        Hash32 a, b;
        a[0] = 1;
        b[0] = 2;
        put(OutPoint{OutPoint::Regular, a, 0}, withdrawal_out(addr(k), 5000, 1000, P2WPKH));
        put(OutPoint{OutPoint::Regular, b, 0}, withdrawal_out(addr(k), 5000, 1200, P2TR));
        t.commit();
    }
    Txn t(store);
    const Bytes spk_a = bt::btc::Address::parse(P2WPKH)->script_pubkey(), spk_b = bt::btc::Address::parse(P2TR)->script_pubkey();
    st.rules = Rules::V0_19;
    auto b19 = st.collect_withdrawal_bundle(t, 10);
    CHECK(b19 && b19->outputs.size() == 4);
    CHECK(b19->outputs[2].script_pubkey == spk_a && b19->outputs[3].script_pubkey == spk_b);
    st.rules = Rules::V0_18;
    auto b18 = st.collect_withdrawal_bundle(t, 10);
    CHECK(b18 && b18->outputs[2].script_pubkey == spk_b && b18->outputs[3].script_pubkey == spk_a);
}

TEST(v019_unsuccessful_withdrawal_outpoint_codecs)
{
    // transaction/outpoint.rs (ec56149): variant 3 { m6id, height, vout }; OutPointKey 41 bytes, zero-padded
    Hash32 m6id;
    for (int i = 0; i < 32; i++) m6id[i] = uint8_t(i);
    OutPoint op = OutPoint::unsuccessful_withdrawal(m6id, 0x01020304, 7);
    Bytes b = borsh_bytes(op);
    CHECK_EQ(b.size(), size_t{41});
    CHECK_EQ(b[0], uint8_t{3});
    CHECK_EQ(b[33], uint8_t{0x04}); // height first, little-endian
    CHECK_EQ(b[37], uint8_t{7});
    auto k = op.key();
    CHECK(Bytes(k.begin(), k.end()) == b);
    CHECK_EQ(op.key_len(), size_t{41});
    auto rk = OutPoint{OutPoint::Regular, m6id, 7}.key();
    CHECK(rk[37] == 0 && rk[40] == 0); // shorter variants zero-padded
    Reader r(b);
    CHECK(borsh_de_outpoint(r) == op);
    Writer w;
    bincode(w, op);
    Reader rb(w.buf);
    CHECK(de_outpoint(rb) == op);
    CHECK_EQ(w.buf.size(), size_t{4 + 8 + 32 + 4 + 4}); // u32 tag, serde bytes (u64 length) m6id, height, vout
    // Ord: after Deposit, then (m6id, height, vout)
    OutPoint dep{OutPoint::Deposit, m6id, 9};
    CHECK(dep < op);
    CHECK(OutPoint::unsuccessful_withdrawal(m6id, 1, 9) < OutPoint::unsuccessful_withdrawal(m6id, 2, 0));
    CHECK(OutPoint::unsuccessful_withdrawal(m6id, 2, 0) < OutPoint::unsuccessful_withdrawal(m6id, 2, 1));
}

TEST(v019_mainnet_magic_and_version)
{
    // lib/net/peer/message.rs (685dc52): Mainnet => 0b0000_0101
    auto m = net::magic_bytes(net::Network::Mainnet);
    CHECK_EQ(bt::hex_encode(bt::ByteSpan(m.data(), 4)), std::string("85189505"));
    auto b = net::magic_bytes(net::Network::Betanet);
    CHECK_EQ(bt::hex_encode(bt::ByteSpan(b.data(), 4)), std::string("85189504"));
    CHECK_EQ(net::network_version(net::Network::Mainnet).minor, uint64_t{19});
    CHECK_EQ(net::network_version(net::Network::Betanet).minor, uint64_t{18});
    CHECK_EQ(std::string(net::network_name(net::Network::Mainnet)), std::string("mainnet"));
}
