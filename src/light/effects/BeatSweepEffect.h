#pragma once

#include "light/effects/AudioEffectBase.h"   // the bands under the sweep, while this effect reads them
#include "light/powerfunctions/fonts.h"       // the beat's text

namespace mm {

/// A bar of light crosses the layout once a beat, or once a bar, over the sixteen bands.
/// @card BeatSweepEffect.gif
///
/// It follows the beat a source states: a deck or Ableton Link over the [MoonModules format](https://moonmodules.org/MoonLight/reference/audio-sync.html#beat), a tracker, or the simulated song. While no beat runs it crosses at its own `tempo`, and in silence the light fades out, so a stopped song goes dark.
///
/// The bands stand behind it as bars, the light takes the color of the band it is over, `downbeat` gives the bar's first beat the palette's other half, and `beatInfo` writes the tempo and the beat in the bar at the top left, "106 3/4".
///
/// @attribution troyhacks
///
/// @technical
///
/// ## Where the light is
///
/// The light's place is the beat's phase, or on `bar` the beat in the bar plus its phase over four, so it lands on the far side as the beat or the bar ends; `bounce` sends every other crossing back. It is computed from the beat each frame rather than stepped, so it moves the same at any frame rate.
class BeatSweepEffect : public AudioEffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// It sweeps across a plane, and a cube takes the plane extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// How long one crossing takes, in option order.
    enum Cycle : uint8_t { kBeat, kBar, kCycleCount };
    /// A crossing per beat or per bar.
    uint8_t cycle    = kBeat;
    /// Every other crossing comes back rather than starting over.
    bool    bounce   = true;
    /// The light's width, 255 a quarter of the layout.
    uint8_t glow     = 64;
    /// How far the light's tail reaches behind it.
    uint8_t trail    = 128;
    /// How bright the bands behind the light stand.
    uint8_t bands    = 96;
    /// The crossings' tempo while no beat runs, in BPM.
    uint8_t tempo    = 120;
    /// The bar's first beat in the palette's other half.
    bool    downbeat = true;
    /// The tempo and the beat in the bar at the top left.
    bool    beatInfo = false;

    /// Publish the cycle, the bounce, the light's glow and tail, the bands' brightness, the tempo with no beat, the downbeat color and the beat's text.
    void defineControls() override {
        static constexpr const char* kCycleOptions[] = {"beat", "bar"};
        controls_.addSelect("cycle", cycle, kCycleOptions, kCycleCount);
        controls_.addControl("bounce", bounce);
        controls_.addControl("glow", glow, 1, 255);
        controls_.addControl("trail", trail, 0, 255);
        controls_.addControl("bands", bands, 0, 255);
        controls_.addControl("tempo", tempo, 40, 200);
        controls_.addControl("downbeat", downbeat);
        controls_.addControl("beatInfo", beatInfo);
    }

