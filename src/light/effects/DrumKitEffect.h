#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// A drum kit seen from the audience, each drum lighting as it is played: the bass drum on the kick, the snare, the hi-hat with stars over the top, and three rack toms and a floor tom by their pitch, so a drum solo plays across the kit.
/// @card DrumKitEffect.gif
///
/// The kick, the snare and the hi-hat are the audio's bass, mid and treble onsets. The toms are the third-octave bands from 80 to 400 Hz, two bands each: the floor tom at the right, the rack toms over the bass drum, the high one at the left. Each hit glows for `decay` and fades; between hits the kit shows at `idle`.
///
/// @technical
///
/// ## Drawn from the hits' ages
///
/// The effect keeps only when each drum last hit and draws every frame from those ages, so the picture is the same at any frame rate and the state is a few bytes. Each drum sits at a fixed place as a fraction of the width and height and is sized by the shorter side, so the kit keeps its shape on any grid; hits add their colors where drums overlap.
class DrumKitEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// The kit is drawn on a plane; a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// How long a hit glows, in tens of milliseconds.
    uint8_t decay = 30;
    /// How bright the kit shows between hits, 0 for dark.
    uint8_t idle = 24;

    /// Publish how long a hit glows and how bright the kit rests.
    void defineControls() override {
        controls_.addControl("decay", decay, 5, 200);
        controls_.addControl("idle", idle, 0, 128);
    }

    /// Note each drum's hit on the newest frame, then draw the kit from the hits' ages: @xref{drawn-from-the-hits-ages}.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        draw::fill(cv, RGB{0, 0, 0});
        const lengthType w = width(), h = height();
        if (w == 0 || h == 0) return;
        const uint32_t ms = elapsed();
        noteHits(*AudioService::latestFrame(), ms);
        const uint32_t span = static_cast<uint32_t>(decay) * 10u;
        const Palette& pal = *Palettes::active();
        drawDrum(cv, w, h, kKickPlace, colorFromPalette(pal, 0, 255), glow(kKick, ms, span));
        for (uint8_t t = 0; t < kToms; t++)
            drawDrum(cv, w, h, kTomPlaces[t], colorFromPalette(pal, static_cast<uint8_t>(128u + t * 32u), 255), glow(static_cast<uint8_t>(kFirstTom + t), ms, span));
        drawDrum(cv, w, h, kSnarePlace, colorFromPalette(pal, 96, 255), glow(kSnare, ms, span));
        const uint8_t hat = glow(kHat, ms, span / 2);   // a hat is short
        drawDrum(cv, w, h, kHatPlace, RGB{255, 255, 255}, hat);
        drawStars(cv, w, h, hat);
    }

