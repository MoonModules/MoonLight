#pragma once

#include "light/effects/PianoNotesBase.h"
#include "light/powerfunctions/boids.h"         // the steering and the trail

#if MM_HEAVY_COMPUTE   // only where the SoC declares a hardware FPU, for the reason boids.h carries

namespace mm {

/// A flock with a boid for each piano key: played keys glow and grow, the drums flash, the hottest key hunts, and now and then the flock becomes a spectrum.
/// @card MoonFlockEffect.gif
///
/// Each of the 79 boids, A0 to D#7, takes its key's pitch class.
///
/// Compare with @ref FlockEffect, the flock in the palette.
///
/// @attribution James Hutchinson, Claude, Craig Reynolds, Daniel Shiffman, Jason Coon
///
/// @guide
///
/// ## Heat and the predator
///
/// A key played heats its boid, drawn bigger and brighter, and the boid cools again as the key rests. The key heated past the threshold becomes the one predator: a disc with a mouth, two lights in radius at the least, swelling, brightening and opening wide with the bass.
///
/// ## The drums
///
/// A drum hit flashes the boids of the keys in its range, a harder hit bigger and longer: the kick to C#3, the snare to C#7, the hats above. Only the keys played heat a boid, so only they make the predator.
///
/// ## The keyboard
///
/// Now and then the boids fly to their keys along an edge or around a ring and become a spectrum, each bar as tall as its key's band is loud. After the hold a bass hit, or a longer wait, releases them: each bar shrinks back to its boid at the flock's pace, and the boids fly off.
///
/// @technical
///
/// ## On Flock's boids
///
/// The steering is Craig Reynolds' boids in [boids.h](../../../src/light/powerfunctions/boids.h), the same as @ref FlockEffect. A key's heat cools by half-life on the effect's clock, so `fade` is the same at any `speed`; a hot boid sized to a grid of about 40 lights or less stays one light and only brightens.
///
/// The predator's slot takes `trigger` to enter and half of it to leave, so a key at the threshold holds its place. The keyboard forms by Reynolds' arrival, with the force and slowing distance of Aurora's Boid.
///
/// Boids move and bars shrink only in steps, while the timers run on the effect's clock from the first tick, so `speed` 0 holds the picture and the timers run on.
///
/// ## The spectrum
///
/// The bars read the third-octave bands, the bars the Spectrum effect draws, since the semitone bank behind the keys answers slower and stops at 2.5 kHz. A third spans four semitones, so about four neighboring keys share a band.
///
/// A frame that carries only the sixteen bands, from audio sync, draws from those, each a third. A bar rises with its band at once and falls with a 150 ms half-life, as a meter does. The drums split the keys by the same bands, as the core's `rangeOfThird` does.
class MoonFlockEffect : public PianoNotesBase {
public:
    /// Catalog tags: MoonLight origin, and reacting to which notes are playing.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The flock flying, forming the keyboard, holding it as a spectrum, or releasing it.
    enum Phase : uint8_t { kFlying, kForming, kHolding, kReleasing };
    /// Where the keyboard forms: a random edge or the ring each time, or one.
    enum Dock : uint8_t { kDockRandom, kDockBottom, kDockTop, kDockLeft, kDockRight, kDockRing, kDockCount };

    /// How many lights wide a hot boid is drawn, where 0 sizes it to the grid.
    uint8_t size = 0;
    /// Steps a second, where 60 is the reference and 0 holds the picture.
    uint8_t speed = boids::kReferenceHz;
    /// How much loudness speeds the flock, up to double `speed`.
    uint8_t audioGain = 200;
    /// How much heat a key gains each time it is played, by how loud.
    uint8_t growth = 64;
    /// Seconds a key's heat takes to halve.
    uint8_t fade = 3;
    /// The heat at which a key's boid becomes the predator.
    uint8_t trigger = 192;
    /// How bright drum hits flash their keys, and the bass the predator.
    /// @guide At 0 the boids keep their own glow and the predator its size; a kick still releases the keyboard.
    uint8_t onsetGlow = 200;
    /// About how many seconds the flock flies before forming the keyboard.
    /// @guide Each wait is drawn within a quarter either side; at 0 the flock flies on.
    uint16_t formEvery = 330;
    /// Seconds the finished keyboard holds before a bass hit may release it.
    uint8_t hold = 8;
    /// Where the keyboard forms: random, an edge, or a ring.
    uint8_t dock = kDockRandom;

