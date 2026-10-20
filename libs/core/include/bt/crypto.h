// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Hash functions used by the LayerTwo-family chains and the mainchain link.
#pragma once

#include <bt/bytes.h>

namespace bt {

/** Incremental BLAKE3 (vendored reference C implementation). */
class Blake3 {
public:
    Blake3();
    /** Keyed mode (blake3::Hasher::new_keyed). */
    explicit Blake3(const ByteArray<32>& key);
    ~Blake3();
    Blake3(const Blake3&);
    Blake3& operator=(const Blake3&);
    Blake3& update(ByteSpan s);
    Blake3& update(const void* p, size_t n) { return update(ByteSpan((const uint8_t*)p, n)); }
    Hash32 finalize() const;
    /** XOF output of arbitrary length from offset 0. */
    void finalize_xof(uint8_t* out, size_t n) const;

private:
    struct Impl;
    Impl* m;
};

Hash32 blake3(ByteSpan s);

/** SHA-256 (FIPS 180-4). */
Hash32 sha256(ByteSpan s);
/** Double SHA-256 (Bitcoin txid/blockhash, internal byte order). */
Hash32 sha256d(ByteSpan s);
/** SHA-512. */
ByteArray<64> sha512(ByteSpan s);
/** SHA-512 over several concatenated parts. */
ByteArray<64> sha512_parts(std::initializer_list<ByteSpan> parts);
/** SHA-512/256 (FIPS 180-4 §5.3.6.2: own IV, truncated), used by rustreexo parent hashes. */
Hash32 sha512_256(ByteSpan s);

/** Base58 without checksum (bitcoin::base58::encode/decode). */
std::string base58_encode(ByteSpan s);
std::optional<Bytes> base58_decode(std::string_view s);
/** Base58Check (4-byte double-SHA256 checksum). */
std::string base58check_encode(ByteSpan s);
std::optional<Bytes> base58check_decode(std::string_view s);

} // namespace bt
