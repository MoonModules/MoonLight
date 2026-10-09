#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/nonblocking.h"   // MM_NONBLOCKING: the app's tick1s calls it

namespace mm {

/// @defgroup DeviceName A device's name until someone sets one
/// @{
/// `MM-` and the last two bytes of the base MAC, shared by the app and MoonBase, so a board's access point carries the name it has in MoonLight.

/// Write the default name for `mac` into `out`, which holds at least 8 bytes.
inline void defaultDeviceName(const uint8_t mac[6], char* out, size_t len) MM_NONBLOCKING {
    if (len < 8) { if (len) out[0] = 0; return; }
    constexpr char kHex[] = "0123456789ABCDEF";
    out[0] = 'M'; out[1] = 'M'; out[2] = '-';
    out[3] = kHex[mac[4] >> 4]; out[4] = kHex[mac[4] & 15];
    out[5] = kHex[mac[5] >> 4]; out[6] = kHex[mac[5] & 15];
    out[7] = 0;
}

/// @}
}  // namespace mm
