// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/bytes.h>

#include <algorithm>

namespace bt {

static const char* HEXDIGITS = "0123456789abcdef";

std::string hex_encode(ByteSpan s)
{
    std::string r;
    r.reserve(s.size() * 2);
    for (uint8_t b : s) {
        r.push_back(HEXDIGITS[b >> 4]);
        r.push_back(HEXDIGITS[b & 15]);
    }
    return r;
}

std::string hex_encode_rev(ByteSpan s)
{
    Bytes t(s.begin(), s.end());
    std::reverse(t.begin(), t.end());
    return hex_encode(t);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<Bytes> hex_decode(std::string_view s)
{
    if (s.size() % 2) return std::nullopt;
    Bytes r;
    r.reserve(s.size() / 2);
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        r.push_back(uint8_t(a << 4 | b));
    }
    return r;
}

std::optional<Hash32> hash_from_rev_hex(std::string_view s)
{
    auto b = hex_decode(s);
    if (!b || b->size() != 32) return std::nullopt;
    std::reverse(b->begin(), b->end());
    return Hash32::from_span(*b);
}

} // namespace bt
