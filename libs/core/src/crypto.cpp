// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/crypto.h>

#include <algorithm>

extern "C" {
#include "blake3.h"
}

namespace bt {

// ---------------------------------------------------------------- BLAKE3
struct Blake3::Impl {
    blake3_hasher h;
};
Blake3::Blake3() : m(new Impl) { blake3_hasher_init(&m->h); }
Blake3::Blake3(const ByteArray<32>& key) : m(new Impl) { blake3_hasher_init_keyed(&m->h, key.data()); }
Blake3::~Blake3() { delete m; }
Blake3::Blake3(const Blake3& o) : m(new Impl(*o.m)) {}
Blake3& Blake3::operator=(const Blake3& o)
{
    *m = *o.m;
    return *this;
}
Blake3& Blake3::update(ByteSpan s)
{
    blake3_hasher_update(&m->h, s.data(), s.size());
    return *this;
}
Hash32 Blake3::finalize() const
{
    Hash32 r;
    blake3_hasher_finalize(&m->h, r.data(), 32);
    return r;
}
void Blake3::finalize_xof(uint8_t* out, size_t n) const { blake3_hasher_finalize(&m->h, out, n); }

Hash32 blake3(ByteSpan s) { return Blake3().update(s).finalize(); }

// ---------------------------------------------------------------- SHA-256
namespace {
inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t ror64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256_block(uint32_t s[8], const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
    0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
    0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
    0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

void sha512_block(uint64_t s[8], const uint8_t* p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t x = 0;
        for (int j = 0; j < 8; j++) x = x << 8 | p[8 * i + j];
        w[i] = x;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
        uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}

struct Sha256Ctx {
    uint32_t s[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    size_t n = 0;
    uint64_t total = 0;
    void update(const uint8_t* p, size_t len)
    {
        total += len;
        while (len) {
            size_t k = std::min(len, 64 - n);
            std::memcpy(buf + n, p, k);
            n += k; p += k; len -= k;
            if (n == 64) { sha256_block(s, buf); n = 0; }
        }
    }
    Hash32 final()
    {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (n != 56) update(&z, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; i++) l[i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 8);
        Hash32 r;
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) r[4 * i + j] = uint8_t(s[i] >> (24 - 8 * j));
        return r;
    }
};

struct Sha512Ctx {
    uint64_t s[8];
    uint8_t buf[128];
    size_t n = 0;
    uint64_t total = 0;
    explicit Sha512Ctx(const uint64_t iv[8]) { std::memcpy(s, iv, sizeof(s)); }
    void update(const uint8_t* p, size_t len)
    {
        total += len;
        while (len) {
            size_t k = std::min(len, 128 - n);
            std::memcpy(buf + n, p, k);
            n += k; p += k; len -= k;
            if (n == 128) { sha512_block(s, buf); n = 0; }
        }
    }
    void final(uint8_t out[64])
    {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (n != 112) update(&z, 1);
        uint8_t l[16] = {0};
        for (int i = 0; i < 8; i++) l[8 + i] = uint8_t(bits >> (56 - 8 * i));
        update(l, 16);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++) out[8 * i + j] = uint8_t(s[i] >> (56 - 8 * j));
    }
};

const uint64_t IV512[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                           0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
const uint64_t IV512_256[8] = {0x22312194fc2bf72cULL, 0x9f555fa3c84c64c2ULL, 0x2393b86b6f53b151ULL, 0x963877195940eabdULL,
                               0x96283ee2a88effe3ULL, 0xbe5e1e2553863992ULL, 0x2b0199fc2c85b8aaULL, 0x0eb72ddc81c52ca2ULL};
} // namespace

Hash32 sha256(ByteSpan s)
{
    Sha256Ctx c;
    c.update(s.data(), s.size());
    return c.final();
}
Hash32 sha256d(ByteSpan s)
{
    Hash32 a = sha256(s);
    return sha256(a.span());
}
ByteArray<64> sha512(ByteSpan s) { return sha512_parts({s}); }
ByteArray<64> sha512_parts(std::initializer_list<ByteSpan> parts)
{
    Sha512Ctx c(IV512);
    for (auto& p : parts) c.update(p.data(), p.size());
    ByteArray<64> r;
    c.final(r.data());
    return r;
}
Hash32 sha512_256(ByteSpan s)
{
    Sha512Ctx c(IV512_256);
    c.update(s.data(), s.size());
    uint8_t out[64];
    c.final(out);
    Hash32 r;
    std::memcpy(r.data(), out, 32);
    return r;
}

// ---------------------------------------------------------------- Base58
static const char* B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

std::string base58_encode(ByteSpan s)
{
    size_t zeros = 0;
    while (zeros < s.size() && s[zeros] == 0) zeros++;
    std::vector<uint8_t> b58((s.size() - zeros) * 138 / 100 + 1);
    size_t len = 0;
    for (size_t i = zeros; i < s.size(); i++) {
        int carry = s[i];
        size_t j = 0;
        for (auto it = b58.rbegin(); (carry != 0 || j < len) && it != b58.rend(); ++it, ++j) {
            carry += 256 * (*it);
            *it = uint8_t(carry % 58);
            carry /= 58;
        }
        len = j;
    }
    auto it = b58.begin() + (b58.size() - len);
    while (it != b58.end() && *it == 0) ++it;
    std::string r(zeros, '1');
    for (; it != b58.end(); ++it) r.push_back(B58[*it]);
    return r;
}

std::optional<Bytes> base58_decode(std::string_view s)
{
    static int8_t map[256];
    static bool init = false;
    if (!init) {
        std::fill(std::begin(map), std::end(map), -1);
        for (int i = 0; i < 58; i++) map[(uint8_t)B58[i]] = int8_t(i);
        init = true;
    }
    size_t zeros = 0;
    while (zeros < s.size() && s[zeros] == '1') zeros++;
    std::vector<uint8_t> b256(s.size() * 733 / 1000 + 1);
    size_t len = 0;
    for (size_t i = zeros; i < s.size(); i++) {
        int carry = map[(uint8_t)s[i]];
        if (carry < 0) return std::nullopt;
        size_t j = 0;
        for (auto it = b256.rbegin(); (carry != 0 || j < len) && it != b256.rend(); ++it, ++j) {
            carry += 58 * (*it);
            *it = uint8_t(carry % 256);
            carry /= 256;
        }
        len = j;
    }
    auto it = b256.begin() + (b256.size() - len);
    while (it != b256.end() && *it == 0) ++it;
    Bytes r(zeros, 0);
    r.insert(r.end(), it, b256.end());
    return r;
}

std::string base58check_encode(ByteSpan s)
{
    Bytes t(s.begin(), s.end());
    Hash32 h = sha256d(s);
    t.insert(t.end(), h.begin(), h.begin() + 4);
    return base58_encode(t);
}

std::optional<Bytes> base58check_decode(std::string_view s)
{
    auto d = base58_decode(s);
    if (!d || d->size() < 4) return std::nullopt;
    Bytes payload(d->begin(), d->end() - 4);
    Hash32 h = sha256d(payload);
    if (!std::equal(h.begin(), h.begin() + 4, d->end() - 4)) return std::nullopt;
    return payload;
}

} // namespace bt
