// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/bitcoin.h>
#include <bt/codec.h>
#include <bt/crypto.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bt::btc {

namespace {
const char* CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

uint32_t polymod(const std::vector<uint8_t>& v)
{
    uint32_t c = 1;
    for (uint8_t x : v) {
        uint8_t c0 = uint8_t(c >> 25);
        c = ((c & 0x1ffffff) << 5) ^ x;
        if (c0 & 1) c ^= 0x3b6a57b2;
        if (c0 & 2) c ^= 0x26508e6d;
        if (c0 & 4) c ^= 0x1ea119fa;
        if (c0 & 8) c ^= 0x3d4233dd;
        if (c0 & 16) c ^= 0x2a1462b3;
    }
    return c;
}
std::vector<uint8_t> hrp_expand(std::string_view hrp)
{
    std::vector<uint8_t> r;
    for (char c : hrp) r.push_back(uint8_t(c) >> 5);
    r.push_back(0);
    for (char c : hrp) r.push_back(uint8_t(c) & 31);
    return r;
}
constexpr uint32_t BECH32_CONST = 1, BECH32M_CONST = 0x2bc830a3;

// bech32 0.11 segwit::decode: returns (hrp lowercase, witness version, program bytes).
std::optional<std::tuple<std::string, uint8_t, Bytes>> segwit_decode(std::string_view s)
{
    if (s.size() > 90) return std::nullopt; // segwit::MAX_STRING_LENGTH
    // check_characters: ASCII, no mixed case, separator = last '1'
    bool lower = false, upper = false;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (u < 33 || u > 126) return std::nullopt;
        if (u >= 'a' && u <= 'z') lower = true;
        if (u >= 'A' && u <= 'Z') upper = true;
    }
    if (lower && upper) return std::nullopt;
    size_t sep = s.rfind('1');
    if (sep == std::string_view::npos || sep == 0) return std::nullopt;
    std::string hrp(s.substr(0, sep));
    if (hrp.size() > 83) return std::nullopt;
    for (auto& c : hrp) c = char(std::tolower((unsigned char)c));
    std::vector<uint8_t> data;
    for (char c : s.substr(sep + 1)) {
        char l = char(std::tolower((unsigned char)c));
        const char* p = std::strchr(CHARSET, l);
        if (!p || !l) return std::nullopt;
        data.push_back(uint8_t(p - CHARSET));
    }
    if (data.size() < 6) return std::nullopt; // checksum
    if (data.size() == 6) return std::nullopt; // NoData
    uint8_t ver = data[0];
    if (ver > 16) return std::nullopt;
    auto exp = hrp_expand(hrp);
    exp.insert(exp.end(), data.begin(), data.end());
    uint32_t want = ver == 0 ? BECH32_CONST : BECH32M_CONST;
    if (polymod(exp) != want) return std::nullopt;
    std::vector<uint8_t> fes(data.begin() + 1, data.end() - 6);
    // padding rules (BIP-173)
    if (!fes.empty()) {
        size_t pad = fes.size() * 5 % 8;
        if (pad > 4) return std::nullopt;
        uint8_t last = fes.back();
        if (last & ((1u << pad) - 1)) return std::nullopt;
    }
    Bytes prog;
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t f : fes) {
        acc = (acc << 5) | f;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            prog.push_back(uint8_t(acc >> bits));
        }
    }
    // witness program length
    if (ver == 0) {
        if (prog.size() != 20 && prog.size() != 32) return std::nullopt;
    } else if (prog.size() < 2 || prog.size() > 40) {
        return std::nullopt;
    }
    return std::make_tuple(hrp, ver, prog);
}

std::string segwit_encode(std::string_view hrp, uint8_t ver, ByteSpan prog)
{
    std::vector<uint8_t> d{ver};
    uint32_t acc = 0;
    int bits = 0;
    for (uint8_t b : prog) {
        acc = (acc << 8) | b;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            d.push_back(uint8_t((acc >> bits) & 31));
        }
    }
    if (bits) d.push_back(uint8_t((acc << (5 - bits)) & 31));
    auto exp = hrp_expand(hrp);
    exp.insert(exp.end(), d.begin(), d.end());
    for (int i = 0; i < 6; i++) exp.push_back(0);
    uint32_t mod = polymod(exp) ^ (ver == 0 ? BECH32_CONST : BECH32M_CONST);
    std::string r(hrp);
    r.push_back('1');
    for (uint8_t x : d) r.push_back(CHARSET[x]);
    for (int i = 0; i < 6; i++) r.push_back(CHARSET[(mod >> (5 * (5 - i))) & 31]);
    return r;
}
} // namespace

std::optional<Address> Address::parse(std::string_view s)
{
    if (auto sw = segwit_decode(s)) {
        auto& [hrp, ver, prog] = *sw;
        Address a;
        a.kind = Kind::Segwit;
        a.witver = ver;
        a.program = prog;
        if (hrp == "bc") a.hrp = Hrp::Mainnet;
        else if (hrp == "tb") a.hrp = Hrp::Testnets;
        else if (hrp == "bcrt") a.hrp = Hrp::Regtest;
        else return std::nullopt; // UnknownHrp: no fallback to base58
        return a;
    }
    if (s.size() > 50) return std::nullopt;
    auto d = base58check_decode(s);
    if (!d || d->size() != 21) return std::nullopt;
    Address a;
    a.program.assign(d->begin() + 1, d->end());
    switch ((*d)[0]) {
    case 0x00: a.kind = Kind::P2pkh; a.net = NetKind::Main; break;
    case 0x6f: a.kind = Kind::P2pkh; a.net = NetKind::Test; break;
    case 0x05: a.kind = Kind::P2sh; a.net = NetKind::Main; break;
    case 0xc4: a.kind = Kind::P2sh; a.net = NetKind::Test; break;
    default: return std::nullopt;
    }
    return a;
}