private:
    /// Where a drum sits and how large, in thousandths: its center of the width and the height, its half-width and half-height of the shorter side.
    struct Place { uint16_t x, y, rx, ry; };
    static constexpr Place kKickPlace = {500, 720, 250, 250};    // the bass drum's head, facing the audience
    static constexpr Place kSnarePlace = {220, 560, 110, 40};    // tilted toward the drummer, a flat oval
    static constexpr Place kHatPlace = {110, 380, 110, 15};      // the closed hi-hat's cymbals, a thin bar
    static constexpr uint8_t kToms = 4;                          // two thirds each, from 80 to 400 Hz
    static constexpr Place kTomPlaces[kToms] = {
        {810, 700, 140, 140},   // the floor tom, 80 and 100 Hz, beside the bass drum
        {650, 360, 100, 90},    // the low rack tom, 125 and 160 Hz
        {500, 300, 95, 85},     // the middle rack tom, 200 and 250 Hz
        {350, 360, 90, 80},     // the high rack tom, 315 and 400 Hz
    };
    static constexpr uint8_t kFirstTomThird = 6;   // 80 Hz
    static constexpr uint8_t kSparkles = 12;       // the stars over a hat
    enum Drum : uint8_t { kKick, kSnare, kHat, kFirstTom };
    uint32_t hitMs_[kFirstTom + kToms] = {};       ///< when each drum last hit, on the effect's clock: a handful of drums from the band onsets, not NoteHits' 79 notes, and a linear fade so a drum's hit reads as a strike
    uint8_t heard_ = 0;                            ///< bit d once drum d has hit
    uint32_t hatSeed_ = 0;                         ///< where the last hat's stars fall
    uint16_t lastSeq_ = 0;                         ///< the frame the last hits came from

    /// Each drum a new frame carries a hit for starts its glow now; a frame two ticks read is one hit, told by its `seq`.
    void noteHits(const AudioFrame& f, uint32_t ms) MM_NONBLOCKING {
        if (f.seq == lastSeq_) return;
        lastSeq_ = f.seq;
        const bool hit[kFirstTom] = {f.onsetLow != 0, f.onsetMid != 0, f.onsetHigh != 0};
        for (uint8_t d = 0; d < kFirstTom; d++) if (hit[d]) start(d, ms);
        if (hit[kHat]) hatSeed_ = ms;
        for (uint8_t b = 0; b < 2 * kToms; b++)
            if (hitAt(f.thirdsHits, static_cast<uint8_t>(kFirstTomThird + b))) start(static_cast<uint8_t>(kFirstTom + b / 2), ms);
    }

    void start(uint8_t drum, uint32_t ms) MM_NONBLOCKING {
        hitMs_[drum] = ms;
        heard_ = static_cast<uint8_t>(heard_ | (1u << drum));
    }

    /// A drum's brightness, full on its hit and fading to `idle` over `span`.
    uint8_t glow(uint8_t drum, uint32_t ms, uint32_t span) const MM_NONBLOCKING {
        const uint32_t age = ms - hitMs_[drum];
        if (!(heard_ & (1u << drum)) || span == 0 || age >= span) return idle;
        const uint8_t hit = static_cast<uint8_t>(255u - age * 255u / span);
        return hit > idle ? hit : idle;
    }

    /// A drum as a filled oval at `p` in `color` at `level`, its rim half again as bright, so the drum reads as a drum at rest too.
    static void drawDrum(const draw::Canvas& cv, lengthType w, lengthType h, Place p, RGB color, uint8_t level) MM_NONBLOCKING {
        if (!level) return;
        const int32_t side = w < h ? w : h;
        const int32_t cx = static_cast<int32_t>(p.x) * w / 1000, cy = static_cast<int32_t>(p.y) * h / 1000;
        const int32_t rx = static_cast<int32_t>(p.rx) * side / 1000, ry = static_cast<int32_t>(p.ry) * side / 1000;
        const int32_t ax = rx > 0 ? rx : 1, ay = ry > 0 ? ry : 1;
        const RGB face = scale8(color, level);
        const RGB rim = scale8(color, static_cast<uint8_t>(level > 170 ? 255 : level * 3 / 2));
        for (int32_t dy = -ry; dy <= ry; dy++)
            for (int32_t dx = -rx; dx <= rx; dx++) {
                const int32_t x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                // Inside the oval when (dx/rx)^2 + (dy/ry)^2 <= 1, in integers; the outer band of it is the rim.
                const int64_t q = static_cast<int64_t>(dx) * dx * ay * ay + static_cast<int64_t>(dy) * dy * ax * ax;
                const int64_t edge = static_cast<int64_t>(ax) * ax * ay * ay;
                if (q > edge) continue;
                draw::addPixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, q * 2 > edge ? rim : face);
            }
    }

    /// The hat's stars over the top quarter, the same pixels for one hit.
    void drawStars(const draw::Canvas& cv, lengthType w, lengthType h, uint8_t level) const MM_NONBLOCKING {
        if (level <= idle) return;
        const RGB c = scale8(RGB{255, 255, 255}, level);
        const uint32_t rows = h / 4 > 0 ? h / 4 : 1;
        for (uint8_t i = 0; i < kSparkles; i++) {
            const lengthType x = static_cast<lengthType>(hashInt(hatSeed_, i, 1) % w);
            const lengthType y = static_cast<lengthType>(hashInt(hatSeed_, i, 2) % rows);
            draw::addPixel(cv, {x, y, 0}, c);
        }
    }
};

}  // namespace mm
