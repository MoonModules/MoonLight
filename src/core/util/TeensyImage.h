#pragma once

/// @defgroup TeensyImage Vetting a Teensy 4 image
/// @{
/// The checks an uploaded image passes before it may overwrite a Teensy's only program: a Teensy 4 image, a MoonLight one, and for the firmware the board runs.
///
/// A wrong image leaves the board offline until a USB flash, so the checks are pure byte reads, which a host test drives with synthetic and real images.
///
/// The mark's prefix comes from the caller's own mark, so it occurs once in a firmware image and a literal here cannot be found in its place.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm::firmware {

/// Where the boot ROM reads the image vector table, whose first byte is 0xD1.
inline constexpr uint32_t kTeensyIvtOffset = 0x1000;

/// Why the first bytes of an upload are no Teensy 4 image, or null when they are.
inline const char* teensyHeaderProblem(const uint8_t* first, size_t n) {
    if (n >= 1 && first[0] == ':') return "error: that is a HEX file, upload the .bin";
    if (n < 4 || std::memcmp(first, "FCFB", 4) != 0) return "error: not a Teensy 4 image";
    return nullptr;
}

/// The variant an image's mark names, up to its `;`, into `out`, or null when the image carries no mark; `mark` begins with the `prefixLen` bytes searched for.
inline const char* markedVariant(const uint8_t* image, uint32_t bytes, const char* mark, uint32_t prefixLen, char* out, size_t cap) {
    if (!cap) return nullptr;
    for (uint32_t i = 0; i + prefixLen < bytes; i++) {
        if (image[i] != static_cast<uint8_t>(mark[0]) || std::memcmp(image + i, mark, prefixLen) != 0) continue;
        size_t k = 0;
        for (uint32_t j = i + prefixLen; j < bytes && image[j] != ';' && k + 1 < cap; j++) out[k++] = static_cast<char>(image[j]);
        out[k] = 0;
        return out;
    }
    return nullptr;
}

/// Whether a whole staged image is refused for a board running `running`, the reason written to `status`.
inline bool teensyImageRefused(const uint8_t* staged, uint32_t bytes, const char* mark, uint32_t prefixLen,
                               const char* running, char* status, size_t statusLen) {
    if (bytes <= kTeensyIvtOffset || staged[kTeensyIvtOffset] != 0xD1) {
        std::snprintf(status, statusLen, "error: not a Teensy 4 image");
        return true;
    }
    char name[32];
    if (!markedVariant(staged, bytes, mark, prefixLen, name, sizeof(name))) {
        std::snprintf(status, statusLen, "error: that image is not MoonLight");
        return true;
    }
    if (std::strcmp(name, running) != 0) {
        std::snprintf(status, statusLen, "error: image is %.20s, board runs %s", name, running);
        return true;
    }
    return false;
}

}  // namespace mm::firmware

/// @}
