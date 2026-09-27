// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// @defgroup sha256 SHA-256, vendored
/// @{
/// SHA-256 as FIPS 180-4 defines it, carried in the tree rather than linked from a library.
///
/// @moreinfo
///
/// ## Why a copy rather than a library
///
/// This is the only cryptographic primitive MoonLight needs, and it needs it on all five targets.
/// OpenSSL does not exist on ESP32, where ESP-IDF ships mbedtls, so linking it would mean an mbedtls path for devices and an OpenSSL path for the desktop.
/// That is two implementations of one function that must agree byte for byte, and a divergence produces identifiers that silently differ between platforms.
/// One vendored file is the smaller thing to own, and the algorithm is frozen.
/// FIPS 180-4 has not changed since 2015 and its test vectors are published, so `unit_sha256.cpp` pins this against them.
///
/// ## What it is not
///
/// The one use today is the MoonStats installation id.
/// This is no general-purpose crypto layer: no HMAC, no streaming over a socket, no constant-time comparison.
/// Nothing here needs them, and an unused primitive is a maintenance cost with no user.
///
/// ## Why one shot and why truncation is safe
///
/// `sha256` takes a whole buffer instead of the usual init, update and final trio.
/// Every caller hashes a short buffer already in memory, so the streaming form would be three functions nobody calls with more than one update.
///
/// `sha256Hex` truncates, which is the standard way to derive a shorter identifier (NIST SP 800-107 section 5.1).
/// At the default of 16 bytes it leaves 128 bits, far past any collision concern for a population of LED controllers.

#include <cstddef>
#include <cstdint>

namespace mm {

/// Digest length in bytes.
inline constexpr size_t kSha256DigestSize = 32;

/// Hash `len` bytes at `data` into `out`, which must hold 32 bytes.
void sha256(const void* data, size_t len, uint8_t out[kSha256DigestSize]);

/// Hash `len` bytes and write the first `outBytes` of the digest as lowercase hex into `out`, which must hold `outBytes * 2 + 1` characters.
void sha256Hex(const void* data, size_t len, char* out, size_t outBytes = 16);

/// @}
}  // namespace mm
