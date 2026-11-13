// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// SHA-256 as FIPS 180-4 defines it, vendored rather than linked: @xref{one-copy-on-every-target|why}; one call over a buffer and a truncated hex: @xref{one-shot-and-truncated|why}.
///
/// @technical
///
/// ## One copy on every target
///
/// ESP-IDF ships mbedtls and the desktop OpenSSL, and two implementations of one function must agree byte for byte; one vendored file of a frozen algorithm is less to own.
///
/// `unit_sha256.cpp` pins it against FIPS 180-4's published test vectors. Its users: the MoonStats installation id, and a desktop's new program checked against the release's digest.
///
/// ## One shot and truncated
///
/// Every caller hashes a buffer already in memory, an identifier or a program of a few MB, so `sha256` takes the whole buffer rather than the init, update and final trio.
///
/// `sha256Hex` truncates, the standard way to derive a shorter identifier (NIST SP 800-107 § 5.1): 16 bytes leave 128 bits, far past any collision concern here.

#include <cstddef>
#include <cstdint>

namespace mm {

/// Digest length in bytes.
inline constexpr size_t kSha256DigestSize = 32;

/// Hash `len` bytes at `data` into `out`, which must hold 32 bytes.
void sha256(const void* data, size_t len, uint8_t out[kSha256DigestSize]);

/// Hash `len` bytes and write the first `outBytes` of the digest as lowercase hex into `out`, which must hold `outBytes * 2 + 1` characters.
void sha256Hex(const void* data, size_t len, char* out, size_t outBytes = 16);

}  // namespace mm
