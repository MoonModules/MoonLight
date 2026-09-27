#pragma once
// Author: MoonLight original

#include "core/services/AudioService.h"   // latestFrame: the spectrum the audio-reactive mode reads
#include "core/util/math16.h"         // sin16, BeatPhase: the sweep clocks
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: aims a rig of moving heads, with formations and an audio-reactive mode.
/// @card MovingHeadEffect.gif
///
/// One head sweeping a sine is a demo, and a rig of them is a show.
/// `formation` is the relationship between them: one sweep, a per-head phase and direction.
/// One control changes the rig's whole character without touching speed or range.
///
/// @moreinfo
///
/// ## Pan and tilt run at different rates
///
/// Two sines at different rates trace a Lissajous path.
/// It closes into a loop when the rates share a small ratio, and drifts when they do not.
/// That drift keeps a long show from looking like a metronome.
///
/// ## The same effect runs on a strip
///
/// Everything is written through EffectBase's role setters, which no-op on an absent channel.
/// So this effect on an LED strip paints its color pattern and moves nothing.
/// An effect says what it wants, and the fixture's preset decides what can be expressed.
class MovingHeadEffect : public EffectBase {
public:
    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎶🎯"; }
    /// D3: every head has its own aim, so the effect places all of them rather than being extruded.
    Dim dimensions() const override { return Dim::D3; }

    /// How the heads relate to each other, which is what the audience reads.
    enum : uint8_t { kFan = 0, kMirror, kChase, kCross, kUnison, kFormationCount };
    /// The formation names, in enum order, for the select control.
    static constexpr const char* kFormationNames[] = {"fan", "mirror", "chase", "cross", "unison"};

    /// The selected formation.
    uint8_t formation = kFan;

    /// Pan sweep rate in BPM, so 60 is one sweep a second.
    uint8_t panBpm  = 6;
    /// Tilt sweep rate in BPM, deliberately unequal to `panBpm`.
    uint8_t tiltBpm = 9;
    /// How much pan travel to use: a band around center, since full pan aims away from the audience.
    uint8_t panRange  = 128;
    /// How much tilt travel to use.
    uint8_t tiltRange = 96;
    /// Where the pan sweep is centered, 128 being the fixture's middle.
    uint8_t panCenter  = 128;
    /// Where the tilt sweep is centered.
    uint8_t tiltCenter = 128;

    /// The gobo pattern, a raw fixture byte: only the fixture's manual says what a value selects.
    uint8_t gobo   = 0;
    /// The gobo or prism spin, a raw fixture byte.
    uint8_t rotate = 0;
    /// Roll a new gobo on a bass hit, with a hold so a busy track cannot strobe through patterns.
    bool goboOnBeat = false;

    /// Move and light with the music: the beam widens with the room and each head takes its own band.
    bool audioReactive = false;

    /// Publish the formation, the sweep, the beam wheels and the audio switch.
    void defineControls() override {
        controls_.addSelect("formation", formation, kFormationNames, kFormationCount);
        controls_.addControl("panBpm", panBpm, 1, 120);
        controls_.addControl("tiltBpm", tiltBpm, 1, 120);
        controls_.addControl("panRange", panRange, 0, 255);
        controls_.addControl("tiltRange", tiltRange, 0, 255);
        controls_.addControl("panCenter", panCenter, 0, 255);
        controls_.addControl("tiltCenter", tiltCenter, 0, 255);
        controls_.addControl("audioReactive", audioReactive);
        // Hidden where a head lacks the wheels, since the writes would be no-ops.
        controls_.addControl("gobo", gobo, 0, 255);
        controls_.addControl("rotate", rotate, 0, 255);
        controls_.addControl("goboOnBeat", goboOnBeat);
        const bool noBeam = !hasBeam();
        const uint8_t last = controls_.count();
        controls_.setHidden(last - 3, noBeam);   // gobo
        controls_.setHidden(last - 2, noBeam);   // rotate
        controls_.setHidden(last - 1, noBeam);   // goboOnBeat
    }

    /// Reset both sweep clocks and return the gobo to the user's own choice.
    void prepare() override {
        pan_ = BeatPhase{};
        tilt_ = BeatPhase{};
        beatDecay_ = 0;
        // From the slider, so a rig returns to the chosen pattern rather than a rolled one.
        goboNow_ = gobo;
        goboHoldUntil_ = 0;
        beatCount_ = 0;
    }

