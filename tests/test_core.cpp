// Known-answer tests for the shared core: hashes, encodings, FROST verification, Utreexo.
#include "test.h"

#include <bt/bitcoin.h>
#include <bt/crypto.h>
#include <bt/frost.h>
#include <bt/utreexo.h>

#include <univalue.h>
#include <unistd.h>

using namespace bt;

static std::string h(ByteSpan s) { return hex_encode(s); }
static ByteSpan str(const char* s) { return ByteSpan((const uint8_t*)s, std::strlen(s)); }

TEST(sha_vectors)
{
    CHECK_EQ(h(sha256(str("abc")).span()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_EQ(h(sha256(str("")).span()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK_EQ(h(sha512(str("abc")).span()),
             "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK_EQ(h(sha512_256(str("abc")).span()), "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");
    // 2-block message
    std::string m(200, 'a');
    CHECK_EQ(h(sha256(str(m.c_str())).span()), "c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5");
}

TEST(blake3_vectors)
{
    CHECK_EQ(h(blake3(str("")).span()), "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    CHECK_EQ(h(blake3(str("abc")).span()), "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
}

TEST(base58_roundtrip)
{
    auto d = base58_decode("3zTJGK3PZDUJjjoV7HtgRBJ1Q9y5");
    REQUIRE(d.has_value());
    CHECK_EQ(d->size(), size_t(20));
    CHECK_EQ(base58_encode(*d), std::string("3zTJGK3PZDUJjjoV7HtgRBJ1Q9y5"));
    CHECK(!base58_decode("0OIl").has_value());
    auto z = base58_decode("111");
    REQUIRE(z.has_value());
    CHECK_EQ(z->size(), size_t(3));
}

TEST(bitcoin_addresses)
{
    auto a = btc::Address::parse("bc1qaw84063ptkynlelkj4l6jff2erhnru93kcdg96");
    REQUIRE(a.has_value());
    CHECK(a->kind == btc::Address::Kind::Segwit);
    CHECK_EQ(a->to_string(), std::string("bc1qaw84063ptkynlelkj4l6jff2erhnru93kcdg96"));
    CHECK_EQ(h(a->script_pubkey()), "0014eb8f57ea215d893fe7f6957fa9252ac8ef31f0b1");
    auto b = btc::Address::parse("1A7pUaYFaXHq1JTTJfLfgy2FwFcjNsVXMT");
    REQUIRE(b.has_value());
    CHECK(b->kind == btc::Address::Kind::P2pkh);
    CHECK_EQ(b->to_string(), std::string("1A7pUaYFaXHq1JTTJfLfgy2FwFcjNsVXMT"));
    // uppercase bech32 parses, prints lowercase
    auto c = btc::Address::parse("BC1QAW84063PTKYNLELKJ4L6JFF2ERHNRU93KCDG96");
    REQUIRE(c.has_value());
    CHECK(*c == *a);
    // taproot (bech32m), BIP-350 vector
    auto t = btc::Address::parse("bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0");
    REQUIRE(t.has_value());
    CHECK_EQ(h(t->script_pubkey()), "512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");
    // v1 with bech32 (not bech32m) checksum is invalid
    CHECK(!btc::Address::parse("bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7k7grplx").has_value());
    // unknown hrp: no fallback
    CHECK(!btc::Address::parse("ltc1qaw84063ptkynlelkj4l6jff2erhnru93kcdg96").has_value());
    // ordering: P2pkh < P2sh < Segwit
    CHECK(*b < *a);
}

TEST(frost_rfc9591_vector)
{
    // frost-ristretto255 tests/helpers/vectors.json (RFC 9591 Appendix E.4)
    auto vk = hex_to_array<32>("e2a62f39eede11269e3bd5a7d97554f5ca384f9f6d3dd9c3c0d05083c7254f57");
    auto sk = hex_to_array<32>("1b25a55e463cfd15cf14a5d3acc3d15053f08da49c8afcf3ab265f2ebc4f970b");
    auto sig = hex_decode("fa954853693068803615803a06e2c23a6228f7d6d6b442b72b26696aa776fe75532350f49b27a123b0c811d54671f6c008e319741a59918baf3c5455a5ec2603");
    REQUIRE(vk && sk && sig);
    frost::SigningKey k{*sk};
    CHECK(k.verifying_key() == *vk);
    frost::Signature s;
    std::memcpy(s.R.data(), sig->data(), 32);
    std::memcpy(s.z.data(), sig->data() + 32, 32);
    CHECK(frost::verify(*vk, s, str("test")));
    CHECK(!frost::verify(*vk, s, str("tesu")));
    // our own signature verifies
    auto s2 = k.sign(str("hello"));
    CHECK(frost::verify(*vk, s2, str("hello")));
    // non-canonical scalar is rejected (z + L)
    frost::Signature bad = s;
    bad.z[31] |= 0x10;
    CHECK(!frost::canonical_scalar(bad.z));
    CHECK(!frost::verify(*vk, bad, str("test")));
    // identity and invalid elements are rejected
    frost::Point zero{};
    CHECK(!frost::valid_element(zero));
    auto inv = hex_to_array<32>("abcdef7de8baf62d57fe0452581b147b152f776e830c346d1119cee0bc954a59");
    CHECK(!frost::valid_element(*inv));
}

static utreexo::NodeHash hash_from_u8(uint8_t v)
{
    return utreexo::NodeHash::some(sha256(ByteSpan(&v, 1)));
}
static utreexo::NodeHash parse_nh(const std::string& s)
{
    if (s == std::string(64, '0')) return utreexo::NodeHash::empty(); // as in their #[cfg(test)] parser
    return utreexo::NodeHash::some(*hex_to_array<32>(s));
}

TEST(utreexo_parent_hash_doc)
{
    auto p = utreexo::parent_hash(utreexo::NodeHash::some(Hash32{}), utreexo::NodeHash::some([] {
                                      Hash32 x;
                                      x.v.fill(1);
                                      return x;
                                  }()));
    CHECK_EQ(p.to_string(), std::string("34e33ca0c40b7bd33d28932ca9e35170def7309a3bf91ecda5e1ceb067548a12"));
}

TEST(utreexo_rustreexo_vectors)
{
    UniValue v;
    REQUIRE(v.read(bttest::read_file(bttest::data_dir() + "/rustreexo_test_cases.json")));
    // insertion tests
    for (auto& c : v["insertion_tests"].getValues()) {
        std::vector<utreexo::NodeHash> leaves;
        for (auto& x : c["leaf_preimages"].getValues()) leaves.push_back(hash_from_u8(uint8_t(x.getInt<int>())));
        utreexo::MemForest f;
        CHECK(!f.modify(leaves, {}).has_value());
        auto roots = f.get_roots();
        auto& exp = c["expected_roots"].getValues();
        REQUIRE(roots.size() == exp.size());
        for (size_t i = 0; i < roots.size(); i++) CHECK(roots[i] == parse_nh(exp[i].get_str()));
    }
    // deletion tests
    for (auto& c : v["deletion_tests"].getValues()) {
        std::vector<utreexo::NodeHash> leaves;
        for (auto& x : c["leaf_preimages"].getValues()) leaves.push_back(hash_from_u8(uint8_t(x.getInt<int>())));
        std::vector<utreexo::NodeHash> dels;
        for (auto& x : c["target_values"].getValues()) dels.push_back(leaves[size_t(x.getInt<int>())]);
        utreexo::MemForest f;
        CHECK(!f.modify(leaves, {}).has_value());
        CHECK(!f.modify({}, dels).has_value());
        auto roots = f.get_roots();
        auto& exp = c["expected_roots"].getValues();
        REQUIRE(roots.size() == exp.size());
        for (size_t i = 0; i < roots.size(); i++) CHECK(roots[i] == parse_nh(exp[i].get_str()));
        // serialization round trip preserves roots
        auto f2 = utreexo::MemForest::deserialize(f.serialize());
        REQUIRE(f2.has_value());
        CHECK(f2->get_roots() == roots);
        CHECK(f2->serialize() == f.serialize());
    }
    // proof tests
    for (auto& c : v["proof_tests"].getValues()) {
        utreexo::Proof p;
        for (auto& x : c["targets"].getValues()) p.targets.push_back(uint64_t(x.getInt<int64_t>()));
        for (auto& x : c["proofhashes"].getValues()) p.hashes.push_back(parse_nh(x.get_str()));
        std::vector<utreexo::NodeHash> roots, dels;
        for (auto& x : c["roots"].getValues()) roots.push_back(parse_nh(x.get_str()));
        for (auto& x : c["target_preimages"].getValues()) dels.push_back(hash_from_u8(uint8_t(x.getInt<int>())));
        auto r = p.verify(dels, roots, uint64_t(c["numleaves"].getInt<int64_t>()));
        bool expected = c["expected"].get_bool();
        CHECK(r.has_value() && *r == expected);
    }
}

TEST(utreexo_prove_verify_roundtrip)
{
    std::vector<utreexo::NodeHash> leaves;
    for (int i = 0; i < 37; i++) leaves.push_back(hash_from_u8(uint8_t(i)));
    utreexo::MemForest f;
    CHECK(!f.modify(leaves, {}).has_value());
    std::vector<utreexo::NodeHash> del = {leaves[3], leaves[17], leaves[36]};
    CHECK(!f.modify({}, del).has_value());
    for (int i : {0, 5, 20, 33}) {
        std::vector<utreexo::NodeHash> t = {leaves[i]};
        auto p = f.prove(t);
        REQUIRE(p.has_value());
        auto ok = f.verify(*p, t);
        CHECK(ok.has_value() && *ok);
    }
    auto p = f.prove({leaves[1], leaves[30]});
    REQUIRE(p.has_value());
    auto ok = f.verify(*p, {leaves[1], leaves[30]});
    CHECK(ok.has_value() && *ok);
    auto bad = f.verify(*p, {leaves[2], leaves[30]});
    CHECK(bad.has_value() && !*bad);
    // a deleted leaf cannot be proven
    CHECK(!f.prove({leaves[3]}).has_value());
}

#include <bt/kv.h>
#include <filesystem>

TEST(kv_sqlite_empty_keys_and_values)
{
    std::string path = std::filesystem::temp_directory_path().string() + "/bt_kv_test_" + std::to_string(::getpid()) + ".sqlite";
    {
        auto st = kv::open_sqlite(path);
        kv::Txn t(*st);
        t.put(1, Bytes{}, Bytes{});
        t.put(1, Bytes{1}, Bytes{2, 3});
        t.put(2, Bytes{9}, Bytes{});
        t.commit();
        kv::Txn r(*st);
        CHECK(r.get(1, Bytes{}).has_value());
        CHECK(r.get(1, Bytes{})->empty());
        CHECK(*r.get(1, Bytes{1}) == (Bytes{2, 3}));
        CHECK(r.get(2, Bytes{9}).has_value());
        CHECK(r.del(1, Bytes{}));
        r.commit();
        kv::Txn r2(*st);
        CHECK(!r2.get(1, Bytes{}).has_value());
        // LMDB-like memcmp order of little-endian u32 keys: 255 sorts after 256
        r2.put(3, Bytes{0xff, 0, 0, 0}, Bytes{1});
        r2.put(3, Bytes{0x00, 1, 0, 0}, Bytes{2});
        r2.commit();
        kv::Txn r3(*st);
        auto last = r3.last(3);
        CHECK(last && last->second == Bytes{1});
    }
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