    /// Draw the bands, then the light where the beat puts it.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 1 || h < 1) return;
        const AudioFrame* f = AudioService::latestFrame();
        const uint32_t ms = elapsed();
        presence_ = presenceAfter(presence_, f->heard(), ms - lastMs_);
        lastMs_ = ms;
        const BeatNow& beat = AudioService::beatNow();
        const Sweep s = sweepAt(beat, ms);
        const int32_t pos = (static_cast<int32_t>(s.pos) * (w - 1)) / 65535;
        const int32_t half = 1 + (static_cast<int32_t>(w) * glow) / 2040;   // 255 is a quarter of the layout
        const int32_t tail = (static_cast<int32_t>(w) * trail) / 512;
        for (lengthType x = 0; x < w; x++) {
            const uint8_t band = static_cast<uint8_t>((static_cast<uint32_t>(x) * 16u) / static_cast<uint32_t>(w));
            const uint8_t light = scale8(lightAt(x - pos, s.forward, half, tail), presence_);
            const uint8_t hue = static_cast<uint8_t>(band * 16u + (s.downbeat && downbeat ? 128u : 0u));
            drawColumn(cv, x, h, f->bands[band], light, hue);
        }
        if (beatInfo && beat.live()) {
            char info[12];
            draw::text(cv, fonts::kFont4x6, beatText(beat, info, sizeof(info)), 0, 0, RGB{255, 255, 255});
        }
    }

    /// The beat as the top left shows it: the tempo and the beat in the bar, "106 3/4", or the tempo alone while the bar is unknown.
    static const char* beatText(const BeatNow& b, char* out, size_t n) {
        const unsigned bpm = b.bpm();
        if (b.inBar) mm::formatTo(out, n, "%u %u/4", bpm, static_cast<unsigned>(b.inBar));
        else mm::formatTo(out, n, "%u", bpm);
        return out;
    }

    /// Where the light is: `pos` 0..65535 across the layout, the way it travels, and whether this is the bar's first beat.
    struct Sweep { uint16_t pos; bool forward; bool downbeat; };

    /// The light's place for `beat` at `ms`, the effect's own tempo standing in while no beat runs. @xref{where-the-light-is}
    Sweep sweepAt(const BeatNow& beat, uint32_t ms) const {
        uint32_t crossing;   // which crossing this is
        uint32_t through;    // how far through it, 0..65535
        if (beat.live()) {
            const uint32_t inBar = beat.inBar ? beat.inBar - 1u : beat.count % 4u;
            crossing = cycle == kBar ? beat.count / 4u : beat.count;
            through = cycle == kBar ? (inBar * 256u + beat.phase) * 64u : beat.phase * 256u;
        } else {
            const uint64_t beats256 = static_cast<uint64_t>(ms) * tempo * 256u / 60000u;   // beats since the start, in 1/256ths
            const uint32_t per = cycle == kBar ? 1024u : 256u;
            crossing = static_cast<uint32_t>(beats256 / per);
            through = static_cast<uint32_t>((beats256 % per) * 65536u / per);
        }
        const bool forward = !bounce || (crossing % 2u) == 0;
        const uint16_t at = static_cast<uint16_t>(through > 65535u ? 65535u : through);
        return {forward ? at : static_cast<uint16_t>(65535u - at), forward, beat.live() && beat.inBar == 1};
    }

    /// The light's brightness `d` lights from its center, full across `half`, fading over `tail` behind it as it travels.
    static uint8_t lightAt(int32_t d, bool forward, int32_t half, int32_t tail) {
        const int32_t behind = forward ? -d : d;
        if ((d < 0 ? -d : d) <= half) return 255;
        if (behind <= half || tail <= 0 || behind > half + tail) return 0;
        return static_cast<uint8_t>(255 - ((behind - half) * 255) / (tail + 1));
    }

    /// How lit the light is after `dtMs` more of sound or silence: back within about 120 ms when sound returns, out over about 500 ms in silence.
    static uint8_t presenceAfter(uint8_t presence, bool heard, uint32_t dtMs) {
        const uint32_t step = (dtMs > 500u ? 500u : dtMs) * 255u / (heard ? 120u : 500u);
        return heard ? static_cast<uint8_t>(presence + step > 255u ? 255u : presence + step)
                     : static_cast<uint8_t>(presence > step ? presence - step : 0u);
    }

private:
    /// One column: its band as a bar from the floor (or a glow on a single row), the light over it in the palette at `hue`.
    void drawColumn(const draw::Canvas& cv, lengthType x, lengthType h, uint8_t level, uint8_t light, uint8_t hue) {
        const lengthType rows = static_cast<lengthType>((static_cast<uint32_t>(level) * h) / 255u);   // 0 for a silent band, every row at 255
        for (lengthType y = 0; y < h; y++) {
            const bool lit = h == 1 || h - 1 - y < rows;
            const uint8_t under = lit ? scale8(h == 1 ? level : 255, bands) : 0;
            const uint8_t bri = light > under ? light : under;
            draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), hue, bri));
        }
    }

    uint8_t  presence_ = 0;   ///< how lit the light is, following sound and silence
    uint32_t lastMs_ = 0;     ///< the last frame's time, which the fade steps from
};

}  // namespace mm
