#pragma once

#include <cstddef>
#include <cstdint>

/// @defgroup fnv A 32-bit fingerprint of a few bytes
/// @{
/// FNV-1a, the textbook small hash, for noticing that settings or a value changed; it is not cryptographic.

namespace mm {

/// FNV-1a folded a byte at a time, for a fingerprint built from several fields.
struct Fnv1a {
    uint32_t h = 2166136261u;   ///< the running hash, starting at the standard offset basis
    /// Fold one byte in.
    void add(uint8_t b) { h = (h ^ b) * 16777619u; }
    /// Fold `len` bytes in.
    void add(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; i++) add(p[i]);
    }
};

/// FNV-1a over `len` bytes.
inline uint32_t fnv1a(const void* data, size_t len) {
    Fnv1a f;
    f.add(data, len);
    return f.h;
}

}  // namespace mm

/// @}
