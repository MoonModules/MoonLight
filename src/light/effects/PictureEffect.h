#pragma once

#include "light/effects/EffectBase.h"
#include "core/services/PictureService.h"   // the picture it samples, as audio effects include AudioService

namespace mm {

/// Effect: the active Picture service's picture, sampled onto the layer, so a picture of any size shows on a layout of any size.
///
/// Geometry stays with the modifiers: a rotate, mirror or tile transforms the picture as it does any effect. A cube shows it on every slice.
///
/// A cell's source rectangle is a column range times a row range, so two small tables sized at prepare hold them, refilled without allocating.
///
/// Area sampling averages in linear light, squaring each value and taking the root of the mean, gamma 2 standing in for the display curve.
class PictureEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "🖼️"; }
    /// A picture is a plane, and a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// The ways `fit` places the picture, in option order.
    enum Fit : uint8_t { kFit, kFill, kStretch, kFitCount };
    /// Keep the picture's aspect with bands or cropped, or distort it to the grid.
    uint8_t fit = kFit;
    /// The ways `sampling` reads a cell's part of the picture, in option order.
    enum Sampling : uint8_t { kPoint, kArea, kSamplingCount };
    /// One pixel per cell, or the average of the cell's rectangle.
    uint8_t sampling = kPoint;

    /// Publish the placement and the sampling.
    void defineControls() override {
        static constexpr const char* kFits[] = {"fit", "fill", "stretch"};
        static constexpr const char* kSamplings[] = {"point", "area"};
        controls_.addSelect("fit", fit, kFits, kFitCount);
        controls_.addSelect("sampling", sampling, kSamplings, kSamplingCount);
    }
    /// The placement changes the ranges, so it refills them.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "fit") == 0; }

    /// Size the range tables to the grid; the first picture fills them.
    void prepare() override {
        cols_.resize(width());
        rows_.resize(height());
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
        if (!redraw && f->generation == lastGen_ && layerGen == lastLayerGen_) return;
        gather(*f);
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
        for (uint32_t i = 0; i < n; i++) {
            const int32_t k = static_cast<int32_t>(i) - offset;
            if (k < 0 || k >= used) { out[i] = {0, 0}; continue; }
            uint32_t lo = static_cast<uint32_t>(k) * pic / static_cast<uint32_t>(used), hi = (static_cast<uint32_t>(k) + 1) * pic / static_cast<uint32_t>(used);
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
                draw::pixel(cv, cell, sampling == kArea ? average(f, c, r) : at(f, (c.lo + c.hi - 1) / 2, (r.lo + r.hi - 1) / 2));
            }
        }
    }

    /// The pixel at (x, y).
    static RGB at(const PictureFrame& f, uint32_t x, uint32_t y) {
        const uint8_t* p = f.rgb + (static_cast<size_t>(y) * f.width + x) * 3;
        return RGB{p[0], p[1], p[2]};
    }

    /// The rectangle's mean in linear light, from at most four reads per axis.
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

    ScratchBuffer<Range> cols_{*this};   ///< each grid column's picture columns
    ScratchBuffer<Range> rows_{*this};   ///< each grid row's picture rows
    uint16_t picW_ = 0, picH_ = 0;       ///< the picture size the tables were filled for
    static constexpr uint32_t kNoPicture = UINT32_MAX;   ///< what lastGen_ holds after clearing for a missing picture
    uint32_t lastGen_ = 0;               ///< the picture last sampled
    uint32_t lastLayerGen_ = 0;          ///< the layer's write generation after that sample
};

}  // namespace mm
