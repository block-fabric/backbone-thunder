// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Non-throwing parsers for numbers and host:port strings from untrusted input
// (CLI flags, RPC params, peer addresses, persisted records, HTTP headers).
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace bt {

/** Whole-string unsigned decimal; no sign, no whitespace, no overflow. */
inline std::optional<uint64_t> parse_u64(std::string_view s)
{
    if (s.empty() || s.size() > 20) return std::nullopt;
    uint64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) return std::nullopt;
    return v;
}

/** Port 1..65535 (allow_zero: also 0, for binding an ephemeral port). */
inline std::optional<uint16_t> parse_port(std::string_view s, bool allow_zero = false)
{
    auto v = parse_u64(s);
    if (!v || *v > 65535 || (!allow_zero && *v == 0)) return std::nullopt;
    return uint16_t(*v);
}

/** "host:port", "[v6]:port"; without a port, default_port if given. */
inline std::optional<std::pair<std::string, uint16_t>> split_hostport(const std::string& s, std::optional<uint16_t> default_port = std::nullopt,
                                                                      bool allow_zero = false)
{
    std::string host, port;
    if (!s.empty() && s[0] == '[') {
        auto e = s.find(']');
        if (e == std::string::npos) return std::nullopt;
        host = s.substr(1, e - 1);
        if (e + 1 == s.size()) {
            if (!default_port) return std::nullopt;
            return std::make_pair(host, *default_port);
        }
        if (s[e + 1] != ':') return std::nullopt;
        port = s.substr(e + 2);
    } else {
        auto c = s.rfind(':');
        if (c == std::string::npos) {
            if (!default_port || s.empty()) return std::nullopt;
            return std::make_pair(s, *default_port);
        }
        host = s.substr(0, c);
        port = s.substr(c + 1);
    }
    if (host.empty()) return std::nullopt;
    auto p = parse_port(port, allow_zero);
    if (!p) return std::nullopt;
    return std::make_pair(host, *p);
}

} // namespace bt
