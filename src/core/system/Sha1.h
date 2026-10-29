#pragma once

#include <cstdint>
#include <cstring>

namespace mm {

/// A minimal RFC 3174 implementation, used only where RFC 6455 mandates it.
///
/// @technical
///
/// SHA-1 is broken for security.
///
/// An inline free function in the namespace, so any unit includes it with no implementation file and calls it unqualified, at the cost of a small copy per unit.

namespace detail {

/// One 64-byte block of the FIPS 180-4 compression function, folded into the five hash words `h`.
inline void sha1Block(uint32_t h[5], const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
               (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                static_cast<uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++) {
        uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (v << 1) | (v >> 31);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

}  // namespace detail

/// Hash `len` bytes at `data` into `out`, which must hold 20 bytes.
inline void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    // Whole blocks stream straight from the input; only the tail is padded, into one or two blocks.
    const size_t full = len / 64;
    for (size_t i = 0; i < full; i++) detail::sha1Block(h, data + i * 64);

    const size_t rest = len - full * 64;
    uint8_t tail[128] = {};
    if (rest) std::memcpy(tail, data + full * 64, rest);
    tail[rest] = 0x80;
    const size_t tailLen = rest + 9 <= 64 ? 64 : 128;   // the 0x80 and the 8-byte length must fit
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; i++) tail[tailLen - 1 - i] = static_cast<uint8_t>(bitLen >> (i * 8));
    for (size_t off = 0; off < tailLen; off += 64) detail::sha1Block(h, tail + off);

    // The digest is the five words, big-endian.
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

} // namespace mm
