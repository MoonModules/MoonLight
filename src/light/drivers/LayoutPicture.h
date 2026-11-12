#pragma once

#include "core/util/ScratchBuffer.h"
#include "light/layers/Layer.h"

#include <cstring>

namespace mm {

/// The layout as a picture, for a driver that shows it on a screen or in a stream: which light each pixel of the physical front face shows.
///
/// A table of its own beside the layer's mapping, which serves the logical box after the modifiers; a screen shows the physical layout.
class LayoutPicture {
public:
    /// Declared as a member of the driver it serves, which owns the table's memory.
    explicit LayoutPicture(MoonModule& owner) : table_(owner) {}

    /// Follow `layer`'s layout over a `w` by `h` face: no table where its order is the raster, else each pixel's light; false when the table finds no memory.
    bool place(const Layer& layer, lengthType w, lengthType h) {
        placed_ = true;
        w_ = w;
        h_ = h;
        Layouts* layouts = layer.layouts();
        const size_t cells = static_cast<size_t>(w) * h;
        // No table where the order is the raster and every pixel is a light index this board can hold.
        if (!layouts || (cells <= static_cast<size_t>(Layer::kNoDriver) && layer.isNaturalOrder())) { table_.resize(0); return true; }
        if (!table_.resize(cells)) return placed_ = false;   // a refused resize leaves the table empty: the raster order
        for (size_t c = 0; c < cells; c++) table_[c] = Layer::kNoDriver;
        Walk walk{this, w, h};
        // A gap claims no pixel, as Layer::placeByLayout() has it: a lit light at the same place shows, and a lone gap's pixel stays black from draw().
        layouts->placeLights(CoordSink{&placeOne, [](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, &walk});
        return true;
    }

    /// Whether the last place() found room for the table it needed.
    bool placed() const MM_NONBLOCKING { return placed_; }

    /// What a status line ends with: an empty string, or why the picture follows the lights' own order.
    const char* note() const MM_NONBLOCKING { return placed_ ? "" : ", no memory to place the lights by the layout"; }

    /// The light pixel `p` shows, or Layer::kNoDriver where none does.
    nrOfLightsType lightAt(size_t p) const MM_NONBLOCKING { return table_ ? table_[p] : static_cast<nrOfLightsType>(p); }

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
    /// What placeOne() fills: the picture, and its face's size.
    struct Walk { LayoutPicture* self; lengthType w, h; };

    /// One light onto its pixel of the front face, where the first light on a pixel shows.
    static void placeOne(void* p, nrOfLightsType i, lengthType x, lengthType y, lengthType z) {
        auto* k = static_cast<Walk*>(p);
        const Coord3D pos{x, y, z}, face{k->w, k->h, 1};
        if (!pos.isInside(face)) return;   // the front face is the picture
        nrOfLightsType& cell = k->self->table_[pos.indexIn(face)];
        if (cell == Layer::kNoDriver) cell = i;
    }

    ScratchBuffer<nrOfLightsType> table_;   ///< each pixel's light, or none where the layout's order is the raster
    bool placed_ = true;                    ///< whether the last place() had room for its table
    lengthType w_ = 0, h_ = 0;              ///< the face the last place() followed
};

}  // namespace mm