    /// Aim every head from its place in the formation, and color it from the palette.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const nrOfLightsType n = nrOfLights();
        if (n == 0) return;
        // Read at frame time (the framework's rule): a resize between frames must not be missed.
        const uint32_t w = width() ? width() : 1;
        const uint32_t h = height() ? height() : 1;
        const uint32_t d = depth() ? depth() : 1;

        // Own the background: an effect must not inherit the previous frame's picture.
        draw::fill(cv, RGB{0, 0, 0});

        pan_.advanceTo(elapsed(), panBpm);
        tilt_.advanceTo(elapsed(), tiltBpm);

        // phase(65536) is the angle16 form sin16 takes; truncating to uint16 is the free wrap.
        const uint16_t panPhase  = static_cast<uint16_t>(pan_.phase(65536));
        const uint16_t tiltPhase = static_cast<uint16_t>(tilt_.phase(65536));

        // Once a frame rather than per head, and either feature needs it.
        const AudioFrame* audio = (audioReactive || goboOnBeat) ? AudioService::latestFrame() : nullptr;
        const bool live = audio && audio->levelSmoothed >= kSilence;

        // A beat widens the sweep and decays over about 20 frames, since one frame is invisible.
        const bool beat = live && audio->level > audio->levelSmoothed + kBeatMargin;
        if (beat) beatDecay_ = 255;
        else if (beatDecay_ > kBeatDecayStep) beatDecay_ = static_cast<uint8_t>(beatDecay_ - kBeatDecayStep);
        else beatDecay_ = 0;

        // A beat rolls a new gobo after a hold, or a kick changes it four times a second.
        if (goboOnBeat) {
            if (beat && static_cast<int32_t>(elapsed() - goboHoldUntil_) >= 0) {
                // hashInt of a beat counter, not a stream RNG: two devices must land on one pattern.
                beatCount_++;
                // Clamped, not wrapped: a high `gobo` plus a slot offset would wrap to a low wheel position.
                const uint16_t slot = static_cast<uint16_t>(gobo) + (hashInt(beatCount_) & 0xE0);
                goboNow_ = static_cast<uint8_t>(slot > 255 ? 255 : slot);   // 8 coarse slots
                // A deadline in milliseconds, not frames, or the hold shortens as the device speeds up.
                goboHoldUntil_ = elapsed() + kGoboHoldMs;
            }
        } else {
            goboNow_ = gobo;      // the slider IS the value when the beat roll is off
            goboHoldUntil_ = 0;
        }

        // Loud music opens the beam and silence holds the rig's aim rather than drifting it.
        const uint16_t loud = live ? audio->levelSmoothed : 0;
        const uint8_t swingPan  = audioReactive ? scaleToLevel(panRange, loud) : panRange;
        const uint8_t swingTilt = audioReactive ? scaleToLevel(tiltRange, loud) : tiltRange;
        const bool frozen = audioReactive && !live;

        for (nrOfLightsType i = 0; i < n; i++) {
            const uint32_t hx = i % w, hy = (i / w) % h, hz = i / (w * h);
            const Formation f = shape(hx, hy, hz, w, h, d);


            // Written every frame, so a changed value takes effect at once.
            setGobo(i, goboNow_);
            setRotate(i, rotate);

            // A frozen rig keeps its last aim: writing from a stopped phase would snap it back.
            if (!frozen) {
                const int32_t p = sin16(static_cast<uint16_t>(panPhase + f.phase));
                const int32_t t = sin16(static_cast<uint16_t>(tiltPhase + f.phase));
                setPan(i, axis(p * f.dir, panCenter, boost(swingPan)));
                setTilt(i, axis(t, tiltCenter, boost(swingTilt)));
            }

            // A palette walk moving with the sweep, so color and motion read as one gesture.
            const uint8_t hue = static_cast<uint8_t>((panPhase >> 8) + (f.phase >> 8));
            uint8_t bright = 255;
            if (audioReactive) {
                const uint8_t band = static_cast<uint8_t>((static_cast<uint32_t>(i) * 16u) / (n ? n : 1));
                const uint8_t mag = audio ? audio->bands[band > 15 ? 15 : band] : 0;
                // A floor keeps a quiet head visible, and the beat lifts the whole rig at once.
                const uint16_t lifted = static_cast<uint16_t>(kDimFloor + (mag * 3u) / 4u + beatDecay_ / 4u);
                bright = static_cast<uint8_t>(lifted > 255 ? 255 : lifted);
            }
            // pixel, not splat: a fixture is one light, and splat would spread it across neighbors.
            draw::pixel(cv, {static_cast<lengthType>(hx),
                             static_cast<lengthType>(hy),
                             static_cast<lengthType>(hz)},
                        colorFromPalette(*Palettes::active(), hue, bright));
        }
    }

