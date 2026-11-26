#pragma once

#include "light/powerfunctions/wled_fonts.h"   // the five fonts every image carries

namespace mm::wbf {

/// Names in select order. The five WLED faces are in the firmware. A PixelFonts face is a file.
inline constexpr const char* kNames[] = {
    "Tom Thumb",
    "Tiny Unicode",
    "5x12",
    "Console 6x8",
    "C64",
};
inline constexpr uint8_t kCount = sizeof(kNames) / sizeof(kNames[0]);

/// PixelFonts faces the menu offers before the file is on the device. Choosing one installs `/moonlive/fonts/<name>`.
inline constexpr const char* kFactoryFonts[] = {
    "pixelfont_11x5_thin.wbf",
    "pixelfont_13x5_thin.wbf",
    "pixelfont_13x7_semibold.wbf",
    "pixelfont_13x7_thin.wbf",
    "pixelfont_15x5_semibold.wbf",
    "pixelfont_15x5_thin.wbf",
    "pixelfont_15x7_semibold.wbf",
    "pixelfont_15x7_thin.wbf",
    "pixelfont_17x5_semibold.wbf",
    "pixelfont_17x5_thin.wbf",
    "pixelfont_17x7_bold.wbf",
    "pixelfont_17x7_semibold.wbf",
    "pixelfont_17x7_thin.wbf",
    "pixelfont_21x11_bold.wbf",
    "pixelfont_21x11_thin.wbf",
    "pixelfont_5x5_thin.wbf",
    "pixelfont_7x5_thin.wbf",
    "pixelfont_9x5_thin.wbf",
};
inline constexpr uint8_t kFactoryCount = sizeof(kFactoryFonts) / sizeof(kFactoryFonts[0]);

/// The packed face at a select index, or the first face when the index is past the embedded set.
inline const Blob* blob(uint8_t i) {
    if (i < kWledCount) return &kWled[i];
    return &kWled[0];
}

}  // namespace mm::wbf