    /// Publish how big and how fast the flock flies, which notes count, the heat, the drums and the keyboard.
    void defineControls() override {
        static constexpr const char* kDockOptions[] = {"random", "bottom", "top", "left", "right", "ring"};
        controls_.addControl("size", size, 0, boids::kMaxSize);
        controls_.addControl("speed", speed, 0, 255);
        controls_.addControl("audioGain", audioGain, 0, 255);
        addNotesSelect();
        controls_.addControl("growth", growth, 1, 255);
        controls_.addControl("fade", fade, 1, 60);
        controls_.addControl("trigger", trigger, 1, 255);
        controls_.addControl("onsetGlow", onsetGlow, 0, 255);
        controls_.addControl("formEvery", formEvery, 0, 3600);
        controls_.addControl("hold", hold, 0, 120);
        controls_.addSelect("dock", dock, kDockOptions, kDockCount);
    }

    /// What the effect knows of one key while it runs, held from the prepare so a disabled effect holds none of it.
    struct KeyState {
        float heat;        ///< the key's heat, 0 to 255
        float spectrum;    ///< the key's band as its bar shows it, rising at once and falling as a meter
        float barLights;   ///< the bar's length as the keyboard releases, in lights
        uint8_t third;     ///< the third-octave band the key sounds in
        bool placed;       ///< whether its boid has landed on its place this keyboard
    };

    /// Hold each key's state, size and clear the trail, size the flock, seed every boid, and find each key's band.
    void prepare() override {
        PianoNotesBase::prepare();
        if (!keys_.resize(kAudioNoteKeys)) {
            setStatus(boids::kNoMemory, Severity::Warning);
            return;
        }
        if (!flock_.prepare(*this, width(), height(), channelsPerLight(), kAudioNoteKeys)) return;
        gridW_ = std::max<lengthType>(1, width());
        gridH_ = std::max<lengthType>(1, height());
        rng_ = Random8{boids::kSeed};                      // every prepare starts the same flight
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            boids::seed(flock_.boids[k], rng_);
            keys_[k] = KeyState{0.0f, 0.0f, 0.0f, thirdOf(k), false};
        }
        std::fill_n(glow_, kRanges, 0.0f);
        std::fill_n(glowHalfLifeMs_, kRanges, kOnsetHalfLifeMs);
        hands_ = hands::PianoHands{};
        hunter_ = boids::kNone;
        heardSeq_ = AudioService::latestFrame()->seq;
        phase_ = kFlying;
        edge_ = kDockBottom;
        clockStarted_ = false;   // the timers start on the first tick, as the step clock does: the effect's clock starts there
        clock_.reset();
    }

    /// Hear the frame, move the phases on, run the steps this tick owes within its time, copy the trail's colors to the layer, and draw the keyboard and the flock over them as they are now.
    void tick() MM_NONBLOCKING override {
        const std::optional<boids::Plane> trail = flock_.plane(width(), height(), channelsPerLight());
        if (!trail || !keys_) return;   // a resize outran the prepare, or memory ran out: the frame waits for it
        gridW_ = std::max<lengthType>(1, trail->w());   // the grid this tick draws on, read each tick
        gridH_ = std::max<lengthType>(1, trail->h());

        const uint32_t now = elapsed();
        if (!clockStarted_) {
            clockStarted_ = true;
            cooledMs_ = now;
            flyFor(now);
        }
        listen(now);
        pacePhase(now);

        boids::runSteps(clock_.steps(now, boids::pace(speed, audioGain)), [&] {
            trail->fade();
            if (phase_ == kFlying) fly(*trail);
            else if (phase_ == kForming) form(*trail);
            else if (phase_ == kReleasing) shrinkBars(*trail, now);
        });

        const boids::Plane layer{canvas()};
        trail->copyColorsTo(layer.cv);
        if (phase_ != kFlying) bars(layer);
        heads(layer);
    }

