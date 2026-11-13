#pragma once

/// The digit rule the URL and JSON decoders share, in one place; it needs nothing past the language, so MoonBase includes it as well.

namespace mm {

/// A hex digit's value, or -1 for any other character.
inline int hexDigit(char h) {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
}

/// Whether `text` is exactly `count` hex digits, as a digest written out is.
inline bool isHexDigits(const char* text, unsigned count) {
    unsigned n = 0;
    for (; text[n]; n++)
        if (n == count || hexDigit(text[n]) < 0) return false;
    return n == count;
}

}  // namespace mm
