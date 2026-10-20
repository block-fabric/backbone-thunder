// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Single-signer Schnorr over ristretto255 exactly as frost-ristretto255 3.0
// (ZcashFoundation frost rev 0966bd15) verifies it, on top of libsodium's ristretto255.
//
//   challenge c = SHA-512("FROST-RISTRETTO255-SHA512-v1" || "chal" || R || VK || msg) mod L
//   valid iff  z*B == R + c*VK
//
// Encoding rules reproduced from frost-core/frost-ristretto255:
//  * VK and R must be canonical ristretto255 encodings and must NOT be the identity;
//  * z must be a canonical scalar (< L).
// Batch verification in their code is probabilistically equivalent to this per-item check.
#pragma once

#include <bt/bytes.h>

namespace bt::frost {

using Point = ByteArray<32>;
using Scalar = ByteArray<32>;
struct Signature {
    Point R;
    Scalar z;
};

/** Initialise libsodium (idempotent). */
void init();

/** Canonical, non-identity ristretto255 encoding (frost Group::deserialize). */
bool valid_element(const Point& p);
/** Canonical scalar (curve25519-dalek Scalar::from_canonical_bytes). */
bool canonical_scalar(const Scalar& s);

/** frost-ristretto255 VerifyingKey::verify / batch Item + verify_single. */
bool verify(const Point& vk, const Signature& sig, ByteSpan msg);

/** Signing key = non-zero scalar. */
struct SigningKey {
    Scalar s;
    Point verifying_key() const;
    /** default_sign with a random nonce (ours: nonce from OS RNG). */
    Signature sign(ByteSpan msg) const;
};

/** Scalar::from_bytes_mod_order_wide. */
Scalar reduce_wide(const ByteArray<64>& wide);

} // namespace bt::frost
