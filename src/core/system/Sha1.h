#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

/// A minimal RFC 3174 implementation, used only where RFC 6455 mandates it.
///
/// @technical
///
/// This is not a general-purpose crypto primitive.
/// SHA-1 is broken for security, and it appears here solely because the WebSocket standard requires it for the accept-key derivation.
///
/// It sits out of class so it is not bound to one translation unit, and in the namespace so a caller needs no qualifier.
/// It is inline so any future unit that wants SHA-1 includes this header without a separate implementation file, the function being small enough that duplicating it costs little.

/// Hash `len` bytes at `data` into `out`, which must hold 20 bytes.
inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Pad message
    size_t padLen = ((len + 8) / 64 + 1) * 64;
    uint8_t padded[512] = {};
    if (padLen > sizeof(padded)) return; // input too large for our buffer
    std::memcpy(padded, data, len);
    padded[len] = 0x80;
    uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; i++) {
        padded[padLen - 1 - i] = static_cast<uint8_t>(bitLen >> (i * 8));
    }

    for (size_t offset = 0; offset < padLen; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(padded[offset + i * 4]) << 24) |
                   (static_cast<uint32_t>(padded[offset + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(padded[offset + i * 4 + 2]) << 8) |
                    static_cast<uint32_t>(padded[offset + i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (v << 1) | (v >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    auto store32 = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);  p[3] = static_cast<uint8_t>(v);
    };
    store32(out, h0); store32(out + 4, h1); store32(out + 8, h2);
    store32(out + 12, h3); store32(out + 16, h4);
}

} // namespace mm
