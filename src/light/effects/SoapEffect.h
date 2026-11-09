#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// A soap film's swirl: an image colored by noise, its rows and columns sliding by a drifting noise field, smearing into marbled bands.
/// @card SoapEffect.gif
///
/// Each frame slides every row by the noise in its first column and every column by the noise in its first row, a fraction of a light blended between two neighbors, and a light pulled in from past the edge takes the palette color of the noise there. A heavy load at a high resolution: belrik's stress test for a large panel.
///
/// @attribution Stepko, Stefan Petrick, blazoncek, Damian Schneider, softhack007
///
/// @technical
///
/// WLED's numbers are per frame at its 24 ms frame; here every frame moves by its share of that, so the film flows alike and as smoothly as the render allows at any rate.
///
/// The noise drifts by the elapsed time, accumulated exactly; the smoothing keeps `smoothness` of the old noise per 24 ms as a half-life, `halfLifeKeep`'s time-correct form of the exponential; the slide is the per-frame amount scaled by the elapsed time.
///
/// The blend rounds, its two weights summing to 255, so a light blended with its own color stays exact rather than darkening a step at a time.
class SoapEffect : public EffectBase {
public:
    /// Catalog tags: WLED origin, flow.
    const char* tags() const override { return "🐙💨"; }
    /// A plane, which extrude lifts through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast the noise field drifts.
    uint8_t speed = 128;
    /// How much old noise the film keeps: high is a slow, smooth swirl.
    uint8_t smoothness = 128;
    /// How far a row or column slides: high folds the bands tighter.
    uint8_t density = 128;

    /// Publish the drift, the smoothing and the slide.
    void defineControls() override {
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("smoothness", smoothness, 0, 255);
        controls_.addControl("density", density, 0, 255);
    }

    /// Size the noise field, the image and one line, and start the film again.
    void prepare() override {
        const size_t lights = static_cast<size_t>(width()) * height();
        noise_.resize(lights);
        image_.resize(lights * 3u);
        line_.resize(static_cast<size_t>(width() > height() ? width() : height()) * 3u);
        started_ = false;
    }

    /// Move the film by the time since the last frame, then show it.
    void tick() MM_NONBLOCKING override {
        if (!noise_ || !image_ || !line_) return;   // a 0x0 grid allocates none
        const lengthType w = width(), h = height();
        const uint32_t now = elapsed();
        if (!started_) start(w, h, now);
        const uint32_t dt = now - lastMs_;
        lastMs_ = now;
        // A long stall (WiFi reconnect, reflash) moves the film by at most a quarter second, not a jump.
        step(w, h, dt < 250 ? dt : 250);
        show(w, h);
    }

private:
    /// The noise coordinates the film starts at, fixed so every device shows the same film.
    static constexpr uint32_t kStart[3] = {0x1F2E3D4Cu, 0x5B6A7988u, 0x97A6B5C4u};

    /// WLED's frame, the unit its per-frame numbers are written in.
    static constexpr uint32_t kFrameMs = 24;

