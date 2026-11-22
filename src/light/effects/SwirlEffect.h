#pragma once

#include "light/effects/AudioEffectBase.h"   // the analysis, while a mod slot reads it
#include "core/services/AudioDemand.h"       // the notes, while a mod slot reads the chord
#include "light/powerfunctions/modmatrix.h"   // the sources a mod slot listens to, the LFOs, and the one routing rule
#include "light/powerfunctions/swirl.h"       // the particle engine and its emitters and fields

#if MM_HEAVY_COMPUTE   // float particles, only where the SoC declares a hardware FPU, as swirl.h says

namespace mm {

/// Particles born by one of five emitters and moved by one of five fields: a strange attractor, a jet, rings, a flow, and every pairing between.
/// @card SwirlEffect.gif
///
/// Pick how particles are born, the `emitter`, and what moves them, the `field`. A `preset` loads a pairing and its knobs at once. Every pairing draws but the edge emitter under an outward vortex with `edge` at die; the parts and their knobs are in the [Swirl power functions](https://moonmodules.org/MoonLight/moonmodules/light/power-functions.html#swirl).
///
/// Compare with @ref FlockEffect, boids steering by their neighbors rather than following a field.
///
/// @attribution James Hutchinson, Claude, Arne Halvorsen, Robert Bridson
///
/// @guide
///
/// ## Turn the knob
///
/// Every knob works at both ends. Low `shape` flings the swarm off the knot, and it reforms from the cloud; `expand` below 128 swallows the rings. A particle flung out or swallowed dies and its emitter refills.
///
/// ## Route the sound
///
/// A mod slot moves a knob with the music, or with an LFO on a board with no microphone. `speed` likes the smooth level, `shape` a kick, `spread` the bass.
///
/// @technical
///
/// ## Memory by the choice
///
/// The prepare holds only what the chosen parts need. The pool holds 24-byte particles for the most `count` can reach, its value plus the depth of any slot on it, rounded up to 32 so a slider's drag prepares at most eight times.
///
/// The attractor's depth, 4 bytes a particle, is held only while the attractor is the field, and the trail only while `trail` or a slot on it can be above 0.
///
/// A control change prepares again exactly when the buffers it needs differ from those held, the depth by its count, or when it loads a preset; every other knob applies on the next tick, the pattern running on, and a slot change that moves the audio it reads turns its demand on or off with no prepare. A refused allocation frees all three buffers, and the tick draws black until a prepare fits.
///
/// ## The tick
///
/// The tick resolves every knob through the mod slots once, runs the steps it owes within its time, each fading the trail first, and draws. A prepare settles the pattern, @xref{memory-by-the-choice|in the buffers it sized}, turbulence at 0: six hundred steps for the attractor, one for the scatter, a hundred and fifty for the rest, at most 72,000 particle-steps in all.
class SwirlEffect : public AudioEffectBase {
public:
    /// Catalog tags: MoonLight origin, and reacting to the audio frame.
    const char* tags() const override { return "💫🎵"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The knobs, by index into `param`: an emitter's first, then a field's, then the shared ones, the order the card shows them.
    enum Param : uint8_t {
        kSpread, kPeriod, kRingSize,
        kShape, kScale, kDrift, kExpand, kSwirl, kDrag, kGravity, kPull,
        kCount, kSpeed, kLife, kTrail, kSize, kWander, kSpin, kTurbulence,
        kParamCount
    };
    /// The presets, in the select's order.
    enum Preset : uint8_t { kHalvorsen, kNozzleJet, kVortexRings, kAdvection, kWhirlpool, kPresetCount };
    /// How many mod slots the effect offers.
    static constexpr uint8_t kSlotCount = 3;

    /// Loads a pairing and its knobs at once; the mod slots stay as they are.
    uint8_t preset = kHalvorsen;
    /// How particles are born: scatter, cloud, nozzle, ring or edge.
    uint8_t emitter = 0;
    /// What moves them: attractor, flow, vortex, ballistic or orbit.
    uint8_t field = 0;
    /// At the border a particle flies off, ends, wraps or bounces.
    uint8_t edge = 0;
    /// What picks a particle's color: birth, age, speed or place.
    uint8_t color = 0;
    /// Every knob's base value, by Param.
    uint8_t param[kParamCount] = {};
    /// Each mod slot's source, a modmatrix::Source.
    uint8_t modSource[kSlotCount] = {};
    /// Each mod slot's knob: 0 none, else the Param one below it.
    uint8_t modDest[kSlotCount] = {};
    /// Each mod slot's depth, in its knob's units.
    uint8_t modDepth[kSlotCount] = {};
    /// Each LFO's rate, r/32 cycles a second.
    uint8_t lfoRate[modmatrix::kLfoCount] = {16, 5};

    /// A swirl starts as the Halvorsen preset.
    SwirlEffect() { loadPreset(kHalvorsen); }

    /// Publish the preset, the emitter and its knobs, the field and its knobs, the shared knobs, the look, the mod slots and the LFO rates, each part's knobs hidden while it is not chosen.
    void defineControls() override {
        controls_.addSelect("preset", preset, kPresetOptions, kPresetCount);   // first, so a state document's tuned knobs apply after the preset it names
        controls_.addSelect("emitter", emitter, swirl::kEmitterOptions, static_cast<uint8_t>(swirl::Emitter::kCount));
        /// The nozzle's cone, from a needle to a full circle.
        addKnob("spread", kSpread);
        /// Steps between rings.
        addKnob("period", kPeriod);
        /// Particles in a ring; a smaller count blows a smaller ring.
        addKnob("ringSize", kRingSize);
        controls_.addSelect("field", field, swirl::kFieldOptions, static_cast<uint8_t>(swirl::Field::kCount));
        /// Halvorsen's constant a hundredfold; 189 is the usual knot.
        addKnob("shape", kShape);
        /// The attractor's zoom, or the size of the flow's features.
        addKnob("scale", kScale);
        /// How fast the flow changes.
        addKnob("drift", kDrift);
        /// Outward above 128, inward below, where the origin swallows them.
        addKnob("expand", kExpand);
        /// Around the origin, one way above 128 and the other below.
        addKnob("swirl", kSwirl);
        /// The share of its speed a particle loses each step.
        addKnob("drag", kDrag);
        /// The pull down the panel.
        addKnob("gravity", kGravity);
        /// The orbit's pull toward the origin.
        addKnob("pull", kPull);
        /// How many particles fly.
        addKnob("count", kCount);
        /// Steps a second: 60 is the reference, 0 holds the picture.
        addKnob("speed", kSpeed);
        /// How long a particle lives, four steps a unit; 0 is forever.
        addKnob("life", kLife);
        /// How long the streaks stay; 0 draws each step fresh and holds no trail.
        addKnob("trail", kTrail);
        /// How wide a particle is drawn, in lights.
        addKnob("size", kSize);
        /// How far the whole motion tours the panel.
        addKnob("wander", kWander);
        /// Turns the motion: 128 holds it still, either side turns it.
        addKnob("spin", kSpin);
        /// Rough curls added over any field.
        addKnob("turbulence", kTurbulence);
        controls_.addSelect("color", color, swirl::kColorOptions, static_cast<uint8_t>(swirl::Color::kCount));
        controls_.addSelect("edge", edge, swirl::kEdgeOptions, static_cast<uint8_t>(swirl::Edge::kCount));
        /// What slot 1 listens to: a sound signal or an LFO.
        addSlotSource("mod1Source", 0);
        /// The knob slot 1 lifts.
        addSlotDest("mod1Dest", 0);
        /// How far a full source lifts slot 1's knob, in the knob's units.
        addSlotDepth("mod1Depth", 0);
        /// What slot 2 listens to: a sound signal or an LFO.
        addSlotSource("mod2Source", 1);
        /// The knob slot 2 lifts.
        addSlotDest("mod2Dest", 1);
        /// How far a full source lifts slot 2's knob, in the knob's units.
        addSlotDepth("mod2Depth", 1);
        /// What slot 3 listens to: a sound signal or an LFO.
        addSlotSource("mod3Source", 2);
        /// The knob slot 3 lifts.
        addSlotDest("mod3Dest", 2);
        /// How far a full source lifts slot 3's knob, in the knob's units.
        addSlotDepth("mod3Depth", 2);
        /// LFO 1's rate: 32 is one cycle a second.
        addLfoRate("lfo1Rate", 0);
        /// LFO 2's rate: 32 is one cycle a second.
        addLfoRate("lfo2Rate", 1);
    }

    /// The audio demands follow the slots, and a preset loads its knobs and shows its part's controls; every other control is read on the next tick.
    void onControlChanged(const char* name) override {
        followAnalysis();   // a slot's source or knob can move the audio it reads, which needs no buffer, so no prepare
        notes_.set(readsNotes());
        if (std::strcmp(name, "preset") != 0) return;
        loadPreset(preset);
        rebuildControls();   // the preset moved the emitter and the field, so the hidden flags follow them
    }

    /// Prepare again for a preset, or when the buffers the controls now need differ from those held: @xref{memory-by-the-choice}.
    bool affectsPrepare(const char* name) const override {
        if (std::strcmp(name, "preset") == 0) return true;
        return poolWanted() != engine_.pool.count() || (depthWanted() ? poolWanted() : 0u) != engine_.depth.count() || trailBytesWanted() != engine_.trail.bytes();
    }

    /// Need the audio the slots read, hold the buffers the choice needs, settle the pattern, and start the step clock.
    void prepare() override {
        AudioEffectBase::prepare();   // the analysis while a slot reads it
        notes_.set(readsNotes());
        if (!engine_.prepare(*this, poolWanted(), depthWanted(), trailBytesWanted())) return;
        motion_ = swirl::Motion{};
        lastMs_ = elapsed();
        resolve();
        motion_.turbulence = 0;   // the settle forms the pattern; the churn returns with the first tick's resolve
        engine_.settle(motion_, settleSteps(motion_.count), look_.edge);
        clock_.reset();
    }

    /// Resolve the knobs, let particles past the count go, run the steps this tick owes within its time, each fading the trail, and draw.
    void tick() MM_NONBLOCKING override {
        if (engine_.pool.count() == 0) {   // the prepare ran out of memory: every buffer is free, and the layer shows black under the warning
            draw::fill(canvas(), RGB{0, 0, 0});
            return;
        }
        std::optional<boids::Plane> trailPlane;
        if (engine_.trailHeld()) {
            trailPlane = engine_.plane(width(), height(), channelsPerLight());
            if (!trailPlane) return;   // a resize outran the prepare: the frame waits for it
        }
        resolve();
        engine_.trim(motion_.count);
        const boids::Plane* plane = trailPlane ? &*trailPlane : nullptr;
        look_.radius = boids::radius(now_[kSize], width(), height());
        const uint32_t owed = clock_.steps(elapsed(), now_[kSpeed]);
        boids::runSteps(owed, [&] {
            if (plane) plane->fade(now_[kTrail]);   // a fade a step, so the trail is as long at any frame rate
            engine_.step(motion_, plane, look_);
        });
        engine_.present(plane, canvas(), look_);
    }

    /// The value knob `p` runs at this tick, after the mod slots.
    uint8_t nowForTest(Param p) const { return now_[p]; }
    /// How many particles are alive, for tests.
    uint16_t liveForTest() const { return engine_.live; }
    /// The pool's capacity, for tests.
    size_t capacityForTest() const { return engine_.pool.count(); }
    /// Whether the trail and the depth are held, for tests.
    bool trailHeldForTest() const { return engine_.trailHeld(); }
    bool depthHeldForTest() const { return engine_.depthHeld(); }
    /// Live particle `i`, or nullptr past the live ones, for tests.
    const swirl::Particle* particleForTest(uint16_t i) const { return i < engine_.live ? &engine_.pool[i] : nullptr; }
    /// The motion, for tests.
    const swirl::Motion& motionForTest() const { return motion_; }

private:
    /// The mask of a knob every motion shows.
    static constexpr uint8_t kAll = 0xFF;
    /// One knob's name, bounds, the value it takes when a preset leaves it alone, and the emitters and fields that show it.
    struct ParamInfo { const char* name; uint8_t lo, hi, def, emitters, fields; };
    /// Every knob, in Param order.
    static constexpr ParamInfo kParams[kParamCount] = {
        {"spread", 0, 255, 23, swirl::mask(swirl::Emitter::kNozzle), kAll},
        {"period", 1, 255, 30, swirl::mask(swirl::Emitter::kRing), kAll},
        {"ringSize", 3, 64, 24, swirl::mask(swirl::Emitter::kRing), kAll},
        {"shape", 100, 255, 189, kAll, swirl::mask(swirl::Field::kAttractor)},
        {"scale", 0, 255, 48, kAll, swirl::mask(swirl::Field::kAttractor, swirl::Field::kFlow)},
        {"drift", 0, 255, 96, kAll, swirl::mask(swirl::Field::kFlow)},
        {"expand", 0, 255, 166, kAll, swirl::mask(swirl::Field::kVortex)},
        {"swirl", 0, 255, 160, kAll, swirl::mask(swirl::Field::kVortex, swirl::Field::kOrbit)},
        {"drag", 0, 255, 61, kAll, swirl::mask(swirl::Field::kBallistic, swirl::Field::kOrbit)},
        {"gravity", 0, 255, 64, kAll, swirl::mask(swirl::Field::kBallistic)},
        {"pull", 0, 255, 64, kAll, swirl::mask(swirl::Field::kOrbit)},
        {"count", 1, 255, 120, kAll, kAll},
        {"speed", 0, 255, swirl::kReferenceHz, kAll, kAll},
        {"life", 0, 255, 0, kAll, kAll},
        {"trail", 0, 255, boids::kTrailKeep, kAll, kAll},
        {"size", 1, boids::kMaxSize, 1, kAll, kAll},
        {"wander", 0, 255, 0, kAll, kAll},
        {"spin", 0, 255, 128, kAll, kAll},
        {"turbulence", 0, 255, 0, kAll, kAll},
    };
    /// The preset names, in Preset order.
    static constexpr const char* kPresetOptions[kPresetCount] = {"Halvorsen", "NozzleJet", "VortexRings", "Advection", "Whirlpool"};
    /// The destinations a slot offers: none, then every knob by name.
    static constexpr std::array<const char*, kParamCount + 1> kDestOptions = [] {
        std::array<const char*, kParamCount + 1> a{};
        a[0] = "none";
        for (uint8_t i = 0; i < kParamCount; i++) a[i + 1] = kParams[i].name;
        return a;
    }();
    /// The pool grows in steps of this many particles.
    static constexpr uint16_t kPoolStep = 32;

    swirl::Engine engine_{*this};   ///< the pool, the depth, the trail, the step and the render
    swirl::Motion motion_;          ///< the emitter and the field, resolved each tick
    swirl::Look look_;              ///< the border, the color and the radius
    boids::StepClock clock_;
    modmatrix::Lfo lfo_[modmatrix::kLfoCount];
    uint32_t lastMs_ = 0;           ///< the last tick's time, which runs the LFOs
    uint8_t now_[kParamCount] = {}; ///< every knob after the mod slots, this tick
    AudioDemand notes_{*this, AudioFeature::Notes};   ///< the notes, while a slot reads the chord

    /// Publish knob `p` as `name`, within its bounds, hidden while its part is not chosen; a test holds `name` to the knob's destination name.
    void addKnob(const char* name, Param p) {
        controls_.addControl(name, param[p], kParams[p].lo, kParams[p].hi);
        controls_.setHidden(controls_.count() - 1, !shown(p));
    }
    /// Publish slot `s`'s source.
    void addSlotSource(const char* name, uint8_t s) {
        controls_.addSelect(name, modSource[s], modmatrix::kSourceOptions, modmatrix::kSourceCount);
    }
    /// Publish slot `s`'s knob, shown once the slot has a source.
    void addSlotDest(const char* name, uint8_t s) {
        controls_.addSelect(name, modDest[s], kDestOptions.data(), static_cast<uint8_t>(kDestOptions.size()));
        controls_.setHidden(controls_.count() - 1, modSource[s] == 0);
    }
    /// Publish slot `s`'s depth, shown once the slot has a source.
    void addSlotDepth(const char* name, uint8_t s) {
        controls_.addControl(name, modDepth[s], 0, 255);
        controls_.setHidden(controls_.count() - 1, modSource[s] == 0);
    }
    /// Publish LFO `l`'s rate, shown once a slot listens to it.
    void addLfoRate(const char* name, uint8_t l) {
        controls_.addControl(name, lfoRate[l], 0, 255);
        controls_.setHidden(controls_.count() - 1, !listens(static_cast<uint8_t>(static_cast<uint8_t>(modmatrix::Source::kLfo1) + l)));
    }

    /// Whether knob `p` belongs to the chosen emitter and field, by its row's masks.
    bool shown(Param p) const { return has(kParams[p].emitters, emitter) && has(kParams[p].fields, field); }
    /// Whether part `v` is in `mask`: every part for kAll, else its bit, none for a value past the list.
    static bool has(uint8_t mask, uint8_t v) { return mask == kAll || (v < 8 && ((mask >> v) & 1u)); }

    /// Whether slot `i` moves a knob: a source and a destination in the list.
    bool routed(uint8_t i) const { return modSource[i] && modDest[i] != 0 && modDest[i] <= kParamCount; }
    /// Whether a routed slot's source answers `reads`, a modmatrix test such as `readsNotes`.
    bool routesRead(bool (*reads)(modmatrix::Source)) const {
        for (uint8_t i = 0; i < kSlotCount; i++)
            if (routed(i) && reads(modmatrix::source(modSource[i]))) return true;
        return false;
    }
    /// The analysis while a routed slot reads it: the base's demand follows this.
    bool readsAnalysis() const override { return routesRead(modmatrix::readsAnalysis); }
    /// Whether a routed slot reads the notes.
    bool readsNotes() const { return routesRead(modmatrix::readsNotes); }

    /// Whether an armed slot listens to source `s`.
    bool listens(uint8_t s) const {
        for (uint8_t i = 0; i < kSlotCount; i++)
            if (modSource[i] == s) return true;
        return false;
    }

    /// The most knob `p` can reach: its base plus the depth of every armed slot on it, within its range.
    uint8_t reach(Param p) const {
        uint32_t v = param[p];
        for (uint8_t i = 0; i < kSlotCount; i++)
            if (modSource[i] && modDest[i] == p + 1) v += modDepth[i];
        return static_cast<uint8_t>(std::min<uint32_t>(v, kParams[p].hi));
    }

    /// The pool the count can reach, rounded up to `kPoolStep`: @xref{memory-by-the-choice}.
    uint16_t poolWanted() const {
        const uint32_t n = (static_cast<uint32_t>(std::max<uint8_t>(reach(kCount), 1)) + kPoolStep - 1) / kPoolStep * kPoolStep;
        return static_cast<uint16_t>(std::min<uint32_t>(n, kParams[kCount].hi));
    }
    /// Whether the attractor's depth is needed.
    bool depthWanted() const { return field == static_cast<uint8_t>(swirl::Field::kAttractor); }
    /// The trail's bytes, when the trail can be above 0.
    size_t trailBytesWanted() const { return reach(kTrail) ? static_cast<size_t>(width()) * height() * channelsPerLight() : 0; }

    /// The particle-steps a prepare's settle may run: Halvorsen's six hundred steps at its default 120 particles, so an attractor above that count settles fewer steps and a prepare costs at most Halvorsen's default.
    static constexpr uint32_t kSettleWork = 600u * 120u;

    /// How many silent steps the prepare settles for `count` particles: @xref{the-tick}.
    uint16_t settleSteps(uint8_t count) const {
        const uint16_t steps = depthWanted() ? 600 : emitter == static_cast<uint8_t>(swirl::Emitter::kScatter) ? 1 : 150;
        return static_cast<uint16_t>(std::min<uint32_t>(steps, kSettleWork / std::max<uint8_t>(count, 1)));
    }

    /// A preset's emitter, field and border.
    struct Pairing { swirl::Emitter emitter; swirl::Field field; swirl::Edge edge; };
    /// Each preset's pairing, in Preset order.
    static constexpr Pairing kPairings[kPresetCount] = {
        {swirl::Emitter::kCloud, swirl::Field::kAttractor, swirl::Edge::kFree},
        {swirl::Emitter::kNozzle, swirl::Field::kBallistic, swirl::Edge::kDie},
        {swirl::Emitter::kRing, swirl::Field::kVortex, swirl::Edge::kDie},
        {swirl::Emitter::kScatter, swirl::Field::kFlow, swirl::Edge::kDie},
        {swirl::Emitter::kEdge, swirl::Field::kOrbit, swirl::Edge::kDie},
    };
    /// One knob a preset sets away from its default.
    struct Tune { Preset preset; Param knob; uint8_t value; };
    /// Every knob a preset sets away from its default: data rather than code, so a preset costs its bytes and no instructions.
    static constexpr Tune kTunes[] = {
        {kNozzleJet, kGravity, 0},
        {kNozzleJet, kTurbulence, 64},
        {kNozzleJet, kLife, 38},     // about the 150 steps the jet was tuned on
        {kNozzleJet, kSpin, 142},    // the nozzle turns a circle in about fifteen seconds
        {kVortexRings, kCount, 192},
        {kVortexRings, kLife, 60},   // four seconds a ring
        {kAdvection, kCount, 160},
        {kAdvection, kScale, 191},   // features twelve and a half units across
        {kWhirlpool, kPull, 160},
        {kWhirlpool, kSwirl, 220},   // the border's particles curve in as arms
    };

    /// Load preset `p`'s pairing, look and knobs, every knob it does not tune at its default; an unknown preset loads the first.
    void loadPreset(uint8_t p) {
        if (p >= kPresetCount) p = kHalvorsen;
        preset = p;
        for (uint8_t i = 0; i < kParamCount; i++) param[i] = kParams[i].def;
        for (const Tune& t : kTunes)
            if (t.preset == p) param[t.knob] = t.value;
        emitter = static_cast<uint8_t>(kPairings[p].emitter);
        field = static_cast<uint8_t>(kPairings[p].field);
        edge = static_cast<uint8_t>(kPairings[p].edge);
        color = static_cast<uint8_t>(swirl::Color::kBirth);
    }

    /// Run the LFOs to now, apply every armed slot to the knobs' bases, and turn the results into the motion's numbers and the look.
    void resolve() MM_NONBLOCKING {
        modmatrix::Inputs in{*AudioService::latestFrame()};
        runLfos(in);
        std::memcpy(now_, param, sizeof(now_));
        applySlots(in);
        swirl::Motion& m = motion_;
        m.emitter = pick(emitter, swirl::Emitter::kScatter);
        m.field = pick(field, swirl::Field::kFlow);
        if (m.field == swirl::Field::kAttractor && !engine_.depthHeld()) m.field = swirl::Field::kFlow;   // the attractor waits for its depth's prepare
        m.count = std::min<uint16_t>(std::max<uint8_t>(now_[kCount], 1), static_cast<uint16_t>(engine_.pool.count()));
        m.life = static_cast<uint16_t>(now_[kLife] * 4);
        m.spread = now_[kSpread] * (4 * boids::kQuarterTurn / 2 / 255);
        m.period = std::max<uint8_t>(now_[kPeriod], 1);
        m.ringSize = std::max<uint8_t>(now_[kRingSize], 3);
        m.a = now_[kShape] / 100.0f;
        m.factor = std::max<uint8_t>(now_[kScale], 8) / 40.0f;
        m.coordScale = static_cast<uint32_t>(65536.0f / (2.0f + now_[kScale] * (14.0f / 255)));
        m.drift = now_[kDrift];
        m.expand = (now_[kExpand] - 128) * (0.4f / 127);
        m.swirl = (now_[kSwirl] - 128) * (0.35f / 127);
        m.drag = now_[kDrag] * (0.05f / 255);
        m.gravity = now_[kGravity] * (0.02f / 255);
        m.pull = now_[kPull] * (0.02f / 255);
        m.spinRate = (now_[kSpin] - 128) * 0.0005f;
        m.wander = now_[kWander] * ((swirl::kSize / 2 - 2) / 255);
        m.turbulence = now_[kTurbulence] * swirl::Motion::kTurbStrength / 255;
        look_.edge = pick(edge, swirl::Edge::kFree);
        look_.color = pick(color, swirl::Color::kBirth);
    }

    /// Run each LFO the time since the last tick, and hand its value to this tick's inputs.
    void runLfos(modmatrix::Inputs& in) MM_NONBLOCKING {
        const uint32_t ms = elapsed();
        for (uint8_t l = 0; l < modmatrix::kLfoCount; l++) {
            lfo_[l].advance(lfoRate[l], ms - lastMs_);
            in.lfo[l] = lfo_[l].value();
        }
        lastMs_ = ms;
    }

    /// Lift each armed slot's knob by its source's share of its depth, within the knob's range.
    void applySlots(const modmatrix::Inputs& in) MM_NONBLOCKING {
        for (uint8_t i = 0; i < kSlotCount; i++) {
            if (!routed(i)) continue;
            const uint8_t d = modDest[i] - 1;
            now_[d] = modmatrix::apply(now_[d], modmatrix::source(modSource[i]), modDepth[i], in, kParams[d].lo, kParams[d].hi);
        }
    }

    /// A stored select as its enum, or `fallback` for a value past the list, from an older build or a hand-edited file.
    template <typename E>
    static E pick(uint8_t v, E fallback) { return v < static_cast<uint8_t>(E::kCount) ? static_cast<E>(v) : fallback; }
};

}  // namespace mm

#endif  // MM_HEAVY_COMPUTE
