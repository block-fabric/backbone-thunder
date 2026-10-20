// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Byte-exact encoders for the two serializations the LayerTwo-family chains use:
//  * borsh 1.x  - canonical form for every hash and size (u32 lengths, u8 enum tags);
//  * bincode 1.3 legacy config over serde (little-endian fixint, u64 lengths, u32 enum tags),
//    used on the P2P wire and in their databases.
#pragma once

#include <bt/bytes.h>

#include <stdexcept>
#include <string>

namespace bt {

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/** Little-endian writer with both borsh and bincode length/tag conventions. */
class Writer {
public:
    Bytes buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; i++) buf.push_back(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) buf.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) buf.push_back(uint8_t(v >> (8 * i))); }
    void u32be(uint32_t v) { for (int i = 3; i >= 0; i--) buf.push_back(uint8_t(v >> (8 * i))); }
    void u64be(uint64_t v) { for (int i = 7; i >= 0; i--) buf.push_back(uint8_t(v >> (8 * i))); }
    void raw(ByteSpan s) { buf.insert(buf.end(), s.begin(), s.end()); }
    template <size_t N>
    void raw(const ByteArray<N>& a) { raw(a.span()); }
    // borsh: Vec<u8> / String = u32 length + bytes
    void borsh_bytes(ByteSpan s) { u32(uint32_t(s.size())); raw(s); }
    void borsh_len(size_t n) { u32(uint32_t(n)); }
    // bincode: u64 length + bytes
    void bincode_bytes(ByteSpan s) { u64(s.size()); raw(s); }
    void bincode_len(size_t n) { u64(n); }
    void bincode_str(std::string_view s) { u64(s.size()); raw(ByteSpan((const uint8_t*)s.data(), s.size())); }
};

class Reader {
public:
    explicit Reader(ByteSpan s) : p(s.data()), end(s.data() + s.size()) {}
    size_t remaining() const { return size_t(end - p); }
    bool empty() const { return p == end; }
    const uint8_t* need(size_t n)
    {
        if (remaining() < n) throw DecodeError("unexpected end of input");
        const uint8_t* r = p;
        p += n;
        return r;
    }
    uint8_t u8() { return *need(1); }
    uint16_t u16() { auto q = need(2); return uint16_t(q[0] | q[1] << 8); }
    uint32_t u32()
    {
        auto q = need(4);
        return uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24;
    }
    uint64_t u64()
    {
        auto q = need(8);
        uint64_t v = 0;
        for (int i = 7; i >= 0; i--) v = v << 8 | q[i];
        return v;
    }
    Bytes raw(size_t n) { auto q = need(n); return Bytes(q, q + n); }
    template <size_t N>
    ByteArray<N> array() { auto q = need(N); ByteArray<N> a; std::memcpy(a.data(), q, N); return a; }
    /** bool as in serde/bincode and borsh: 0 or 1, anything else is an error. */
    bool boolean()
    {
        uint8_t b = u8();
        if (b > 1) throw DecodeError("invalid bool");
        return b == 1;
    }
    /** bincode sequence length, sanity-bounded by the remaining input (each element >= min_elem bytes). */
    size_t bincode_len(size_t min_elem = 1)
    {
        uint64_t n = u64();
        if (min_elem && n > remaining() / min_elem) throw DecodeError("length exceeds input");
        return size_t(n);
    }
    Bytes bincode_bytes() { size_t n = bincode_len(1); return raw(n); }
    std::string bincode_str()
    {
        Bytes b = bincode_bytes();
        std::string s(b.begin(), b.end());
        if (!utf8_valid(s)) throw DecodeError("invalid utf-8");
        return s;
    }
    size_t borsh_len(size_t min_elem = 1)
    {
        uint32_t n = u32();
        if (min_elem && n > remaining() / min_elem) throw DecodeError("length exceeds input");
        return n;
    }
    Bytes borsh_bytes() { size_t n = borsh_len(1); return raw(n); }

    static bool utf8_valid(std::string_view s);

private:
    const uint8_t* p;
    const uint8_t* end;
};

} // namespace bt
