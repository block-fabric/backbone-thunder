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