std::string Address::to_string() const
{
    switch (kind) {
    case Kind::P2pkh:
    case Kind::P2sh: {
        Bytes b;
        uint8_t pfx = kind == Kind::P2pkh ? (net == NetKind::Main ? 0x00 : 0x6f) : (net == NetKind::Main ? 0x05 : 0xc4);
        b.push_back(pfx);
        append(b, program);
        return base58check_encode(b);
    }
    case Kind::Segwit:
        return segwit_encode(hrp == Hrp::Mainnet ? "bc" : hrp == Hrp::Testnets ? "tb" : "bcrt", witver, program);
    }
    return {};
}

Bytes Address::script_pubkey() const
{
    Bytes s;
    switch (kind) {
    case Kind::P2pkh:
        s = {0x76, 0xa9, 0x14};
        append(s, program);
        s.push_back(0x88);
        s.push_back(0xac);
        break;
    case Kind::P2sh:
        s = {0xa9, 0x14};
        append(s, program);
        s.push_back(0x87);
        break;
    case Kind::Segwit:
        s.push_back(witver == 0 ? 0x00 : uint8_t(0x50 + witver));
        s.push_back(uint8_t(program.size()));
        append(s, program);
        break;
    }
    return s;
}

std::strong_ordering operator<=>(const Address& a, const Address& b)
{
    if (auto c = a.kind <=> b.kind; c != 0) return c;
    if (a.kind == Address::Kind::Segwit) {
        if (auto c = a.witver <=> b.witver; c != 0) return c;
        if (auto c = std::lexicographical_compare_three_way(a.program.begin(), a.program.end(), b.program.begin(), b.program.end()); c != 0) return c;
        return a.hrp <=> b.hrp;
    }
    if (auto c = std::lexicographical_compare_three_way(a.program.begin(), a.program.end(), b.program.begin(), b.program.end()); c != 0) return c;
    return a.net <=> b.net;
}

size_t compact_size_len(uint64_t n) { return n < 253 ? 1 : n <= 0xffff ? 3 : n <= 0xffffffff ? 5 : 9; }

void write_compact_size(Bytes& out, uint64_t n)
{
    Writer w;
    if (n < 253) w.u8(uint8_t(n));
    else if (n <= 0xffff) { w.u8(253); w.u16(uint16_t(n)); }
    else if (n <= 0xffffffff) { w.u8(254); w.u32(uint32_t(n)); }
    else { w.u8(255); w.u64(n); }
    append(out, w.buf);
}

Bytes serialize_inputless_tx(int32_t version, const std::vector<TxOut>& outs, uint32_t locktime)
{
    Writer w;
    w.u32(uint32_t(version));
    write_compact_size(w.buf, 0);
    write_compact_size(w.buf, outs.size());
    for (auto& o : outs) {
        w.u64(o.value);
        write_compact_size(w.buf, o.script_pubkey.size());
        w.raw(o.script_pubkey);
    }
    w.u32(locktime);
    return w.buf;
}

uint64_t inputless_tx_weight(const std::vector<TxOut>& outs)
{
    uint64_t base = 4 + compact_size_len(0) + compact_size_len(outs.size()) + 4;
    for (auto& o : outs) base += 8 + compact_size_len(o.script_pubkey.size()) + o.script_pubkey.size();
    uint64_t total = base + 2;
    return base * 3 + total;
}

Bytes op_return_push(ByteSpan data)
{
    Bytes s{0x6a};
    size_t n = data.size();
    if (n < 0x4c) s.push_back(uint8_t(n));
    else if (n <= 0xff) { s.push_back(0x4c); s.push_back(uint8_t(n)); }
    else if (n <= 0xffff) { s.push_back(0x4d); s.push_back(uint8_t(n)); s.push_back(uint8_t(n >> 8)); }
    else { s.push_back(0x4e); for (int i = 0; i < 4; i++) s.push_back(uint8_t(n >> (8 * i))); }
    append(s, data);
    return s;
}

static uint64_t read_compact(Reader& r)
{
    uint8_t c = r.u8();
    if (c < 253) return c;
    if (c == 253) return r.u16();
    if (c == 254) return r.u32();
    return r.u64();
}

std::optional<ParsedTx> parse_tx(ByteSpan raw)
{
    try {
        Reader r(raw);
        ParsedTx t;
        t.version = int32_t(r.u32());
        uint64_t nin = read_compact(r);
        bool segwit = false;
        if (nin == 0) {
            // BIP144 marker/flag
            uint8_t flag = r.u8();
            if (flag != 1) return std::nullopt;
            segwit = true;
            nin = read_compact(r);
        }
        if (nin > r.remaining() / 41) return std::nullopt;
        for (uint64_t i = 0; i < nin; i++) {
            r.need(36);
            uint64_t sl = read_compact(r);
            r.need(size_t(sl));
            r.need(4);
        }
        t.n_inputs = size_t(nin);
        uint64_t nout = read_compact(r);
        if (nout > r.remaining() / 9) return std::nullopt;
        for (uint64_t i = 0; i < nout; i++) {
            TxOut o;
            o.value = r.u64();
            uint64_t sl = read_compact(r);
            o.script_pubkey = r.raw(size_t(sl));
            t.outputs.push_back(std::move(o));
        }
        if (segwit) {
            for (uint64_t i = 0; i < nin; i++) {
                uint64_t items = read_compact(r);
                for (uint64_t j = 0; j < items; j++) r.need(size_t(read_compact(r)));
            }
        }
        t.locktime = r.u32();
        if (!r.empty()) return std::nullopt;
        return t;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

} // namespace bt::btc
