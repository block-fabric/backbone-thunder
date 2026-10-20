// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Byte containers and hex helpers shared by the whole engine.
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bt {

using Bytes = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;

/** Fixed-size byte array with lexicographic ordering (the order Rust derives for [u8; N]). */
template <size_t N>
struct ByteArray {
    std::array<uint8_t, N> v{};

    static constexpr size_t size() { return N; }
    uint8_t* data() { return v.data(); }
    const uint8_t* data() const { return v.data(); }
    uint8_t& operator[](size_t i) { return v[i]; }
    const uint8_t& operator[](size_t i) const { return v[i]; }
    auto begin() const { return v.begin(); }
    auto end() const { return v.end(); }
    ByteSpan span() const { return ByteSpan(v.data(), N); }
    bool is_zero() const
    {
        for (auto b : v)
            if (b) return false;
        return true;
    }
    static ByteArray from_span(ByteSpan s)
    {
        if (s.size() != N) throw std::runtime_error("ByteArray: wrong length");
        ByteArray r;
        std::memcpy(r.v.data(), s.data(), N);
        return r;
    }
    friend auto operator<=>(const ByteArray&, const ByteArray&) = default;
    friend bool operator==(const ByteArray&, const ByteArray&) = default;
};

using Hash32 = ByteArray<32>;
using Hash20 = ByteArray<20>;

std::string hex_encode(ByteSpan s);
template <size_t N>
std::string hex_encode(const ByteArray<N>& a) { return hex_encode(a.span()); }
/** Hex of the reversed bytes: the display order of Bitcoin txids/block hashes. */
std::string hex_encode_rev(ByteSpan s);

std::optional<Bytes> hex_decode(std::string_view s);
/** Parse exactly N bytes from hex (in the given order). */
template <size_t N>
std::optional<ByteArray<N>> hex_to_array(std::string_view s)
{
    auto b = hex_decode(s);
    if (!b || b->size() != N) return std::nullopt;
    return ByteArray<N>::from_span(*b);
}
/** Parse a display-order (reversed) Bitcoin hash. */
std::optional<Hash32> hash_from_rev_hex(std::string_view s);

inline void append(Bytes& out, ByteSpan s) { out.insert(out.end(), s.begin(), s.end()); }

} // namespace bt