    /// Fill the noise field without smoothing and color the image from it.
    void start(lengthType w, lengthType h, uint32_t now) {
        drift_ = 0;
        lastMs_ = now;
        sampleNoise(w, h, 0);
        const size_t lights = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < lights; i++) put(i, edgeColor(noise_[i]));
        started_ = true;
    }

    /// `dt` milliseconds of film: the noise drifts and is smoothed, then the rows slide and the columns slide.
    void step(lengthType w, lengthType h, uint32_t dt) {
        drift_ += static_cast<uint64_t>(w < h ? w : h) * (static_cast<uint64_t>(speed) + 2u) / 2u * dt;
        sampleNoise(w, h, keepFor(dt));
        slide(true, w, h, dt);
        slide(false, w, h, dt);
    }

    /// The share of the old noise kept over `dt`, of 65536: `smoothness` of it per WLED frame, as a half-life.
    uint32_t keepFor(uint32_t dt) const {
        const uint32_t s = smoothness < 250 ? smoothness : 250;   // above 250 the film barely moves
        if (s == 0) return 0;
        // keep^(t / frame) halves every frame * log2(2) / log2(255 / keep), with log2 in 8.8.
        const uint32_t halfLife = kFrameMs * 256u / (log2Q8(255) - log2Q8(s));
        return halfLifeKeep(dt, halfLife > 0 ? halfLife : 1);
    }

    /// The noise field at the current coordinates, `keep` of 65536 of each old value kept.
    void sampleNoise(lengthType w, lengthType h, uint32_t keep) {
        const uint32_t along = static_cast<uint32_t>(drift_ / kFrameMs);
        const uint32_t cx = kStart[0] + along, cy = kStart[1] + along, cz = kStart[2] + along;
        const uint32_t sx = 160000u / static_cast<uint32_t>(w);
        const uint32_t sy = 160000u / static_cast<uint32_t>(h);
        for (lengthType y = 0; y < h; y++) {
            const int32_t oy = static_cast<int32_t>(sy) * (y - h / 2);
            for (lengthType x = 0; x < w; x++) {
                const int32_t ox = static_cast<int32_t>(sx) * (x - w / 2);
                const uint32_t n = inoise16(cx + static_cast<uint32_t>(ox), cy + static_cast<uint32_t>(oy), cz) >> 8;
                uint8_t& v = noise_[static_cast<size_t>(y) * w + x];
                v = static_cast<uint8_t>((static_cast<uint32_t>(v) * keep + n * (65536u - keep) + 32768u) >> 16);
            }
        }
    }

    /// Slide every row (`rows`) or every column by the noise at its start over `dt`, blending a fraction between two lights.
    void slide(bool rows, lengthType w, lengthType h, uint32_t dt) {
        const int32_t lines = rows ? h : w;   // how many lines slide
        const int32_t len = rows ? w : h;     // the length of each
        const int32_t amplitude = ((len - 8) >> 3 > 1 ? (len - 8) >> 3 : 1) * (1 + (density >> 5));
        uint8_t* buf = line_.data();
        for (int32_t i = 0; i < lines; i++) {
            // In lights 8.8: WLED's amount a frame, scaled by this frame's share of one.
            const int32_t amount = (static_cast<int32_t>(noise_[rows ? static_cast<size_t>(i) * w : static_cast<size_t>(i)]) - 128) * amplitude * static_cast<int32_t>(dt) / static_cast<int32_t>(kFrameMs);
            const int32_t whole = (amount < 0 ? -amount : amount) >> 8;
            const uint8_t frac = static_cast<uint8_t>((amount < 0 ? -amount : amount) & 255);
            // Linear, the textbook sub-light shift: WLED's eased weights move a small fraction at half its size, so a fast render's film would crawl.
            const uint32_t keepB = frac, keepA = 255u - keepB;
            for (int32_t j = 0; j < len; j++) {
                const int32_t a = amount < 0 ? j - whole : j + whole;
                const int32_t b = amount < 0 ? a - 1 : a + 1;
                const RGB ca = sample(rows, i, a, len, w);
                const RGB cb = sample(rows, i, b, len, w);
                uint8_t* o = buf + static_cast<size_t>(j) * 3u;
                o[0] = blend(ca.r, cb.r, keepA, keepB);
                o[1] = blend(ca.g, cb.g, keepA, keepB);
                o[2] = blend(ca.b, cb.b, keepA, keepB);
            }
            for (int32_t j = 0; j < len; j++) {
                const size_t at = rows ? static_cast<size_t>(i) * w + j : static_cast<size_t>(j) * w + i;
                const uint8_t* o = buf + static_cast<size_t>(j) * 3u;
                put(at, RGB{o[0], o[1], o[2]});
            }
        }
    }

    /// Light `k` of line `i`: the image's light inside the line, past its ends the palette color of the noise where it wraps.
    RGB sample(bool rows, int32_t i, int32_t k, int32_t len, lengthType w) const {
        const int32_t wrapped = (k < 0 ? -k : k) % len;
        const size_t at = rows ? static_cast<size_t>(i) * w + wrapped : static_cast<size_t>(wrapped) * w + i;
        if (k >= 0 && k < len) return RGB{image_[at * 3u], image_[at * 3u + 1], image_[at * 3u + 2]};
        return edgeColor(noise_[at]);
    }

    /// `a` and `b` weighted by `wa` and `wb`, which sum to 255, rounded: a light blended with its own color stays exact.
    static uint8_t blend(uint8_t a, uint8_t b, uint32_t wa, uint32_t wb) { return static_cast<uint8_t>((a * wa + b * wb + 127u) / 255u); }

    /// The palette color a noise value brings in.
    static RGB edgeColor(uint8_t n) { return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(static_cast<uint8_t>(~n) * 3u)); }

    /// Store `c` as image light `i`.
    void put(size_t i, RGB c) {
        image_[i * 3u] = c.r;
        image_[i * 3u + 1] = c.g;
        image_[i * 3u + 2] = c.b;
    }

    /// Copy the image to the layer.
    void show(lengthType w, lengthType h) {
        uint8_t* out = buffer();
        const uint8_t cpl = channelsPerLight();
        const size_t lights = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < lights; i++, out += cpl) {
            if (cpl >= 1) out[0] = image_[i * 3u];
            if (cpl >= 2) out[1] = image_[i * 3u + 1];
            if (cpl >= 3) out[2] = image_[i * 3u + 2];
        }
    }

    ScratchBuffer<uint8_t> noise_{*this};   ///< the smoothed noise field, a value a light
    ScratchBuffer<uint8_t> image_{*this};   ///< the film itself, three bytes a light, kept between frames
    ScratchBuffer<uint8_t> line_{*this};    ///< one row or column while it slides
    uint64_t drift_ = 0;                    ///< how far the noise has drifted, in WLED frames' units times kFrameMs, exact at any frame split
    uint32_t lastMs_ = 0;                   ///< the last frame's time
    bool started_ = false;
};

}  // namespace mm
