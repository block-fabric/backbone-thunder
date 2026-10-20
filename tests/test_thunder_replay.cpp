// End-to-end known answers against public eCash betanet data (slot 9).
#include "test.h"

#include <thunder/betanet_fixture.h>

#include <cstdio>

using namespace thunder;

static const fixture::Data& data()
{
    static std::optional<fixture::Data> d;
    if (!d) {
        std::string err;
        d = fixture::load(bttest::data_dir() + "/betanet", err);
        if (!d) std::fprintf(stderr, "fixture load: %s\n", err.c_str());
    }
    return *d;
}

TEST(betanet_tx_borsh_roundtrip)
{
    // every public tx: borsh decode -> re-encode identical -> blake3 == txid
    size_t n = 0;
    for (auto& [txid, hex] : data().txhex) {
        auto raw = bt::hex_decode(hex);
        REQUIRE(raw.has_value());
        bt::Reader r(*raw);
        Transaction t = borsh_de_transaction(r);
        CHECK(r.empty());
        CHECK(t.canonical_bytes() == *raw);
        CHECK_EQ(bt::hex_encode(t.txid()), txid);
        // bincode round trip of the same transaction
        bt::Writer w;
        bincode(w, t);
        bt::Reader br(w.buf);
        Transaction back = de_transaction(br);
        CHECK(br.empty());
        CHECK(back.canonical_bytes() == t.canonical_bytes());
        n++;
    }
    CHECK_EQ(n, size_t(34));
}

TEST(betanet_replay_index)
{
    bt::kv::MemStore store;
    State st;
    auto r = fixture::replay(data(), st, store);
    std::fprintf(stderr, "  replayed to %u, %u header hashes and %u merkle roots recomputed, %u BMM checks; %s\n", r.replayed_to,
                 r.verified_hashes, r.verified_merkle_roots, r.bmm_verified, r.error.c_str());
    for (size_t i = 0; i < r.notes.size() && i < 5; i++) std::fprintf(stderr, "  note: %s\n", r.notes[i].c_str());
    CHECK(r.verified_hashes > 0);
}

TEST(betanet_replay_full_blocks)
{
    auto blocks = fixture::load_blocks_bin(bttest::data_dir() + "/betanet/blocks.bin");
    REQUIRE(blocks.size() == 2243);
    bt::kv::MemStore store;
    State st;
    auto r = fixture::replay_full(data(), blocks, st, store);
    std::fprintf(stderr, "  %u blocks connected (all checks), %u byte-exact re-encodings, %u hashes = index, %u FROST sigs, %u utreexo proofs, %u BMM, %u deposits; %s\n",
                 r.blocks, r.reencoded_exact, r.hashes_match_index, r.signatures, r.proofs, r.bmm_verified, r.deposits, r.error.c_str());
    CHECK(r.error.empty());
    CHECK_EQ(r.blocks, 2243u);
    CHECK_EQ(r.reencoded_exact, 2243u);
    CHECK_EQ(r.bmm_verified, 2243u);
    CHECK_EQ(r.deposits, 26u);
    bt::kv::Txn t(store);
    CHECK_EQ(*st.height(t), 2242u);
    CHECK_EQ(bt::hex_encode(*st.tip(t)), bt::hex_encode(data().blocks[2242].id));
    uint64_t wealth = st.sidechain_wealth(t);
    std::fprintf(stderr, "  sidechain_wealth %llu sats, utxos %zu, stxos %zu\n", (unsigned long long)wealth, st.utxos(t).size(), st.stxos(t).size());
    // the bundle the enforcer saw Submitted at 970,438 is the one we built
    auto b = st.bundle(t, *bt::hex_to_array<32>("aa769fec72ddc7e59fb6dc480d62ba19236804598d053b6c50522cae30b0ced4"));
    REQUIRE(b.has_value());
    CHECK(b->first.kind == WithdrawalBundleInfo::Known);
    CHECK(b->second.latest().value == BundleStatus::Submitted);
    for (auto& [m6id, info] : st.bundles(t))
        std::fprintf(stderr, "  bundle %s %s: %s @%u\n", bt::hex_encode(m6id).c_str(), info.first.kind == WithdrawalBundleInfo::Known ? "known" : "unknown",
                     to_string(info.second.latest().value), info.second.latest().height);
    if (auto p = st.pending_bundle_m6id(t)) std::fprintf(stderr, "  pending m6id %s\n", bt::hex_encode(*p).c_str());
}

TEST(betanet_full_rollback_to_genesis)
{
    // Disconnect the whole betanet history (block + its 2WPD window, newest first, as
    // net_task.rs disconnect_tip_ does) and check that every table returns to empty;
    // then reconnect and check the tip and state again.
    auto blocks = fixture::load_blocks_bin(bttest::data_dir() + "/betanet/blocks.bin");
    bt::kv::MemStore store;
    State st;
    std::vector<fixture::ReplayStep> steps;
    auto r = fixture::replay_full(data(), blocks, st, store, &steps);
    REQUIRE(r.error.empty());
    uint64_t wealth;
    size_t nutxo;
    {
        bt::kv::Txn t(store);
        wealth = st.sidechain_wealth(t);
        nutxo = st.utxos(t).size();
    }
    for (size_t i = steps.size(); i-- > 0;) {
        bt::kv::Txn t(store);
        st.disconnect_2wpd(t, steps[i].window);
        st.disconnect_tip(t, steps[i].block.header, steps[i].block.body);
        if (i > 0) st.put_accumulator(t, *bt::utreexo::MemForest::deserialize(steps[i - 1].accumulator));
        else t.del(State::T_ACCUMULATOR, bt::Bytes{});
        t.commit();
    }
    bt::kv::Txn t(store);
    CHECK(!st.tip(t).has_value());
    CHECK(st.utxos(t).empty());
    CHECK(st.stxos(t).empty());
    CHECK(st.bundles(t).empty());
    CHECK(!st.pending_bundle_m6id(t).has_value());
    CHECK(st.deposit_blocks(t).empty());
    CHECK(st.bundle_event_blocks(t).empty());
    // reconnect everything
    State st2;
    auto r2 = fixture::replay_full(data(), blocks, st2, store);
    CHECK(r2.error.empty());
    bt::kv::Txn t2(store);
    CHECK_EQ(st2.sidechain_wealth(t2), wealth);
    CHECK_EQ(st2.utxos(t2).size(), nutxo);
}
