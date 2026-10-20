// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/crypto.h>
#include <bt/frost.h>

#include <mutex>
#include <sodium.h>
#include <stdexcept>

namespace bt::frost {

static const char CONTEXT[] = "FROST-RISTRETTO255-SHA512-v1";

void init()
{
    static std::once_flag f;
    std::call_once(f, [] {
        if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    });
}

bool valid_element(const Point& p)
{
    init();
    if (p.is_zero()) return false; // the identity's only canonical encoding is all-zero
    return crypto_core_ristretto255_is_valid_point(p.data()) == 1;
}

// L = 2^252 + 27742317777372353535851937790883648493, little-endian
static const uint8_t L_BYTES[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                    0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

bool canonical_scalar(const Scalar& s)
{
    for (int i = 31; i >= 0; i--) {
        if (s[i] < L_BYTES[i]) return true;
        if (s[i] > L_BYTES[i]) return false;
    }
    return false; // == L
}

Scalar reduce_wide(const ByteArray<64>& wide)
{
    init();
    Scalar r;
    crypto_core_ristretto255_scalar_reduce(r.data(), wide.data());
    return r;
}

static Scalar challenge(const Point& R, const Point& vk, ByteSpan msg)
{
    auto h = sha512_parts({ByteSpan((const uint8_t*)CONTEXT, sizeof(CONTEXT) - 1), ByteSpan((const uint8_t*)"chal", 4),
                           R.span(), vk.span(), msg});
    return reduce_wide(h);
}

// q = n*p (n canonical); identity encodes as all-zero (libsodium returns -1 but writes it).
static Point mul(const Scalar& n, const Point& p)
{
    Point q;
    crypto_scalarmult_ristretto255(q.data(), n.data(), p.data());
    return q;
}
static Point mul_base(const Scalar& n)
{
    Point q;
    crypto_scalarmult_ristretto255_base(q.data(), n.data());
    return q;
}

static bool is_identity(const Point& p) { return p.is_zero(); }

static Point add(const Point& a, const Point& b)
{
    // crypto_core_ristretto255_add rejects invalid encodings; the identity (zero) is valid.
    Point r;
    if (is_identity(a)) return b;
    if (is_identity(b)) return a;
    if (crypto_core_ristretto255_add(r.data(), a.data(), b.data()) != 0) throw std::runtime_error("ristretto add");
    return r;
}

bool verify(const Point& vk, const Signature& sig, ByteSpan msg)
{
    init();
    if (!valid_element(vk) || !valid_element(sig.R) || !canonical_scalar(sig.z)) return false;
    Scalar c = challenge(sig.R, vk, msg);
    Point zB = mul_base(sig.z);
    Point cA = mul(c, vk);
    Point rhs = add(sig.R, cA);
    // ristretto255 encodings are canonical, so point equality == encoding equality.
    return zB == rhs;
}

Point SigningKey::verifying_key() const { return mul_base(s); }

Signature SigningKey::sign(ByteSpan msg) const
{
    init();
    Point vk = verifying_key();
    Scalar k;
    do {
        crypto_core_ristretto255_scalar_random(k.data());
    } while (k.is_zero());
    Point R = mul_base(k);
    Scalar c = challenge(R, vk, msg);
    Scalar cs, z;
    crypto_core_ristretto255_scalar_mul(cs.data(), c.data(), s.data());
    crypto_core_ristretto255_scalar_add(z.data(), k.data(), cs.data());
    return Signature{R, z};
}

} // namespace bt::frost
