#pragma once

#include <cstdint>

namespace mm {

/// One light's color, the three channels every effect writes.
struct RGB {
    uint8_t r;   ///< red
    uint8_t g;   ///< green
    uint8_t b;   ///< blue
};

/// Integer HSV to RGB, the hue circle mapped onto six sectors.
constexpr RGB hsvToRgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return {v, v, v};

    // Scale hue to 0-1535 (6 sectors * 256)
    uint16_t hue = static_cast<uint16_t>(h) * 6;
    uint8_t sector = static_cast<uint8_t>(hue >> 8);    // 0-5
    uint8_t frac = static_cast<uint8_t>(hue & 0xFF);    // 0-255 within sector

    // The three partial values a sector interpolates between.
    uint8_t p = static_cast<uint8_t>((static_cast<uint16_t>(v) * (255 - s)) >> 8);
    uint8_t q = static_cast<uint8_t>((static_cast<uint16_t>(v) * (255 - ((static_cast<uint16_t>(s) * frac) >> 8))) >> 8);
    uint8_t t = static_cast<uint8_t>((static_cast<uint16_t>(v) * (255 - ((static_cast<uint16_t>(s) * (255 - frac)) >> 8))) >> 8);

    switch (sector) {
        case 0:  return {v, t, p};
        case 1:  return {q, v, p};
        case 2:  return {p, v, t};
        case 3:  return {p, q, v};
        case 4:  return {t, p, v};
        default: return {v, p, q};
    }
}

/// Integer RGB → (hue 0..359, saturation 0..255).
inline void rgbToHueSat(uint8_t r, uint8_t g, uint8_t b, uint16_t& hue, uint16_t& sat) {
    const uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const uint8_t delta = static_cast<uint8_t>(mx - mn);
    if (delta == 0 || mx == 0) { hue = 0; sat = 0; return; }
    sat = static_cast<uint16_t>((static_cast<uint32_t>(delta) * 255) / mx);
    int32_t h;
    if (mx == r)      h = 60 * (static_cast<int32_t>(g) - b) / delta;
    else if (mx == g) h = 120 + 60 * (static_cast<int32_t>(b) - r) / delta;
    else              h = 240 + 60 * (static_cast<int32_t>(r) - g) / delta;
    if (h < 0) h += 360;
    hue = static_cast<uint16_t>(h % 360);
}

/// Scale a channel by a fraction, corrected so a full scale returns the value unchanged.
constexpr uint8_t scale8(uint8_t val, uint8_t scale) {
    return static_cast<uint8_t>(((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) + 1 + ((static_cast<uint16_t>(val) * static_cast<uint16_t>(scale)) >> 8)) >> 8);
}

} // namespace mm
