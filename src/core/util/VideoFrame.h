#pragma once

#include <cstdint>

namespace mm {

/// One decoded video frame, produced by VideoService and read by video-reactive effects.
///
/// @moreinfo
///
/// The same plain-struct contract as AudioFrame, except that a frame is hundreds of kilobytes, so this borrows a pointer to the producer's buffer rather than carrying the pixels.
/// `rgb` is valid only until VideoService's next tick, so hold it for one effect tick and never across frames.
/// Before any frame exists it is null, which every consumer must tolerate.
///
/// ## The tone curve
///
/// `tone` maps the source's encoding to linear light, and SDR is sRGB, a curve like any other, so a published frame always carries one.
/// Read pixels through channel(): a consumer averaging the encoded bytes averages a quantity that is not proportional to light.
/// The table is 12-bit rather than 8 because linear has no headroom at the dark end, which is what encodings exist for.
/// Small enough that a zone of ~1M pixels still sums inside a uint32.
struct VideoFrame {
    /// The pixels: width*height*3, row-major, top-left origin, no padding.
    const uint8_t* rgb = nullptr;
    /// Frame width in pixels.
    uint16_t width = 0;
    /// Frame height in pixels.
    uint16_t height = 0;
    // Compare for INEQUALITY, never ordering: a still PPM bumps it every tick, the way a camera aimed at a still object sends one every period.
    /// Bumped per published frame.
    uint32_t seq = 0;
    // Same one-tick lifetime as `rgb`.
    /// 256-entry curve from the source's encoding to linear light, 0..kLinearMax.
    const uint16_t* tone = nullptr;
    /// The full-scale value of a linear sample.
    static constexpr uint16_t kLinearMax = 4095;

    // One lookup on a read the caller already makes.
    /// One channel of the pixel at `px` as linear light, or the encoded byte when no curve is published.
    uint16_t channel(const uint8_t* px, int c) const { return tone ? tone[px[c]] : px[c]; }
};

/// The "no source" frame consumers fall back to.
inline constexpr VideoFrame kNoVideoFrame{};

} // namespace mm
