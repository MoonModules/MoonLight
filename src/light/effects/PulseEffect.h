#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

/// Expanding shells from a drifting origin, one per beat, alive with or without sound.
/// @card PulseEffect.gif
///
/// A pulse is a thin shell that grows from a point and dims as it travels.
/// The same shell is a wave on a strip, a ring on a panel and a sphere in a volume.
/// A shell is a distance, and every layout has distances.
/// Between sounds the shells keep arriving on the idle clock, so a silent room still moves.
///
/// @moreinfo
///
/// ## Why this is the default effect
///
/// A first boot shows three things at once: the lights work, the device runs, and it hears the room.
/// A dense field shows the first and hides the other two, since every light is already busy.
/// A shell leaves most of the layout dark, so one beat is unmistakable, and the drifting origin proves the device runs before any sound arrives.
///
/// ## One shell, three dimensions
///
/// The radius is a Euclidean distance over whatever axes the layout has, so the code branches on nothing.
/// A 300-light strip has one axis, where a shell becomes a pair of fronts running outward from the origin.
/// A panel gives a ring and a volume gives a sphere, from the same three subtractions.
///
/// ## One set of defaults fits every layout
///
/// Both the travel and the shell's width are shares of the layout's own diagonal rather than counts of lights.
/// A fixed lights-a-second speed crosses a 16x16 panel in half a second and a 300-light strip in seven, so one set of defaults cannot suit both.
/// As a share, a shell takes the same time to cross whatever it is on, and roughly three are alive at any moment on every geometry.
///
/// ## The square root is paid per shell, not per light
///
/// Comparing a radius against a distance and comparing their squares decide the same thing, so the shell test runs in squared space.
/// The root is then taken only for a light that some shell already contains, which is a few percent of the grid.
/// That matters on an in-order pipeline, where a 16-iteration root is 16 branch stalls a light rather than a speculated handful.
///
/// ## What drives a pulse, and what will drive it later
///
/// Three inputs decide everything: when a pulse fires, where it starts, and what color it takes.
/// Audio fills all three today, where an onset fires it, the drift places it, and the dominant frequency colors it.
/// A sensor added later fills the same three, which is why `emit` takes exactly those arguments.
class PulseEffect : public EffectBase {
public:
    /// Catalog tags: a MoonLight original that reacts to sound.
    const char* tags() const override { return "💫🎶"; }
    /// A shell is a distance, which every layout has, so all three axes are used.
    Dim dimensions() const override { return Dim::D3; }

    /// Pulses per minute while nothing is heard, where 0 waits for sound alone.
    uint8_t bpm = 40;
    /// How fast a shell crosses the layout, whatever its size, where 255 crosses it in a second.
    uint8_t speed = 60;
    /// The shell's width as a share of the layout, so it stays a shell at any size.
    uint8_t thickness = 40;
    /// How strongly sound fires a pulse, where 0 leaves the idle clock alone in charge.
    uint8_t audioGain = 200;
    /// How far the origin wanders from the center, where 0 pins it there.
    uint8_t drift = 120;

    /// Publish the rhythm, the shell's shape, and how much the room drives it.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 200);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("thickness", thickness, 1, 255);
        controls_.addControl("audioGain", audioGain, 0, 255);
        controls_.addControl("drift", drift, 0, 255);
    }

    /// Dim what is there, fire a pulse where one is due, then draw every live shell.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = cv.dims.x, h = cv.dims.y, d = cv.dims.z;
        const uint32_t now = elapsed();

        // The wake, by half-life rather than per frame, so the trail is the same at any framerate.
        const uint32_t dt = started_ ? now - lastMs_ : 0u;
        started_ = true;
        lastMs_ = now;
        draw::decay(cv, kWakeHalfLifeMs, dt);

        // When: a hit fires at once, and the idle clock fires while the room is quiet.
        const AudioFrame* f = AudioService::latestFrame();
        const bool heard = f != nullptr && f->onset != 0 && audioGain > 0;
        if (heard && !onsetSeen_) {
            // Strength sets the shell's brightness, so a loud hit reads as a loud one.
            const uint16_t lift = (static_cast<uint16_t>(f->onset) * audioGain) / 255;
            // What color: the dominant frequency, so a bass note and a cymbal differ on sight.
            emit(now, static_cast<uint8_t>(155 + (lift > 100 ? 100 : lift)), hueFromPitch(f->peakHz));
            lastIdle_ = now;   // a beat is a pulse, so the quiet clock waits a full interval after it
        } else if (bpm > 0 && now - lastIdle_ >= 60000u / bpm) {
            lastIdle_ = now;
            emit(now, 150, static_cast<uint8_t>(now / 64));   // a slow walk through the palette
        }
        onsetSeen_ = heard;

        // Where: three oscillators whose periods share no factor, so the path never repeats.
        const Coord3D origin{axisCenter(w, now, 9001, 0),
                             axisCenter(h, now, 11003, 2100),
                             axisCenter(d, now, 13007, 4200)};

        // The furthest a shell travels before it has left the layout entirely.
        const int32_t reach = maxRadius(w, h, d);
        const Palette& pal = *Palettes::active();

        // Shares of `reach` rather than counts of lights: @xref{one-set-of-defaults-fits-every-layout}.
        const int32_t half = 1 + (reach * thickness) / 1024;   // at least 1, so a shell is never empty
        const int32_t crossMs = (256 * 1000) / (speed + 1);    // to cross the whole layout, at any size

        // Hoisted: a radius is the same for every light, and the bounds are squared for the test below.
        int32_t radius[kPulses], loSq[kPulses], hiSq[kPulses];
        bool anyLive = false;
        for (uint8_t i = 0; i < kPulses; i++) {
            Pulse& p = pulses_[i];
            if (!p.live) { radius[i] = -1; continue; }
            radius[i] = (static_cast<int32_t>(now - p.born) * reach) / crossMs;
            if (radius[i] > reach + half) { p.live = false; radius[i] = -1; continue; }   // it has left
            const int32_t lo = radius[i] - half > 0 ? radius[i] - half : 0;
            const int32_t hi = radius[i] + half;
            loSq[i] = lo * lo;
            hiSq[i] = hi * hi;
            anyLive = true;
        }
        if (!anyLive) return;   // nothing to draw, so the whole grid walk is skipped

        for (lengthType z = 0; z < d; z++)
        for (lengthType y = 0; y < h; y++)
        for (lengthType x = 0; x < w; x++) {
            const int32_t dx = x - origin.x, dy = y - origin.y, dz = z - origin.z;
            // Squared, so the root below is paid only inside a shell: @xref{the-square-root-is-paid-per-shell-not-per-light}.
            const int32_t distSq = dx * dx + dy * dy + dz * dz;

            int32_t lit = 0;
            uint8_t hue = 0;
            int32_t r = -1;   // resolved lazily, at most once, and only inside a shell
            for (uint8_t i = 0; i < kPulses; i++) {
                if (radius[i] < 0 || distSq < loSq[i] || distSq > hiSq[i]) continue;
                if (r < 0) r = isqrt32(static_cast<uint32_t>(distSq));
                const int32_t off = r > radius[i] ? r - radius[i] : radius[i] - r;
                if (off > half) continue;
                // Brightest at the shell's middle, and dimmer the further the shell has travelled.
                const int32_t edge = 255 - (off * 255) / (half + 1);
                const int32_t travel = 255 - (radius[i] * 255) / (reach + 1);
                const int32_t v = (edge * travel * pulses_[i].strength) / 65025;
                if (v > lit) { lit = v; hue = pulses_[i].hue; }
            }
            if (lit <= 0) continue;
            // Added rather than written, so two shells crossing brighten where they meet.
            draw::addPixel(cv, {x, y, z},
                           colorFromPalette(pal, hue, static_cast<uint8_t>(lit > 255 ? 255 : lit)));
        }
    }

