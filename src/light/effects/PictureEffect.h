#pragma once

#include "light/effects/EffectBase.h"
#include "core/services/PictureService.h"   // the picture it samples, as audio effects include AudioService

namespace mm {

/// The active @ref PictureService "Picture service"'s picture on the layer, so a picture of any size shows on a layout of any size.
/// @card PictureEffect.gif
///
/// A modifier rotates, mirrors or tiles it as it does any effect, and a cube shows it on every slice.
class PictureEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "🖼️"; }
    /// A picture is a plane, and a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// The ways `fit` places the picture, in option order.
    enum Fit : uint8_t { kFit, kFill, kStretch, kFitCount };
    /// Keep the aspect with bands or cropped, or stretch it to the grid.
    /// @guide **fit** shows the whole picture, with bands where its aspect leaves the grid empty. With **fill** it covers the grid and crops what overflows, and **stretch** distorts it to the grid's aspect.
    uint8_t fit = kFit;
    /// The ways `sampling` reads a cell's part of the picture, in option order.
    enum Sampling : uint8_t { kPoint, kArea, kSamplingCount };
    /// One pixel per light, or the mean of the light's rectangle.
    /// @guide **point** takes the one pixel at a light's center, sharp and the fastest. With **area** each light averages its rectangle of the picture, so fine detail stays steady when a large picture shrinks onto few lights.
    uint8_t sampling = kPoint;
    /// How far into the picture the outermost lights look, as a percent; 0 leaves them their own share.
    /// @guide A border strip's outermost lights own a sliver at the very edge, where a letterbox and the overscan junk live. **edgeDepth** lets them reach that far in instead. It costs reads in proportion.
    uint8_t edgeDepth = 0;
    /// How much of the gap to a new colour each light closes per picture; 0 follows the picture exactly.
    /// @guide Raise **smoothing** and a light eases toward its new colour over several pictures, which settles a noisy source. A cut is a real jump, so **snapAbove** lands a bigger move whole.
    uint8_t smoothing = 0;
    /// A channel moving further than this jumps rather than easing; 0 eases every move.
    uint8_t snapAbove = 80;

    /// Publish the placement and the sampling.
    void defineControls() override {
        static constexpr const char* kFits[] = {"fit", "fill", "stretch"};
        static constexpr const char* kSamplings[] = {"point", "area"};
        controls_.addSelect("fit", fit, kFits, kFitCount);
        controls_.addSelect("sampling", sampling, kSamplings, kSamplingCount);
        controls_.addControl("edgeDepth", edgeDepth, 0, 50);   // Hyperion samples ~8%
        controls_.addControl("smoothing", smoothing, 0, 255);
        controls_.addControl("snapAbove", snapAbove, 0, 255);
        controls_.setHidden(controls_.count() - 1, smoothing == 0);
    }
    /// The placement changes the ranges and the sampling the picture drawn, so both redraw.
    bool affectsPrepare(const char* name) const override {
        // edgeDepth moves the ranges, and smoothing allocates or frees the accumulators.
        return std::strcmp(name, "fit") == 0 || std::strcmp(name, "sampling") == 0
            || std::strcmp(name, "edgeDepth") == 0 || std::strcmp(name, "smoothing") == 0;
    }

    /// Size the range tables to the grid; the first picture fills them.
    void prepare() override {
        cols_.resize(width());
        rows_.resize(height());
        // 8.8 per channel, and only while smoothing, since a per-picture step is a fraction of a byte.
        const size_t cells = static_cast<size_t>(cols_.count()) * rows_.count();
        state_.resize(smoothing != 0 ? cells * 3u : 0);
        primed_ = false;
        picW_ = picH_ = 0;   // stale, so the next tick refills and draws
    }

    /// Sample a picture that is new, or redraw one the layer overwrote; with nothing to sample the layer is cleared once.
    void tick() MM_NONBLOCKING override {
        const PictureService::Reader reader;
        if (!reader) return;   // the source is swapping or filling its only picture: sample on the next tick
        const PictureFrame* f = &reader.frame();
        const Layer* l = layer();
        const uint32_t layerGen = l ? l->bufferGen() : 0;
        if (!f->rgb || !cols_ || !rows_) {
            setStatus("no picture: add a Picture service under Services");
            // An effect owns its background, so a missing picture shows as black rather than the last frame.
            if (lastGen_ != kNoPicture || layerGen != lastLayerGen_) draw::fill(canvas(), RGB{0, 0, 0});
            lastGen_ = kNoPicture;
            lastLayerGen_ = layerGen + 1;
            picW_ = picH_ = 0;   // a picture arriving later refills the ranges
            return;
        }
        if (status()) clearStatus();
        bool redraw = false;
        if (f->width != picW_ || f->height != picH_) {
            fillRanges(cols_.data(), static_cast<uint16_t>(cols_.count()), f->width, f->height, true);
            fillRanges(rows_.data(), static_cast<uint16_t>(rows_.count()), f->width, f->height, false);
            picW_ = f->width;
            picH_ = f->height;
            redraw = true;
        }
        // A settling smoother has not reached the picture yet, so it draws again on an unchanged one.
        if (!redraw && !settling_ && f->generation == lastGen_ && layerGen == lastLayerGen_) return;
        settling_ = false;
        gather(*f);
        primed_ = true;
        lastGen_ = f->generation;
        lastLayerGen_ = layerGen + 1;   // the Layer bumps the generation after this tick, for our own write
    }