    /// The boid that is the predator, or 255 for none, for tests.
    uint8_t hunterForTest() const { return hunter_; }
    /// The flock's `Phase`, for tests.
    uint8_t phaseForTest() const { return phase_; }
    /// Where the keyboard forms this time, a `Dock` other than random, for tests.
    uint8_t edgeForTest() const { return edge_; }
    /// Key `k`'s heat, for tests.
    float heatForTest(uint8_t k) const { return keys_[k].heat; }
    /// Key `k`'s pitch class across the palette, for tests.
    static uint8_t pitchHueForTest(uint8_t k) { return pitchHue(k); }
    /// The third-octave band key `k` sounds in, for tests.
    static uint8_t thirdForTest(uint8_t k) { return thirdOf(k); }
    /// A drum range's glow, 0 the kick's, 1 the snare's, 2 the hats', for tests.
    float glowForTest(uint8_t range) const { return glow_[range]; }
    /// The drum range key `k` lives in, for tests.
    static uint8_t rangeOfForTest(uint8_t k) { return rangeOfThird(thirdOf(k)); }
    /// How many boids stand on their places, for tests.
    uint8_t placedForTest() const {
        uint8_t n = 0;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) n += placed(k);
        return n;
    }

private:
    /// MIDI's number for the first of the notes, A0, which puts each key in its pitch class.
    static constexpr uint8_t kFirstMidi = 21;
    /// How bright a boid whose key is cold glows, out of 255, so the resting flock reads as lit and the played keys stand out above it.
    static constexpr uint8_t kColdLevel = 96;
    /// The least a strike counts as, out of 255, so a soft note the core keeps still shows.
    static constexpr uint8_t kStrikeFloor = 64;
    /// The drums' ranges: the kick's, the snare's and the hats', as the core's `rangeOfThird` numbers them.
    enum Range : uint8_t { kLow, kMid, kHigh, kRanges };
    /// How long a faint drum hit's glow takes to halve, in milliseconds: a flash rather than a glow that lingers.
    static constexpr uint32_t kOnsetHalfLifeMs = 100;
    /// A full-strength hit's glow lasts this many times longer, so the energy of a hit extends its fade.
    static constexpr uint32_t kOnsetHalfLifeStretch = 4;
    /// How much wider a flash draws a boid at a full hit's peak, shrinking back as the flash fades.
    static constexpr float kFlashSwell = 0.3f;
    /// The predator's mouth, as the angle either side of its heading: nearly shut in silence, wide open on a full bass.
    static constexpr float kMouthShut = 0.17f;   // 10 degrees
    static constexpr float kMouthOpen = 0.79f;   // 45 degrees
    /// The predator's least radius before its bass swell, in lights, so its mouth shows on a small panel.
    static constexpr float kMouthMinRadius = 2.0f;
    /// How long a spectrum bar takes to fall halfway when its band goes quiet, in milliseconds: @xref{the-spectrum|a meter's fall}.
    static constexpr float kSpectrumFallMs = 150.0f;
    /// The ring's radius, as a share of the grid's shorter side.
    static constexpr float kRingShare = 0.3f;

    /// How far either side of `formEvery` a wait may fall, as a share of it.
    static constexpr float kFormJitter = 0.25f;
    /// The longest the boids may take to reach their keys, after which the stragglers land at once, so a slow `speed` still completes the keyboard.
    static constexpr uint32_t kFormMaxMs = 20000;
    /// A boid this close to its key's place, in flock units, lands on it.
    static constexpr float kLandWithin = 0.25f;
    /// The arrival's slowing distance and turning force, Aurora's Boid's: within the distance a boid slows in proportion, and the force, above a boid's in flight, keeps it from circling its place: @xref{on-flocks-boids|the arrival}.
    static constexpr float kSlowWithin = 4.0f;
    static constexpr float kFormForce = 0.050f;

    /// A key's foot on the grid and the way its bar grows, in light coordinates: the edge or the ring point it stands on, and a unit direction.
    struct Foot {
        float x, y, dx, dy;
    };

    boids::Flock flock_{*this};                 ///< boid k is key k, and the trail
    ScratchBuffer<KeyState> keys_{*this};       ///< key k's state
    float glow_[kRanges] = {};                ///< each drum range's glow, from its last hit, fading
    uint32_t glowHalfLifeMs_[kRanges] = {};     ///< how fast each glow fades, set by its hit's strength
    uint8_t hunter_ = boids::kNone;             ///< the boid that is the predator
    uint8_t phase_ = kFlying;                   ///< flying, forming the keyboard, holding it or releasing it
    uint8_t edge_ = kDockBottom;                ///< where the keyboard forms this time
    bool kicked_ = false;                       ///< whether the newest frame carried a bass hit
    bool clockStarted_ = false;                 ///< whether the heat and the phases have their first tick's time
    uint16_t heardSeq_ = 0;                     ///< the audio frame whose strikes last heated the keys
    lengthType gridW_ = 1, gridH_ = 1;          ///< the grid the places are set on, read at the start of each tick
    uint32_t phaseMs_ = 0;                      ///< when the phase began
    float waitShare_ = 1.0f;                    ///< this flight's, or this hold's, share of `formEvery`, drawn once
    uint32_t cooledMs_ = 0;                     ///< when the heat last cooled
    boids::StepClock clock_;
    Random8 rng_{boids::kSeed};

    // --- Listening -------------------------------------------------------------------------------

    /// The drum range key `k` lives in.
    Range rangeOf(uint8_t k) const { return static_cast<Range>(rangeOfThird(keys_[k].third)); }

    /// The third-octave band key `k` sounds in: band 17 is 1 kHz, a third apart each.
    static uint8_t thirdOf(uint8_t k) {
        const float hz = 27.5f * std::exp2(k / 12.0f);
        return static_cast<uint8_t>(std::clamp<long>(std::lround(17 + 3 * std::log2(hz / 1000.0f)), 0, 30));
    }
    /// The one of the sixteen bands nearest each third, from the core's table of which thirds they are, built once in flash.
    static constexpr std::array<uint8_t, 31> kNearestSixteen = [] {
        std::array<uint8_t, 31> nearest{};
        for (int t = 0; t < 31; t++)
            for (uint8_t b = 1; b < 16; b++) {
                const int now = kSixteenOfThirds[nearest[t]] - t, then = kSixteenOfThirds[b] - t;
                if ((then < 0 ? -then : then) < (now < 0 ? -now : now)) nearest[t] = b;
            }
        return nearest;
    }();

    /// The share of a value that survives `dtMs` at a half-life of `halfLifeMs`, exactly, so a fast tick cools as much in a second as a slow one; a half-life of 0 keeps none.
    static float survives(uint32_t dtMs, float halfLifeMs) { return halfLifeMs > 0 ? std::exp2(-static_cast<float>(dtMs) / halfLifeMs) : 0.0f; }

    /// Cool every key and every drum's glow by the time since the last tick, let the spectrum fall and rise with the bands, light the drums the new frame hits, heat each kept key it strikes, and free the predator's slot once its key cools.
    void listen(uint32_t now) {
        const uint32_t dt = now - cooledMs_;
        cooledMs_ = now;
        cool(dt);
        const AudioFrame* f = AudioService::latestFrame();
        followBands(*f, dt);
        if (f->seq == heardSeq_) return;   // a frame's strikes count once, however many ticks read it
        heardSeq_ = f->seq;
        hearDrums(*f);
        keep(*f, now);
        for (uint8_t k = 0; k < kAudioNoteKeys; k++)
            if (hitAt(struck_, k) && shown_.has(k)) heatKey(k, growth * static_cast<float>(std::max(level(*f, k), kStrikeFloor)) / 255.0f);
    }

    /// Cool every key's heat and every drum's glow by `dt` milliseconds, and free the predator's slot once its key is under half the `trigger`.
    void cool(uint32_t dt) {
        const float left = survives(dt, fade * 1000.0f);
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) keys_[k].heat *= left;
        for (uint8_t r = 0; r < kRanges; r++) glow_[r] *= survives(dt, static_cast<float>(glowHalfLifeMs_[r]));
        if (hunter_ != boids::kNone && keys_[hunter_].heat < trigger / 2.0f) hunter_ = boids::kNone;
    }

    /// Each key's spectrum bar rises with its band in frame `f` at once and falls by `dt` milliseconds of @xref{the-spectrum|a meter's fall}.
    void followBands(const AudioFrame& f, uint32_t dt) {
        const AudioBars bands = barsOf(f, kBandsThirds);
        const float fall = survives(dt, kSpectrumFallMs);
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            KeyState& key = keys_[k];
            const uint8_t band = bands.scale == kBandsThirds ? key.third : kNearestSixteen[key.third];
            const float loud = band < bands.n ? bands.mag[band] : 0.0f;
            key.spectrum = std::max(loud, key.spectrum * fall);
        }
    }

    /// Light each drum range frame `f` hits, as bright as `onsetGlow` and the hit allow, fading slower the stronger the hit; note whether the kick sounded.
    void hearDrums(const AudioFrame& f) {
        kicked_ = f.onsetLow > 0;
        const uint8_t hits[kRanges] = {f.onsetLow, f.onsetMid, f.onsetHigh};
        for (uint8_t r = 0; r < kRanges; r++) {
            const float lit = hits[r] * onsetGlow / 255.0f;
            if (lit <= glow_[r]) continue;   // a weaker hit inside a stronger one's glow leaves it be
            glow_[r] = lit;
            glowHalfLifeMs_[r] = kOnsetHalfLifeMs + (kOnsetHalfLifeStretch - 1) * kOnsetHalfLifeMs * hits[r] / 255u;
        }
    }

    /// Add `gain` to key `k`'s heat; crossing `trigger` makes its boid the predator, and the previous one rejoins the flock. Of keys crossing in one frame, the highest takes the slot.
    void heatKey(uint8_t k, float gain) {
        float& heat = keys_[k].heat;
        const bool below = heat < trigger;
        heat = std::min(255.0f, heat + gain);
        if (below && heat >= trigger) hunter_ = k;
    }

    // --- Drawing ---------------------------------------------------------------------------------

    /// Key `k`'s pitch class across the palette, C at the start.
    static uint8_t pitchHue(uint8_t k) { return static_cast<uint8_t>((kFirstMidi + k) % 12 * 256u / 12u); }

    /// Key `k`'s color: its pitch class, as bright as the key is hot, from a cold glow up, plus `glow`.
    RGB keyColor(const Palette& pal, uint8_t k, float glow) const {
        const float level = kColdLevel + (255 - kColdLevel) * keys_[k].heat / 255.0f + glow;
        return colorFromPalette(pal, pitchHue(k), static_cast<uint8_t>(std::min(255.0f, level)));
    }

    /// How wide key `k`'s boid is drawn before any flash, as a radius: one light when cold, growing to the `size` radius at full heat.
    float heatRadius(uint8_t k, lengthType w, lengthType h) const {
        const float full = boids::radius(size, w, h);
        return 0.5f + (full - 0.5f) * keys_[k].heat / 255.0f;
    }

    /// How much a glow swells a boid: up to `kFlashSwell` wider at a full one, measured against `onsetGlow` so the swell is the same at any brightness, at most a full one's.
    float swell(float glow) const { return onsetGlow ? 1.0f + kFlashSwell * std::min(1.0f, glow / onsetGlow) : 1.0f; }

    /// Key `k`'s boid as it is now: its color and size with its drum's glow.
    void head(const boids::Plane& plane, const Palette& pal, uint8_t k) const {
        const float glow = glow_[rangeOf(k)];
        plane.head(flock_.boids[k].x, flock_.boids[k].y, keyColor(pal, k, glow), heatRadius(k, plane.w(), plane.h()) * swell(glow));
    }

    /// Key `k`'s streak on `trail`, from where its boid stood before the step to where it is, in its color and size with its drum's glow.
    void traceKey(const boids::Plane& trail, const Palette& pal, uint8_t k, const boids::Boid& from) const {
        const float glow = glow_[rangeOf(k)];
        const boids::Boid& b = flock_.boids[k];
        trail.trace(from.x, from.y, b.x, b.y, keyColor(pal, k, glow), heatRadius(k, trail.w(), trail.h()) * swell(glow));
    }

    /// Every boid drawn as it is now, the flying predator with its mouth; a boid on its place where a bar is one light is that bar, which carries its band's loudness, so it draws no head over it.
    void heads(const boids::Plane& layer) const {
        const Palette& pal = *Palettes::active();
        const bool mouthed = phase_ == kFlying && hunter_ != boids::kNone;
        const bool barIsBoid = phase_ != kFlying && reach() <= 1;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++)
            if ((!mouthed || k != hunter_) && !(barIsBoid && placed(k))) head(layer, pal, k);
        if (!mouthed) return;
        const boids::Boid& p = flock_.boids[hunter_];
        const uint8_t bass = AudioService::latestFrame()->bass;
        const float glow = bass * onsetGlow / 255.0f;   // the predator pulses with the bass line, not its key's drum
        layer.mouth(p.x, p.y, p.vx, p.vy, keyColor(pal, hunter_, glow), std::max(kMouthMinRadius, heatRadius(hunter_, layer.w(), layer.h())) * swell(glow),
                    kMouthShut + (kMouthOpen - kMouthShut) * bass / 255.0f);
    }

    // --- Flying ----------------------------------------------------------------------------------

    /// One step of flight: every boid's forces gathered from where they all are, each fleeing the predator, and the predator's, the wind, then all of them move, each leaving its streak.
    void fly(const boids::Plane& trail) {
        const bool hunting = hunter_ != boids::kNone;
        const boids::Boid* hunter = hunting ? &flock_.boids[hunter_] : nullptr;
        const std::span<boids::Boid> flock(flock_.boids.data(), kAudioNoteKeys);
        for (uint8_t k = 0; k < kAudioNoteKeys; k++)
            if (k != hunter_) boids::gather(flock[k], boids::kBoid, flock, hunting ? hunter_ : k, hunter);
        if (hunting) boids::gather(flock[hunter_], boids::kPredator, flock, hunter_, nullptr);
        boids::gust(flock, rng_);

        const Palette& pal = *Palettes::active();
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            const boids::Boid from = flock[k];
            boids::move(flock[k], k == hunter_ ? boids::kPredator.maxSpeed : boids::kBoid.maxSpeed);
            traceKey(trail, pal, k, from);
        }
    }

    // --- The keyboard ----------------------------------------------------------------------------

    /// Draw the next wait's share of `formEvery`: from three quarters to five quarters of it.
    void drawWait() { waitShare_ = 1.0f + kFormJitter * (2.0f * boids::uniform(rng_) - 1.0f); }
    /// The wait now, in milliseconds: `formEvery` read live, so a change applies to the wait under way.
    uint32_t waitMs() const { return static_cast<uint32_t>(formEvery * 1000.0f * waitShare_); }

    /// Start a flight at `now`, lasting a wait.
    void flyFor(uint32_t now) {
        enter(kFlying, now);
        drawWait();
    }

    /// Where this keyboard forms: `dock`, or at random among the edges and the ring; a strip only along its length, and a grid too small for a ring on an edge.
    uint8_t pickEdge() {
        if (gridH_ < 2) return kDockBottom;
        if (gridW_ < 2) return kDockLeft;
        const bool ringFits = std::min(gridW_, gridH_) >= 8;
        if (dock != kDockRandom && (dock != kDockRing || ringFits)) return dock;
        return static_cast<uint8_t>(kDockBottom + rng_.next8() % (ringFits ? 5 : 4));
    }

    /// Key `k`'s share of an edge `n` lights long: the lights from `from` up to `to`, at least one.
    struct Share {
        lengthType from, to;
    };
    static Share shareOf(uint8_t k, lengthType n) {
        const lengthType from = static_cast<lengthType>(static_cast<int32_t>(k) * n / kAudioNoteKeys);
        return {from, static_cast<lengthType>(std::max<int32_t>(from + 1, (static_cast<int32_t>(k) + 1) * n / kAudioNoteKeys))};
    }

    /// Key `k`'s foot on the grid, in light coordinates, at the middle of its bar, and the way its bar grows: A0 at the left of the bottom and top edges, at the bottom of the side edges, and at the top of the ring, going clockwise.
    Foot footOf(uint8_t k) const {
        const float w = gridW_, h = gridH_;
        const Share across = shareOf(k, gridW_), up = shareOf(k, gridH_);
        const float x = (across.from + across.to - 1) / 2.0f, y = h - 1 - (up.from + up.to - 1) / 2.0f;
        switch (edge_) {
            case kDockTop: return {x, 0, 0, 1};
            case kDockLeft: return {0, y, 1, 0};
            case kDockRight: return {w - 1, y, -1, 0};
            case kDockRing: {
                const float r = kRingShare * std::min(w, h), turn = ((k + 0.5f) / kAudioNoteKeys - 0.25f) * 4 * boids::kQuarterTurn;
                return {(w - 1) / 2 + r * std::cos(turn), (h - 1) / 2 + r * std::sin(turn), std::cos(turn), std::sin(turn)};
            }
            default: return {x, h - 1, 0, -1};
        }
    }

    /// How long a bar can grow from its foot, in lights: across the grid from an edge, out to the grid's edge from the ring.
    float reach() const {
        if (edge_ == kDockRing) return std::max(1.0f, std::min<float>(gridW_, gridH_) * (0.5f - kRingShare));
        return edge_ == kDockLeft || edge_ == kDockRight ? gridW_ : gridH_;
    }

    /// Key `k`'s place in the flock's space: its foot, which a boid lands on.
    boids::Boid placeOf(uint8_t k) const {
        const Foot f = footOf(k);
        boids::Boid b;
        b.x = gridW_ > 1 ? (f.x + 0.5f) * boids::kSize / gridW_ : boids::kSize / 2;
        b.y = gridH_ > 1 ? (f.y + 0.5f) * boids::kSize / gridH_ : boids::kSize / 2;
        return b;
    }
    /// Whether key `k`'s boid stands on its place.
    bool placed(uint8_t k) const { return keys_[k].placed; }
    /// Put key `k`'s boid on its place, at rest.
    void land(uint8_t k) {
        flock_.boids[k] = placeOf(k);
        keys_[k].placed = true;
    }
    /// Lift every boid off its place.
    void unplaceAll() {
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) keys_[k].placed = false;
    }

    /// Key `k`'s bar length in lights while the keyboard holds: as far as its band is loud, at least one light.
    float liveBar(uint8_t k) const { return 1.0f + keys_[k].spectrum * (reach() - 1) / 255.0f; }

    /// Move the phases on: a flight that has lasted its wait forms the keyboard at a new place, a keyboard with every boid placed holds, and a hold of at least `hold` seconds starts its release on a bass hit, or once it has lasted another wait beyond them.
    void pacePhase(uint32_t now) {
        const uint32_t age = now - phaseMs_;
        if (phase_ == kFlying) paceFlight(now, age);
        else if (phase_ == kForming) paceForming(now, age);
        else if (phase_ == kHolding) paceHold(now, age);
        kicked_ = false;
    }

    /// Enter a phase at `now`.
    void enter(Phase p, uint32_t now) {
        phase_ = p;
        phaseMs_ = now;
    }

    /// A flight `age` milliseconds old that has lasted its wait forms the keyboard at a new place.
    void paceFlight(uint32_t now, uint32_t age) {
        if (!formEvery || age < waitMs()) return;
        enter(kForming, now);
        edge_ = pickEdge();
        unplaceAll();
    }

    /// A keyboard forming for `age` milliseconds holds once every boid is placed, the stragglers landing at once past `kFormMaxMs`.
    void paceForming(uint32_t now, uint32_t age) {
        const bool late = age >= kFormMaxMs;
        bool all = true;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            if (late && !placed(k)) land(k);
            all = all && placed(k);
        }
        if (!all) return;
        enter(kHolding, now);
        drawWait();
    }

    /// A hold `age` milliseconds old releases on a kick once it has lasted `hold` seconds, or once it has lasted another wait beyond them; each bar shrinks from where it stands.
    void paceHold(uint32_t now, uint32_t age) {
        const uint32_t held = hold * 1000u;
        if (!(kicked_ && age >= held) && age < held + waitMs()) return;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) keys_[k].barLights = liveBar(k);
        enter(kReleasing, now);
    }

    /// Reynolds' arrival: steer boid `b` toward key `k`'s place, slowing as it nears so it lands there.
    void arrive(boids::Boid& b, uint8_t k) {
        const boids::Boid p = placeOf(k);
        const float dx = p.x - b.x, dy = p.y - b.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < kLandWithin) { land(k); return; }
        const float top = boids::kBoid.maxSpeed;
        boids::steerToward(b, dx, dy, d < kSlowWithin ? top * d / kSlowWithin : top, kFormForce, 1);
        boids::move(b, top);
    }

    /// One step while the keyboard forms: the boids still on their way steer to their places, each leaving its streak.
    void form(const boids::Plane& trail) {
        const Palette& pal = *Palettes::active();
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            if (placed(k)) continue;
            const boids::Boid from = flock_.boids[k];
            arrive(flock_.boids[k], k);
            traceKey(trail, pal, k, from);
        }
    }

    /// One step of the release: every bar shrinks toward its boid by as far as a boid flies in a step along it; once every bar is down to its boid, the boids fly off the way their bars grew and the flock takes over.
    void shrinkBars(const boids::Plane& trail, uint32_t now) {
        const bool across = edge_ == kDockLeft || edge_ == kDockRight;
        const float scale = edge_ == kDockRing ? std::min(trail.scaleX(), trail.scaleY()) : across ? trail.scaleX() : trail.scaleY();
        const float shrink = boids::kBoid.maxSpeed * scale;
        bool down = true;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            const float boid = std::max(1.0f, 2 * heatRadius(k, trail.w(), trail.h()));
            float& length = keys_[k].barLights;
            length = std::min(length, std::max(boid, length - shrink));   // a bar only shrinks, a short one waiting at its length
            down = down && length <= boid;
        }
        if (!down) return;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            const Foot f = footOf(k);
            const float spread = (boids::uniform(rng_) - 0.5f) * boids::kBoid.maxSpeed * 2;
            flock_.boids[k].vx = f.dx * boids::kBoid.maxSpeed - f.dy * spread;
            flock_.boids[k].vy = f.dy * boids::kBoid.maxSpeed + f.dx * spread;
        }
        unplaceAll();
        flyFor(now);
    }

    /// Every placed boid as its key's bar from its foot: as long as its band is loud while the keyboard holds, as it has shrunk to while it releases; where the reach is one light, as bright as the band.
    void bars(const boids::Plane& layer) const {
        const Palette& pal = *Palettes::active();
        const float span = reach();
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            if (!placed(k)) continue;
            const float lights = phase_ == kReleasing ? keys_[k].barLights : liveBar(k);
            const uint8_t level = span > 1 ? 255 : static_cast<uint8_t>(std::max<float>(keys_[k].spectrum, kColdLevel));
            bar(layer, k, colorFromPalette(pal, pitchHue(k), level), static_cast<lengthType>(std::ceil(lights)));
        }
    }

    /// Key `k`'s bar of `length` lights: across its share of an edge, grown inward, or a ray out from its point on the ring.
    void bar(const boids::Plane& layer, uint8_t k, RGB c, lengthType length) const {
        if (edge_ == kDockBottom || edge_ == kDockTop) columns(layer, k, c, length);
        else if (edge_ == kDockLeft || edge_ == kDockRight) rows(layer, k, c, length);
        else ray(layer, k, c, length);
    }

    /// Key `k`'s bar on the bottom or top edge: its share of the columns, each grown `length` lights inward.
    void columns(const boids::Plane& layer, uint8_t k, RGB c, lengthType length) const {
        const bool top = edge_ == kDockTop;
        const Share cols = shareOf(k, layer.w());
        for (lengthType x = cols.from; x < cols.to; x++)
            draw::bar(layer.cv, x, top ? 0 : static_cast<lengthType>(layer.h() - 1), length, top ? draw::Grow::Down : draw::Grow::Up, c);
    }

    /// Key `k`'s bar on the left or right edge: its share of the rows, A0 at the bottom, each grown `length` lights inward.
    void rows(const boids::Plane& layer, uint8_t k, RGB c, lengthType length) const {
        const bool left = edge_ == kDockLeft;
        const lengthType h = layer.h();
        const Share share = shareOf(k, h);
        for (lengthType r = share.from; r < share.to; r++)
            draw::bar(layer.cv, left ? 0 : static_cast<lengthType>(layer.w() - 1), static_cast<lengthType>(h - 1 - r), length, left ? draw::Grow::Right : draw::Grow::Left, c);
    }

    /// Key `k`'s bar on the ring: a ray of `length` lights out from its point.
    void ray(const boids::Plane& layer, uint8_t k, RGB c, lengthType length) const {
        const Foot f = footOf(k);
        for (lengthType i = 0; i < length; i++)
            layer.blend(static_cast<int32_t>(std::lround(f.x + f.dx * i)), static_cast<int32_t>(std::lround(f.y + f.dy * i)), c, 1.0f);
    }
};

}  // namespace mm

#endif  // MM_HEAVY_COMPUTE
