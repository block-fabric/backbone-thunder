// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/protobuf.h>

namespace bt::pb {

void Encoder::varint(uint64_t v)
{
    while (v >= 0x80) {
        buf.push_back(uint8_t(v | 0x80));
        v >>= 7;
    }
    buf.push_back(uint8_t(v));
}

static bool read_varint(const uint8_t*& p, const uint8_t* e, uint64_t& out)
{
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (p == e) return false;
        uint8_t b = *p++;
        out |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

std::optional<Message> Message::parse(ByteSpan b)
{
    Message m;
    const uint8_t* p = b.data();
    const uint8_t* e = p + b.size();
    while (p < e) {
        uint64_t key;
        if (!read_varint(p, e, key)) return std::nullopt;
        uint32_t field = uint32_t(key >> 3);
        uint8_t wire = uint8_t(key & 7);
        Value v;
        v.wire = wire;
        switch (wire) {
        case 0:
            if (!read_varint(p, e, v.v)) return std::nullopt;
            break;
        case 1:
            if (e - p < 8) return std::nullopt;
            for (int i = 7; i >= 0; i--) v.v = v.v << 8 | p[i];
            p += 8;
            break;
        case 2: {
            uint64_t n;
            if (!read_varint(p, e, n) || uint64_t(e - p) < n) return std::nullopt;
            v.b.assign(p, p + n);
            p += n;
            break;
        }
        case 5:
            if (e - p < 4) return std::nullopt;
            for (int i = 3; i >= 0; i--) v.v = v.v << 8 | p[i];
            p += 4;
            break;
        default: return std::nullopt;
        }
        m.fields[field].push_back(std::move(v));
    }
    return m;
}

const std::vector<Message::Value>* Message::all(uint32_t f) const
{
    auto it = fields.find(f);
    return it == fields.end() ? nullptr : &it->second;
}

// proto3: for singular fields the last occurrence wins
std::optional<uint64_t> Message::u64(uint32_t f) const
{
    auto a = all(f);
    if (!a || a->empty()) return std::nullopt;
    return a->back().v;
}
std::optional<Bytes> Message::bytes(uint32_t f) const
{
    auto a = all(f);
    if (!a || a->empty() || a->back().wire != 2) return std::nullopt;
    return a->back().b;
}
std::optional<std::string> Message::str(uint32_t f) const
{
    auto b = bytes(f);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}
std::optional<Message> Message::msg(uint32_t f) const
{
    auto b = bytes(f);
    if (!b) return std::nullopt;
    return parse(*b);
}
std::vector<Message> Message::msgs(uint32_t f) const
{
    std::vector<Message> r;
    if (auto a = all(f))
        for (auto& v : *a)
            if (v.wire == 2)
                if (auto m = parse(v.b)) r.push_back(std::move(*m));
    return r;
}
std::optional<uint64_t> Message::u64_value(uint32_t f) const
{
    auto m = msg(f);
    if (!m) return std::nullopt;
    return m->u64(1).value_or(0);
}
std::optional<std::string> Message::string_value(uint32_t f) const
{
    auto m = msg(f);
    if (!m) return std::nullopt;
    return m->str(1).value_or("");
}
std::optional<Bytes> Message::bytes_value(uint32_t f) const
{
    auto m = msg(f);
    if (!m) return std::nullopt;
    return m->bytes(1).value_or(Bytes{});
}
std::optional<std::string> Message::hex_msg(uint32_t f) const
{
    auto m = msg(f);
    if (!m) return std::nullopt;
    return m->string_value(1);
}

} // namespace bt::pb
