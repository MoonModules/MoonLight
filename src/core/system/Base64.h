#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mm {

/// RFC 4648 with the standard alphabet and `=` padding.
///
/// @technical
///
/// Two places use it.
/// The WebSocket handshake response encodes a hash of the client key and the protocol's magic string, and the password serialization in the state response obfuscates with an XOR before encoding.
/// Both payloads are short, so the encoder is straightforward rather than optimized.

/// Encode `in` into `out` as a null-terminated string, truncating rather than overflowing.
inline void base64Encode(std::span<const uint8_t> in, std::span<char> out) {
    if (out.empty()) return;  // no room even for the terminator
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t inLen = in.size();
    const size_t outMax = out.size();
    size_t oi = 0;
    for (size_t i = 0; i < inLen && oi + 4 < outMax; i += 3) {
        uint32_t n = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < inLen) n |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (i + 2 < inLen) n |= static_cast<uint32_t>(in[i + 2]);
        out[oi++] = table[(n >> 18) & 0x3F];
        out[oi++] = table[(n >> 12) & 0x3F];
        out[oi++] = (i + 1 < inLen) ? table[(n >> 6) & 0x3F] : '=';
        out[oi++] = (i + 2 < inLen) ? table[n & 0x3F] : '=';
    }
    out[oi] = 0;
}

} // namespace mm