private:
    /// One axis of a cell's source rectangle, [lo, hi) in picture pixels; empty outside a fitted picture.
    struct Range { uint16_t lo, hi; };

    /// Each cell's range along one axis: the grid's `n` cells over the picture's `picW` × `picH`, `columns` naming the axis.
    void fillRanges(Range* out, uint16_t n, uint16_t picW, uint16_t picH, bool columns) const {
        const uint32_t gw = cols_.count(), gh = rows_.count();
        uint32_t usedW = gw, usedH = gh;
        spannedCells(picW, picH, usedW, usedH);
        const int32_t used = static_cast<int32_t>(columns ? usedW : usedH);
        const uint32_t pic = columns ? picW : picH;
        const int32_t offset = (static_cast<int32_t>(columns ? gw : gh) - used) / 2;   // negative when cropped
        // What an edge cell reaches instead of its own share, deeper OR shallower. The control sets the depth rather than raising a floor under it, and 0 leaves every cell its share.
        const uint32_t deep = static_cast<uint32_t>(pic) * edgeDepth / 100u;
        for (uint32_t i = 0; i < n; i++) {
            const int32_t k = static_cast<int32_t>(i) - offset;
            if (k < 0 || k >= used) { out[i] = {0, 0}; continue; }
            uint32_t lo = static_cast<uint32_t>(k) * pic / static_cast<uint32_t>(used), hi = (static_cast<uint32_t>(k) + 1) * pic / static_cast<uint32_t>(used);
            if (deep) {
                if (k == 0) hi = deep;                       // the low edge looks inward from 0
                if (k == used - 1 && pic > deep) lo = pic - deep;   // and the high edge back from the end
            }
            if (hi <= lo) hi = lo + 1;   // a picture smaller than the grid: neighboring cells share a pixel
            out[i] = {static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)};
        }
    }

    /// The cells the picture spans: the grid itself when stretched, else its aspect sets one side, smaller than the grid with bands or larger and cropped.
    void spannedCells(uint16_t picW, uint16_t picH, uint32_t& usedW, uint32_t& usedH) const {
        if (fit == kStretch || !picW || !picH) return;
        const bool wider = static_cast<uint32_t>(picW) * usedH >= static_cast<uint32_t>(picH) * usedW;
        const uint32_t scaledH = (static_cast<uint32_t>(picH) * usedW + picW / 2) / picW;   // its height at the grid's width
        const uint32_t scaledW = (static_cast<uint32_t>(picW) * usedH + picH / 2) / picH;   // its width at the grid's height
        if (wider == (fit == kFit)) usedH = scaledH ? scaledH : 1;
        else usedW = scaledW ? scaledW : 1;
    }

    /// One pass over the grid, each cell from its rectangle.
    void gather(const PictureFrame& f) {
        const draw::Canvas cv = canvas();
        const uint32_t gw = cols_.count(), gh = rows_.count();
        for (uint32_t y = 0; y < gh; y++) {
            const Range r = rows_[y];
            for (uint32_t x = 0; x < gw; x++) {
                const Range c = cols_[x];
                const size_t cell = static_cast<size_t>(y) * gw + x;
                if (r.hi == r.lo || c.hi == c.lo) { draw::pixel(cv, cell, RGB{0, 0, 0}); continue; }
                const RGB want = sampling == kArea ? average(f, c, r) : at(f, (c.lo + c.hi - 1) / 2, (r.lo + r.hi - 1) / 2);
                draw::pixel(cv, cell, state_ ? eased(cell, want) : want);
            }
        }
    }

    /// One cell eased toward `want`, keeping the fraction of a step that a byte would lose.
    RGB eased(size_t cell, RGB want) {
        const uint8_t target[3] = {want.r, want.g, want.b};
        const int32_t step = 256 - smoothing;                      // 255 closes almost all of the gap
        const int32_t snap = static_cast<int32_t>(snapAbove) << 8;
        uint8_t out[3];
        for (int ch = 0; ch < 3; ch++) {
            const size_t slot = cell * 3 + ch;
            const int32_t held = state_[slot];
            const int32_t wanted = static_cast<int32_t>(target[ch]) << 8;
            const int32_t delta = wanted - held;
            // The first picture, and any move big enough to be a cut, land whole.
            const bool jump = !primed_ || (snapAbove && (delta > snap || delta < -snap));
            // >> floors, so without the nudge a rising channel stalls one count short for ever.
            int32_t move = (delta * step) >> 8;
            if (move == 0 && delta != 0) move = delta > 0 ? 1 : -1;
            const int32_t now = jump ? wanted : held + move;
            state_[slot] = static_cast<uint16_t>(now);
            out[ch] = static_cast<uint8_t>(now >> 8);
            if (now != wanted) settling_ = true;
        }
        return RGB{out[0], out[1], out[2]};
    }

    /// The pixel at (x, y).
    static RGB at(const PictureFrame& f, uint32_t x, uint32_t y) {
        const uint8_t* p = f.rgb + (static_cast<size_t>(y) * f.width + x) * 3;
        return RGB{p[0], p[1], p[2]};
    }

    /// The rectangle's mean in linear light (square, average, root: gamma 2 for the display curve), from at most four reads per axis.
    static RGB average(const PictureFrame& f, Range c, Range r) {
        const uint32_t sx = (c.hi - c.lo + 3u) / 4u, sy = (r.hi - r.lo + 3u) / 4u;
        uint32_t sum[3] = {0, 0, 0}, n = 0;
        for (uint32_t y = r.lo; y < r.hi; y += sy)
            for (uint32_t x = c.lo; x < c.hi; x += sx) {
                const RGB p = at(f, x, y);
                sum[0] += static_cast<uint32_t>(p.r) * p.r;
                sum[1] += static_cast<uint32_t>(p.g) * p.g;
                sum[2] += static_cast<uint32_t>(p.b) * p.b;
                n++;
            }
        return RGB{isqrt(sum[0] / n), isqrt(sum[1] / n), isqrt(sum[2] / n)};
    }

    /// The integer square root of a value below 65,536, the bitwise method.
    static uint8_t isqrt(uint32_t v) {
        uint32_t root = 0;
        for (uint32_t bit = 1u << 14; bit; bit >>= 2) {
            if (v >= root + bit) { v -= root + bit; root = (root >> 1) + bit; }
            else root >>= 1;
        }
        return static_cast<uint8_t>(root);
    }

    ScratchBuffer<uint16_t> state_{*this};   ///< 8.8 per channel per cell, only while smoothing
    bool primed_ = false;                    ///< whether a first picture has landed whole
    bool settling_ = false;                  ///< whether the last gather left a cell short of its target
    ScratchBuffer<Range> cols_{*this};   ///< each grid column's picture columns
    ScratchBuffer<Range> rows_{*this};   ///< each grid row's picture rows
    uint16_t picW_ = 0, picH_ = 0;       ///< the picture size the tables were filled for
    static constexpr uint32_t kNoPicture = UINT32_MAX;   ///< what lastGen_ holds after clearing for a missing picture
    uint32_t lastGen_ = 0;               ///< the picture last sampled
    uint32_t lastLayerGen_ = 0;          ///< the layer's write generation after that sample
};

}  // namespace mm