private:
    static constexpr uint16_t kSilence       = 8;   ///< below this the room is quiet, not playing
    static constexpr uint16_t kBeatMargin    = 8;   ///< raw over smoothed is a transient
    static constexpr uint8_t  kBeatDecayStep = 12;  ///< about 20 frames from a kick back to rest
    static constexpr uint8_t  kDimFloor      = 40;  ///< a quiet band still shows its head
    /// How long a rolled gobo stays put: long enough to read, short enough to answer the music.
    static constexpr uint32_t kGoboHoldMs = 2000;

    /// One head's place in the formation: a phase offset into the sweep, and a direction.
    struct Formation { uint16_t phase; int32_t dir; };

    /// One head's place in its formation, from its position so a mirror splits the truss as it looks.
    Formation shape(uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, uint32_t d) const {
        // A diagonal walk over all three axes, so a 3D array ripples like a volume.
        const uint32_t span = w + h + d;
        const uint16_t spread = span > 3
            ? static_cast<uint16_t>((static_cast<uint64_t>(x + y + z) * 65536u) / span) : 0;
        switch (formation) {
            case kMirror:
                // Split on the axis the rig runs along, or a 1 x 1 x N rig collapses to unison.
                return {0, (w > 1 ? (x < w / 2) : h > 1 ? (y < h / 2) : (z < d / 2)) ? 1 : -1};
            case kChase:
                // A wave traveling across the rig: the same sweep, delayed head by head.
                return {spread, 1};
            case kCross:
                // Checkerboard parity of the position, so alternate heads oppose along every axis.
                return {0, ((x + y + z) & 1u) ? -1 : 1};
            case kUnison:
                // Every head as one: the reference the other formations read against.
                return {0, 1};
            case kFan:
            default:
                // Neighbors differ by half a chase's spread, so the beams open like a hand.
                return {static_cast<uint16_t>(spread / 2), 1};
        }
    }

    /// Widen the sweep on a beat, by up to half again, so a beat moves the rig.
    uint8_t boost(uint8_t range) const {
        if (!audioReactive || beatDecay_ == 0) return range;
        const uint16_t wider = static_cast<uint16_t>(range + (range * beatDecay_) / 512u);
        return static_cast<uint8_t>(wider > 255 ? 255 : wider);
    }

    /// Scale a range by the room's loudness, with a floor so a quiet passage still moves.
    static uint8_t scaleToLevel(uint8_t range, uint16_t level) {
        const uint16_t l = level > 255 ? 255 : level;
        const uint16_t scaled = static_cast<uint16_t>(range / 4u + (range * l) / 340u);
        return static_cast<uint8_t>(scaled > range ? range : scaled);
    }

    /// Map a sine onto a DMX byte around `center`, clamped: a wrap is a full-speed swing to the opposite stop.
    static uint8_t axis(int32_t wave, uint8_t center, uint8_t range) {
        const int32_t swing = (wave * range) / 65536;
        int32_t v = static_cast<int32_t>(center) + swing;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return static_cast<uint8_t>(v);
    }

    BeatPhase pan_;                ///< the pan sweep clock
    BeatPhase tilt_;               ///< the tilt sweep clock, at its own rate
    uint8_t   beatDecay_ = 0;      ///< how far the last beat has decayed
    /// The gobo written, which is the slider until a beat rolls it, held separate so the slider stands.
    uint8_t   goboNow_   = 0;
    uint32_t goboHoldUntil_ = 0;   ///< elapsed() past which a new gobo may be rolled
    uint32_t  beatCount_ = 0;      ///< hashed for the roll, so every device on one track agrees
};

} // namespace mm
