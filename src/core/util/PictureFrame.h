#pragma once

#include <cstdint>

namespace mm {

/// A picture as a source publishes it: RGB rows, top row first, three bytes a pixel.
///
/// `PictureService` produces it and `PictureEffect` samples it, so an effect never touches a capture device or a decoder.
struct PictureFrame {
    const uint8_t* rgb = nullptr;   ///< the pixels, null when there is no picture
    uint16_t width = 0;             ///< pixels across
    uint16_t height = 0;            ///< pixels down
    uint32_t generation = 0;        ///< changes with the pixels or the size, so a consumer skips a picture it already sampled
};

}  // namespace mm
