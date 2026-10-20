// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bt/codec.h>

namespace bt {

// Strict UTF-8 validation with the same accept set as Rust's core::str::from_utf8:
// no overlong forms, no surrogates (U+D800..DFFF), nothing above U+10FFFF.
bool Reader::utf8_valid(std::string_view s)
{
    const auto* p = (const unsigned char*)s.data();
    const auto* e = p + s.size();
    while (p < e) {
        unsigned c = *p;
        if (c < 0x80) { p++; continue; }
        int n;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) n = 1;
        else if (c == 0xE0) { n = 2; lo = 0xA0; }
        else if (c >= 0xE1 && c <= 0xEC) n = 2;
        else if (c == 0xED) { n = 2; hi = 0x9F; }
        else if (c >= 0xEE && c <= 0xEF) n = 2;
        else if (c == 0xF0) { n = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) n = 3;
        else if (c == 0xF4) { n = 3; hi = 0x8F; }
        else return false;
        if (e - p <= n) return false;
        if (p[1] < lo || p[1] > hi) return false;
        for (int i = 2; i <= n; i++)
            if (p[i] < 0x80 || p[i] > 0xBF) return false;
        p += n + 1;
    }
    return true;
}

} // namespace bt
