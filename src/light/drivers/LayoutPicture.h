#pragma once

#include "core/util/ScratchBuffer.h"
#include "light/layers/Layer.h"

#include <algorithm>   // std::min, std::max: the box around a window's lights
#include <cstring>

namespace mm {

/// A frame for a 4:2:0 encoder, its chroma sampled in 2x2 blocks: an odd width or height doubles the scale, so both come out even, as every such encoder, hardware or libx264, takes.
inline void evenFrame(uint8_t& scale, uint32_t& w, uint32_t& h) {
    if (!(w & 1u) && !(h & 1u)) return;
    scale = static_cast<uint8_t>(scale * 2);
    w *= 2;
    h *= 2;
}

/// The layout as a picture, for a driver that shows it on a screen or in a stream: which light each pixel of the physical front face shows.
///
/// A table of its own beside the layer's mapping, which serves the logical box after the modifiers; a screen shows the physical layout.
///
/// A driver showing a window of the lights, its start and count, gets the picture cropped to where those lights sit: two screens, each its half of one canvas.
class LayoutPicture {
public:
    /// Declared as a member of the driver it serves, which owns the table's memory.
    explicit LayoutPicture(MoonModule& owner) : table_(owner) {}

    /// Follow `layer`'s layout over a `w` by `h` face, each pixel's light; false when memory refuses its table. A window, `count` lights from `from`, crops it to where they sit: both 0 the whole face, no lights one dark pixel.
    bool place(const Layer& layer, lengthType w, lengthType h, nrOfLightsType from = 0, nrOfLightsType count = 0) {
        placed_ = true;
        dark_ = false;
        w_ = w;
        h_ = h;
        Layouts* layouts = layer.layouts();
        const size_t cells = static_cast<size_t>(w) * h;
        const bool whole = from == 0 && count == 0;
        // No table where the order is the raster and every pixel is a light index this board can hold.
        if (whole && (!layouts || (cells <= static_cast<size_t>(Layer::kNoDriver) && layer.isNaturalOrder()))) { table_.resize(0); return true; }
        if (!table_.resize(cells)) {
            // A refused resize leaves the table empty: the raster order for the whole face, one dark pixel for a window, which the raster cannot keep.
            if (!whole) { w_ = h_ = 1; dark_ = true; }
            return placed_ = false;
        }
        fill(layer, layouts != nullptr, cells);
        if (!whole) crop(from, count);
        return true;
    }

    /// The face's width after place(), cropped to a window.
    lengthType width() const MM_NONBLOCKING { return w_; }
    /// The face's height after place(), cropped to a window.
    lengthType height() const MM_NONBLOCKING { return h_; }

    /// Whether the last place() found room for the table it needed.
    bool placed() const MM_NONBLOCKING { return placed_; }

    /// What a status line ends with: an empty string, or why the picture follows the lights' own order.
    const char* note() const MM_NONBLOCKING { return placed_ ? "" : ", no memory to place the lights by the layout"; }

    /// The light pixel `p` shows, or Layer::kNoDriver where none does.
    nrOfLightsType lightAt(size_t p) const MM_NONBLOCKING { return table_ ? table_[p] : dark_ ? Layer::kNoDriver : static_cast<nrOfLightsType>(p); }

    /// Draw the placed face into `dst` as `scale` by `scale` blocks, each pixel's light converted once by `correct`; black where none reaches.
    template <class Correct>
    void draw(const uint8_t* src, uint8_t srcCh, nrOfLightsType have, uint8_t scale, uint8_t* dst, Correct correct) const MM_NONBLOCKING {
        const lengthType w = w_, h = h_;
        const size_t rowBytes = static_cast<size_t>(w) * scale * 3;
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const nrOfLightsType i = lightAt(static_cast<size_t>(y) * w + x);
                uint8_t rgb[3] = {0, 0, 0};
                if (i < have) correct(src + static_cast<size_t>(i) * srcCh, rgb);
                uint8_t* block = dst + static_cast<size_t>(y) * scale * rowBytes + static_cast<size_t>(x) * scale * 3;
                for (uint8_t px = 0; px < scale; px++) std::memcpy(block + px * 3, rgb, 3);
                for (uint8_t py = 1; py < scale; py++) std::memcpy(block + py * rowBytes, block, static_cast<size_t>(scale) * 3);
            }
    }

private:
    /// Each pixel's light: the layout's where it has one, the raster order where it has none.
    void fill(const Layer& layer, bool byLayout, size_t cells) {
        if (byLayout) { layer.placeByLayout(table_.data(), cells, {w_, h_, 1}); return; }   // the front face is the picture
        for (size_t p = 0; p < cells; p++) table_[p] = p < static_cast<size_t>(Layer::kNoDriver) ? static_cast<nrOfLightsType>(p) : Layer::kNoDriver;
    }

    /// Keep the box around the window's lights and dark every other light in it, moving the rows up in place; one dark pixel where the window has no light on the face.
    void crop(nrOfLightsType from, nrOfLightsType count) {
        const uint64_t end = static_cast<uint64_t>(from) + count;
        const auto inWindow = [&](nrOfLightsType i) { return i != Layer::kNoDriver && i >= from && i < end; };
        const Box b = boxOf(inWindow);
        if (b.x0 > b.x1 || b.y0 > b.y1) { w_ = h_ = 1; table_[0] = Layer::kNoDriver; return; }
        moveBox(b.x0, b.y0, static_cast<lengthType>(b.x1 - b.x0 + 1), static_cast<lengthType>(b.y1 - b.y0 + 1), inWindow);
    }

    /// A box on the face, its corners inclusive; empty while x0 > x1.
    struct Box { lengthType x0, y0, x1, y1; };

    /// The box around the pixels whose light `keep` takes.
    template <class Keep>
    Box boxOf(Keep keep) const {
        Box b{w_, h_, 0, 0};
        for (size_t p = 0; p < static_cast<size_t>(w_) * h_; p++) {
            if (!keep(table_[p])) continue;
            const lengthType x = static_cast<lengthType>(p % w_), y = static_cast<lengthType>(p / w_);
            b = {std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y)};
        }
        return b;
    }

    /// Move the `nw` by `nh` box at (x0, y0) to the table's start, darking every light `keep` refuses; each cell lands at or before where it was read, so the copy runs in place.
    template <class Keep>
    void moveBox(lengthType x0, lengthType y0, lengthType nw, lengthType nh, Keep keep) {
        for (size_t p = 0; p < static_cast<size_t>(nw) * nh; p++) {
            const nrOfLightsType i = table_[static_cast<size_t>(y0 + p / nw) * w_ + x0 + p % nw];
            table_[p] = keep(i) ? i : Layer::kNoDriver;
        }
        w_ = nw;
        h_ = nh;
    }

    ScratchBuffer<nrOfLightsType> table_;   ///< each pixel's light, or none where the layout's order is the raster
    bool placed_ = true;                    ///< whether the last place() had room for its table
    bool dark_ = false;                     ///< a window refused its table: one dark pixel rather than lights outside it
    lengthType w_ = 0, h_ = 0;              ///< the face the last place() followed
};

}  // namespace mm
