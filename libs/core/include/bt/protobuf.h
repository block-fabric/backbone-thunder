// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Minimal protobuf wire-format encoder/decoder: enough for the cusf.mainchain.v1
// messages we exchange, without a protoc/libprotobuf dependency.
#pragma once

#include <bt/bytes.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bt::pb {

class Encoder {
public:
    Bytes buf;
    void varint(uint64_t v);
    void tag(uint32_t field, uint8_t wire) { varint((uint64_t(field) << 3) | wire); }
    void u64(uint32_t field, uint64_t v) { tag(field, 0); varint(v); }
    void boolean(uint32_t field, bool v) { u64(field, v ? 1 : 0); }
    void bytes(uint32_t field, ByteSpan b) { tag(field, 2); varint(b.size()); buf.insert(buf.end(), b.begin(), b.end()); }
    void str(uint32_t field, std::string_view s) { bytes(field, ByteSpan((const uint8_t*)s.data(), s.size())); }
    void msg(uint32_t field, const Encoder& m) { bytes(field, m.buf); }
    // google.protobuf wrappers: a message whose field 1 holds the value
    void u32_value(uint32_t field, uint32_t v) { Encoder e; e.u64(1, v); msg(field, e); }
    void u64_value(uint32_t field, uint64_t v) { Encoder e; e.u64(1, v); msg(field, e); }
    void string_value(uint32_t field, std::string_view s) { Encoder e; e.str(1, s); msg(field, e); }
    void bytes_value(uint32_t field, ByteSpan b) { Encoder e; e.bytes(1, b); msg(field, e); }
    // cusf.common.v1 hex wrappers: { StringValue hex = 1 }
    void hex_msg(uint32_t field, const std::string& hex) { Encoder e; e.string_value(1, hex); msg(field, e); }
};

/** A decoded message: fields by number, each a list of raw values in order. */
class Message {
public:
    struct Value {
        uint8_t wire = 0;
        uint64_t v = 0; // varint / fixed
        Bytes b;        // length-delimited
    };
    static std::optional<Message> parse(ByteSpan b);
    bool has(uint32_t f) const { return fields.count(f) > 0; }
    const std::vector<Value>* all(uint32_t f) const;
    std::optional<uint64_t> u64(uint32_t f) const;
    std::optional<Bytes> bytes(uint32_t f) const;
    std::optional<std::string> str(uint32_t f) const;
    std::optional<Message> msg(uint32_t f) const;
    std::vector<Message> msgs(uint32_t f) const;
    // wrappers
    std::optional<uint64_t> u64_value(uint32_t f) const;
    std::optional<std::string> string_value(uint32_t f) const;
    std::optional<Bytes> bytes_value(uint32_t f) const;
    /** cusf.common.v1.{Hex,ConsensusHex,ReverseHex}: the hex string inside. */
    std::optional<std::string> hex_msg(uint32_t f) const;

private:
    std::map<uint32_t, std::vector<Value>> fields;
};

} // namespace bt::pb