private:
    /// One expanding shell: when it started, how bright it is, and where in the palette it sits.
    struct Pulse {
        uint32_t born = 0;
        uint8_t  strength = 0;
        uint8_t  hue = 0;
        bool     live = false;
    };
    /// Enough for a fast passage to overlap, few enough that the inner loop stays cheap.
    static constexpr uint8_t kPulses = 6;
    /// How long the wake takes to halve, which is what a per-frame fade of 40 came to at 60 fps.
    static constexpr uint32_t kWakeHalfLifeMs = 70;

    Pulse    pulses_[kPulses];
    uint8_t  next_ = 0;          ///< round-robin, so the oldest shell is the one replaced
    uint32_t lastIdle_ = 0;      ///< when the idle clock last fired
    uint32_t lastMs_ = 0;        ///< the previous frame's time, for the wake's half-life
    bool     started_ = false;   ///< the first frame has no previous one to measure against
    bool     onsetSeen_ = false; ///< an onset is a rising edge rather than a level

    /// Start a shell, taking the oldest slot when every one is busy.
    void emit(uint32_t now, uint8_t strength, uint8_t hue) {
        Pulse& p = pulses_[next_];
        p.born = now;
        p.strength = strength;
        p.hue = hue;
        p.live = true;
        next_ = static_cast<uint8_t>((next_ + 1) % kPulses);
    }

    /// The origin on one axis: the middle, plus a slow triangle wave `drift` scales.
    lengthType axisCenter(lengthType extent, uint32_t now, uint32_t period, uint32_t phase) const {
        const int32_t mid = extent / 2;
        if (extent <= 2 || drift == 0) return static_cast<lengthType>(mid);
        const uint32_t t = (now + phase) % (period * 2);
        // -128..127 over the full period, which is the wave before the amplitude is applied.
        const int32_t tri = static_cast<int32_t>(t < period ? t : period * 2 - t);
        const int32_t wave = (tri * 256) / static_cast<int32_t>(period) - 128;
        // Three quarters of the half-extent at full drift, so a shell keeps room on every side.
        const int32_t amp = (mid * 3 * drift) / (4 * 255);
        return static_cast<lengthType>(mid + (wave * amp) / 128);
    }

    /// The longest distance any shell travels, which is the layout's diagonal.
    static int32_t maxRadius(lengthType w, lengthType h, lengthType d) {
        const int32_t mx = w, my = h, mz = d;
        const int32_t diag = isqrt32(static_cast<uint32_t>(mx * mx + my * my + mz * mz));
        return diag > 1 ? diag : 1;   // a single light still needs a non-zero divisor below
    }

    /// A hue from a frequency, logarithmic because pitch is, so an octave is one step.
    static uint8_t hueFromPitch(uint16_t hz) {
        uint8_t octaves = 0;
        uint32_t v = hz;
        while (v > 40 && octaves < 8) { v >>= 1; octaves++; }
        return static_cast<uint8_t>(octaves * 32);
    }

    /// Integer square root, so the per-light distance costs no float.
    static int32_t isqrt32(uint32_t v) {
        uint32_t r = 0, bit = 1u << 30;
        while (bit > v) bit >>= 2;
        while (bit != 0) {
            if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }
        return static_cast<int32_t>(r);
    }
};

}  // namespace mm
