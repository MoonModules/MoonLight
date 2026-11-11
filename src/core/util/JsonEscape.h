#pragma once
/// A string escaped for a JSON string literal (RFC 8259 § 7): the quote, the backslash and every control character, which a literal may not hold raw.
///
/// Dependency-free, so the app's JSON writer and the migration image's network document share it.
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace mm {

/// The escape for one byte into `esc` (room for 7), its length: the byte itself, a backslash pair, or `\u00XX` for a control character without a short form.
inline size_t jsonEscapeByte(unsigned char c, char* esc) {
    static constexpr char kShort[] = "\b\f\n\r\t";
    static constexpr char kLetter[] = "bfnrt";
    if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = static_cast<char>(c); return 2; }
    if (c >= 0x20) { esc[0] = static_cast<char>(c); return 1; }
    if (const char* at = c ? std::strchr(kShort, c) : nullptr) { esc[0] = '\\'; esc[1] = kLetter[at - kShort]; return 2; }
    return static_cast<size_t>(std::snprintf(esc, 7, "\\u%04x", c));
}

/// Escape `in` into `out`, the contents between the quotes, truncating at a whole escape rather than overflowing.
inline void jsonEscape(const char* in, char* out, size_t outMax) {
    if (outMax == 0) return;
    size_t oi = 0;
    for (; *in; in++) {
        char esc[7];
        const size_t n = jsonEscapeByte(static_cast<unsigned char>(*in), esc);
        if (oi + n + 1 > outMax) break;
        std::memcpy(out + oi, esc, n);
        oi += n;
    }
    out[oi] = 0;
}

}  // namespace mm
