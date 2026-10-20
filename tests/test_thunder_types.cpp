// Thunder codec/hash known answers from public eCash betanet data.
#include "test.h"

#include <thunder/types.h>

#include <bt/crypto.h>

using namespace thunder;

TEST(thunder_empty_merkle_root)
{
    Coinbase cb;
    auto r = compute_merkle_root(cb, {});
    REQUIRE(r.has_value());
    // empty-body root shared by betanet Thunder/BitNames/BitAssets (v0.18 rules, coinbase memo)
    CHECK_EQ(bt::hex_encode(*r), std::string("812c97082cde521a6128a777b45d9221259e8e3e33d6f84b7b496ef43ca6bdbb"));
}

TEST(thunder_betanet_genesis_and_block1)
{
    Header g;
    g.merkle_root = *bt::hex_to_array<32>("812c97082cde521a6128a777b45d9221259e8e3e33d6f84b7b496ef43ca6bdbb");
    // prev_main = betanet block 968,909
    g.prev_main_hash = *bt::hash_from_rev_hex("00000000000000039ead064d25bbb098555be80207c32ff493ec0fb0ec3f1113");
    CHECK_EQ(bt::hex_encode(g.hash()), std::string("8b52259afff825b7ccbbb35bb00de2a8cad00a7ced10ca6c58db14a21da1ec74"));
    Header b1 = g;
    b1.prev_side_hash = g.hash();
    b1.prev_main_hash = *bt::hash_from_rev_hex("0000000000000000ca123028c7f364b1991861777bcd6f529da94e5f588e3c24");
    CHECK_EQ(bt::hex_encode(b1.hash()), std::string("c5bd920a91d48a432f686bb7e21ed1c41777c063d1bf1ab07551c6cef7fbe14d"));
    // bincode round trip of a header
    Writer w;
    bincode(w, b1);
    Reader r(w.buf);
    Header back = de_header(r);
    CHECK(r.empty());
    CHECK(back == b1);
}

TEST(thunder_limits)
{
    CHECK_EQ(body_size_limit(0), uint64_t(8 * 1024 * 1024));
    CHECK_EQ(body_sigops_limit(0), uint64_t(42800));
    CHECK_EQ(body_sigops_limit(4319), uint64_t(42800));
    CHECK_EQ(body_sigops_limit(4320), uint64_t(44512)); // floor(42800*1.04)
    CHECK_EQ(body_size_limit(4320), uint64_t(8724152)); // floor(8388608*1.04)
    CHECK_EQ(body_size_limit(120 * 4320), uint64_t(8ull * 1024 * 1024 * 111));
    // from month 6 the consensus limit exceeds the 10 MiB P2P block cap (bug §5.3)
    CHECK(body_size_limit(6 * 4320) > 10 * 1024 * 1024);
    CHECK(body_size_limit(5 * 4320) < 10 * 1024 * 1024);
}

TEST(thunder_address_forms)
{
    auto a = Address::from_base58("3zTJGK3PZDUJjjoV7HtgRBJ1Q9y5");
    REQUIRE(a.has_value());
    CHECK_EQ(a->to_base58(), std::string("3zTJGK3PZDUJjjoV7HtgRBJ1Q9y5"));
    auto dep = a->format_for_deposit();
    CHECK(dep.rfind("s9_3zTJGK3PZDUJjjoV7HtgRBJ1Q9y5_", 0) == 0);
    CHECK_EQ(dep.size(), std::string("s9_3zTJGK3PZDUJjjoV7HtgRBJ1Q9y5_").size() + 6);
    // the deposit form is not a node address: it burns to ALL_ZEROS on deposit (bug §5.5)
    CHECK(!Address::from_base58(dep).has_value());
}
