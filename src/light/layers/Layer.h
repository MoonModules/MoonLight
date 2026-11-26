#pragma once

#include "light/util/FixtureChannels.h"   // motion-channel offsets an effect writes through
#include "light/layers/Buffer.h"
#include "core/util/ClockSync.h"   // FleetTime: the fleet clock effects animate against
#include "light/layouts/Layouts.h"
#include "light/effects/EffectBase.h"
#include "light/layers/MappingLUT.h"
#include "light/layers/BlendMap.h"   // BlendOp, for blendOp()
#include "light/modifiers/ModifierBase.h"
#include "light/powerfunctions/draw.h"        // draw::fade, the collected once-per-frame fade
#include "light/powerfunctions/particles.h"   // particles::FrameTime, the elapsed-to-scale conversion
#include "platform/platform_clock.h"   // millis, micros
#include "platform/platform_heap.h"   // free, alloc, freeHeap

#include <atomic>   // the worker time, added from a render worker's thread
#include <cstdint>

namespace mm {

/// One rendering layer: a buffer, a mapping onto physical lights, and the effects that fill it.
/// @card Layer.png
///
/// Effects write the buffer, modifiers reshape it, and `Drivers` composites the stack of layers into one output.
///
/// @guide
///
/// ## Trails and fades
///
/// Nothing clears the buffer between frames, so an effect that fades what is already there leaves a trail. A fade is a rate, so a trail is the same length at any frame rate. Several fading effects cost one pass.
///
/// ## The status line
///
/// The status shows the logical box the effects render into. A mirror modifier folds a 128 by 128 layout into a 64 by 64 box. A warning replaces it when the layer does not fit in memory.
///
/// @technical
///
/// **Interface here, bodies in `Layer.cpp`.** About 139 files include this header, the most edited one in the light domain, so a body edit rebuilds one object there instead of all of them; the tick and the per-pixel helpers stay here, where the render path inlines them (coding-standards.md, File shape).
///
/// **What it owns.** A buffer sized to the logical box, a `MappingLUT` onto physical positions, and two child lists.
///
/// **Compositing happens elsewhere.** `blendMode` and `opacity` are inert here: a layer cannot know its place in the stack, so `Drivers` reads both, plus the child order, and composites.
///
/// **The buffer persists.** Nothing clears it per frame, which is what makes trails possible, so each effect owns its background.
///
/// **Two paths.** The cold path folds the box through the static modifiers, then builds the table. The hot path runs each effect, extrudes, then applies the live modifiers.
///
/// **Fades.** An effect asks for a fade per sixtieth-of-a-second reference frame, and the layer scales it by the share of that frame the real frame covered. The layer keeps the gentlest request across the fading effects.
class Layer : public MoonModule {
public:
    /// A layer, so the Layers container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Layer; }
    /// The child roles a layer accepts: effects that write it, modifiers that reshape it.
    const char* acceptsChildRoles() const override { return "effect,modifier"; }

    /// Release the live-pass scratch this layer allocated.
    ~Layer() override;


    /// The blend modes, in option order, which kBlendModeOptions fixes so a persisted preset keeps its meaning.
    enum BlendMode : uint8_t { kBlendAlpha, kBlendAdditive, kBlendMultiply, kBlendScreen, kBlendDarken, kBlendLighten,
                               kBlendDifference, kBlendExclusion, kBlendModeCount };
    /// How this layer combines with the layers below it.
    /// @guide Multiply darkens what is beneath it, additive adds light, and alpha blends by opacity.
    uint8_t blendMode = kBlendAdditive;
    /// How strongly this layer shows, from invisible to full.
    uint8_t opacity = 255;
    /// Whether this layer shows in a composite: at opacity 0 it neither renders nor blends, so a show switches looks by opacity at no cost. The bottom layer lies over black, so it fades out like any other.
    bool contributes() const MM_NONBLOCKING { return opacity > 0; }

    /// Add one tick's time that a render worker ran, the part of this layer's time off the render loop.
    void addWorkerUs(uint32_t us) MM_NONBLOCKING { workerAccumUs_.fetch_add(us, std::memory_order_relaxed); }
    /// The microseconds per frame a render worker ran this layer, averaged over the last second as its tick time is.
    uint32_t workerUs() const { return workerUs_; }
    /// Average the worker time over the same frames as the tick time.
    void publishTiming(uint32_t frameCount) MM_NONBLOCKING override {
        MoonModule::publishTiming(frameCount);
        const uint32_t accum = workerAccumUs_.exchange(0, std::memory_order_relaxed);
        workerUs_ = frameCount > 0 ? accum / frameCount : 0;
    }

    /// Publish the two composition controls the `Drivers` container reads.
    void defineControls() override;

    /// The `BlendMap` op each `blendMode` selects, in BlendMode order so an index and its op cannot drift; class scope keeps the render path provably nonblocking.
    static constexpr BlendOp kBlendOps[kBlendModeCount] = {BlendOp::Alpha, BlendOp::Additive, BlendOp::Multiply, BlendOp::Screen,
                                                           BlendOp::Darken, BlendOp::Lighten, BlendOp::Difference,
                                                           BlendOp::Exclusion};
    /// The `BlendMap` op this layer's `blendMode` selects, read by `Drivers`.
    BlendOp blendOp() const { return blendMode < kBlendModeCount ? kBlendOps[blendMode] : BlendOp::Alpha; }

    /// Point this layer at the shared `Layouts` describing the physical topology.
    void setLayouts(Layouts* lg) { layouts_ = lg; }
    /// The active `Layouts`, for a consumer that needs per-light coordinates.
    Layouts* layouts() const { return layouts_; }
    /// Set channels per light: 3 for RGB, 4 for RGBW, more for a fixture profile; zero is rejected so every effect can assume cpl >= 1.
    void setChannelsPerLight(uint8_t cpl) { if (cpl > 0) channelsPerLight_ = cpl; }

    /// Set where this layer's fixtures keep their motion channels; every offset is absent by default, so setPan() is a no-op on a plain LED strip.
    void setFixtureChannels(const FixtureChannels& fc) { fixture_ = fc; }
    /// Where this layer's fixtures keep their motion channels.
    const FixtureChannels& fixtureChannels() const { return fixture_; }

    /// Cold path: size the box from the layouts, build the mapping, and clear the buffer.
    void prepare() override;

    /// Apply the frame's fade, then tick each enabled effect into the layer's buffer.
    void tick() MM_NONBLOCKING override {
        // The serial phase belongs to whoever ticks: a layer container runs it before dispatch, and a layer ticked directly applies its own.
        if (selfApplies_) { Palettes::applyRequest(); applyPending(); }
        // Gated per child here because the layer iterates its own children, not through the Scheduler.
        const uint32_t local = platform::millis();
        elapsed_ = static_cast<uint32_t>(local + FleetTime::offsetUs() / 1000);   // fleet time: devices side by side show one moment
        frameMs_ = frameStarted_ ? local - lastLocalMs_ : 0;   // the frame's length on the local clock, untouched by a fleet-clock step
        lastLocalMs_ = local;
        frameStarted_ = true;
        // The fade on the local clock, which only moves forward: a frozen clock spends a whole idle gap at once and wipes the trail.
        const uint32_t frameScale = fadeTime_.advance(local);
        fadeDue_ = fadeDue_ + frameScale < 2 * particles::FrameTime::kOne ? fadeDue_ + frameScale : 2 * particles::FrameTime::kOne;   // bounded: the carry, not this, holds how much
        // Once a reference frame has elapsed, as Echo's pass: each fade rounds every pixel down by up to a unit, so run every frame of a fast render the rounding alone erases a trail.
        if (fadeBy_ > 0) fadeCarry_ += static_cast<uint32_t>(fadeBy_) * frameScale;
        fadeBy_ = 0;
        if (fadeCarry_ > 0 && fadeDue_ >= particles::FrameTime::kOne) {
            fadeDue_ -= particles::FrameTime::kOne;   // the excess kept, so the passes keep the reference rate
            uint32_t amt = fadeCarry_ / particles::FrameTime::kOne;
            // A stall tops up rather than bursting, and drops the remainder so the next frame is clean.
            if (amt > 255) {
                amt = 255;
                fadeCarry_ = 0;              // the gap is spent, not banked for the next frame
            } else {
                fadeCarry_ -= amt * particles::FrameTime::kOne;
            }
            if (amt > 0) {
                draw::fade(buffer_, static_cast<uint8_t>(amt));
                bufferGen_++;
            }
        }
        // Gated once here, so every effect may assume the box is at least 1 on every axis.
        const bool hasGrid = width_ > 0 && height_ > 0 && depth_ > 0 && buffer_.count() > 0;
        for (uint8_t i = 0; hasGrid && i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Effect) continue;
            if (!child(i)->enabled()) continue;
            auto* eff = static_cast<EffectBase*>(child(i));
            uint32_t start = platform::micros();
            eff->tick();
            // The effect writes only its own slice; the framework duplicates it across the rest.
            extrude(eff->dimensions());
            bufferGen_++;   // this effect wrote the shared buffer; see bufferGen()
            addChildTimeUs(eff, platform::micros() - start);
        }
        // After the effect pass, so the frame's buffer is fully written before any modifier acts.
        bool rebuild = false;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            const uint32_t start = platform::micros();
            m->tick();
            addChildTimeUs(m, platform::micros() - start);
            rebuild |= m->consumeNeedsRebuild();
        }
        // One rebuild however many modifiers asked, applied on the render loop before the next frame renders, since layers share the layout a rebuild places lights through.
        if (rebuild) { rebuildPending_ = true; return; }

        // Skipped when nothing is live, so a static-only chain pays nothing for this.
        if (hasGrid && hasLive_) { applyLivePass(); bufferGen_++; }
    }

    /// The structural work last frame's tick decided on, applied before this frame renders: a modifier's rebuild, then each effect's own. Effects calls it on the render loop, one layer at a time.
    void applyPending();

    /// Size the live-pass snapshot to the current buffer, or free it when nothing is live, so the render path only copies and never allocates.
    void ensureLiveScratch();

    /// Remap the buffer through the live modifiers, once per frame, by a backward gather so no destination tears.
    void applyLivePass();
    /// Cell `cell` of the buffer gathers its source through the live modifiers from the snapshot, dark when the source lies off the box.
    void gatherLiveCell(uint8_t* buf, const Coord3D& cell, const Coord3D& logical);

    /// Warp the frame through the live modifiers' composed affine map, stepping the source along each row; false when one of them is not affine, or a source lies past 16.16 steps in int32, so the per-light pass runs.
    bool warpAffine(const Coord3D& logical);

    /// The enabled live modifiers' maps composed in child order into `t`; false when one is not affine.
    bool composeLive(LiveAffine& t, const Coord3D& logical) const;

    /// Whether every 16.16 source the row steps reach, one past each row's end included, fits int32: the map is linear, so the box's corners bound it.
    bool stepsFit(const LiveAffine& t) const;

    /// Row `y` of one slice: the source steps by the map's column terms, two additions a light, a source off the box reading dark.
    void warpRow(const LiveAffine& t, lengthType y, const uint8_t* src, uint8_t* slice) const;

    /// Copy the effect's written slice across the axes it does not iterate; real work only when the effect declares fewer axes than the layout has.
    void extrude(Dim effectDim) MM_NONBLOCKING;

    /// The logical light data every effect writes into.
    Buffer& buffer() { return buffer_; }
    /// The logical light data, for a reader that does not write it.
    const Buffer& buffer() const { return buffer_; }
    /// The mapping from logical cells to physical light positions.
    const MappingLUT& lut() const { return lut_; }

    /// The logical box width, which is what effects iterate.
    lengthType width() const { return width_; }
    /// The logical box height.
    lengthType height() const { return height_; }
    /// The logical box depth.
    lengthType depth() const { return depth_; }
    /// Bytes per light: 3 for RGB, 4 for RGBW, more when fixtures carry motion channels.
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    /// Milliseconds of fleet time at the start of this frame, the clock every effect animates against, the same on every device of the network.
    uint32_t elapsed() const { return elapsed_; }
    /// This frame's length in milliseconds on the local clock, the step an effect integrates by, which a fleet-clock step leaves alone.
    uint32_t frameMs() const MM_NONBLOCKING { return frameMs_; }

    /// Where this device's lights sit in the wall, from its Layouts, zero where it is the whole picture.
    Coord3D origin() const { return layouts_ ? layouts_->origin() : Coord3D{0, 0, 0}; }
    /// The wall's extents, from the Layouts, zero where this device is the whole picture.
    Coord3D whole() const { return layouts_ ? layouts_->whole() : Coord3D{0, 0, 0}; }

    /// Ask for a fade of `amt`/255 per reference frame, collected as the gentlest across effects; every amount is a rate, so a blank buffer is draw::fill.
    void fadeToBlackBy(uint8_t amt) { fadeBy_ = fadeBy_ ? (amt < fadeBy_ ? amt : fadeBy_) : amt; }

    /// How many times anything has written the shared buffer, so a holder knows it is untouched; every new writer of buffer_ bumps it.
    uint32_t bufferGen() const { return bufferGen_; }

    /// How many physical lights the mapping covers.
    nrOfLightsType physicalLightCount() const {
        return layouts_ ? layouts_->totalLightCount() : 0;
    }

    /// The physical box width, before any modifier reshapes it; a driver reads it rather than caching a startup value.
    lengthType physicalWidth() const { return physicalWidth_; }
    /// The physical box height.
    lengthType physicalHeight() const { return physicalHeight_; }
    /// The physical box depth.
    lengthType physicalDepth() const { return physicalDepth_; }

    /// The effect whose wiring-order frame goes to the output unmapped, or null: the sole enabled layer, mapped, no enabled modifier, one enabled effect that delivers wiring order.
    EffectBase* wiringSource() const MM_NONBLOCKING {
        if (!lut_.hasLUT()) return nullptr;   // an identity mapping is already one copy
        EffectBase* source = soleEffect();
        return source && source->deliversWiringOrder() && soleLayer() ? source : nullptr;
    }

    /// The one enabled effect, or null when there is none, there are two sharing the buffer, or a modifier transforms the frame.
    EffectBase* soleEffect() const MM_NONBLOCKING;

    /// Whether this is the only enabled layer, since a stack blends logical buffers.
    bool soleLayer() const MM_NONBLOCKING;

    /// Each of the first `cells` cells of `box` gets the light the layout puts there, the first where several share it, `kNoDriver` where none: a layer's logical box, or a screen's face.
    void placeByLayout(nrOfLightsType* out, size_t cells, Coord3D box) const;

    /// The gap callback of a walk where a gap is dropped and stays black: a wire slot present, with no source.
    static constexpr CoordCallback kDropGap = [](void*, nrOfLightsType, lengthType, lengthType, lengthType) {};

    /// Whether a mapping was wanted but could not be built, so the layer degraded to identity.
    bool lutSkipped() const { return lutSkipped_; }

    /// Fold the box through the static modifiers and build the mapping, on the cold path, once the physical dimensions are set.
    void rebuildLUT();
    /// The box after every enabled static modifier has sized it, from the physical box; sets `staticCount_` and whether any modifier is live.
    Coord3D foldBox();

    /// A box cell that is not a real light has no driver index.
    static constexpr nrOfLightsType kNoDriver = static_cast<nrOfLightsType>(-1);

    /// Whether the layout emits lights in box order, which validates the dense fast path; measured over the coords the build walks.
    bool isNaturalOrder() const;

    /// Build the folded mapping in place as a compressed sparse row: count each cell's lights, turn the counts into runs, then place each light in its cell's run.
    bool buildFoldedLUT(const Coord3D& logical, nrOfLightsType logicalCount, nrOfLightsType driverCount);

    /// How many cells a box holds (the flat light count); 0 on any 0-extent axis.
    static nrOfLightsType cellCount(const Coord3D& box) {
        return static_cast<nrOfLightsType>(box.x) * static_cast<nrOfLightsType>(box.y) *
               static_cast<nrOfLightsType>(box.z);
    }

    /// Hold a folded box inside its legal bounds, so a modifier cannot size it away: a zero-width box would blank the layer.
    void clampLogical(Coord3D& logical) const {
        if (physicalWidth_  > 0 && logical.x < 1) logical.x = 1;
        if (physicalHeight_ > 0 && logical.y < 1) logical.y = 1;
        if (physicalDepth_  > 0 && logical.z < 1) logical.z = 1;
        if (logical.x < 0) logical.x = 0;
        if (logical.y < 0) logical.y = 0;
        if (logical.z < 0) logical.z = 0;
    }

private:
    Layouts* layouts_ = nullptr;
    Buffer buffer_;
    MappingLUT lut_;
    uint8_t channelsPerLight_ = 3;
    FixtureChannels fixture_;
    bool lutSkipped_ = false;
    lengthType physicalWidth_ = 0;
    lengthType physicalHeight_ = 0;
    lengthType physicalDepth_ = 0;
    lengthType width_ = 0;  // logical (what effects see)
    lengthType height_ = 0;
    lengthType depth_ = 0;
    bool selfApplies_ = true;   ///< this layer runs its own pending work: bare, or under a parent that is no layer container
    uint32_t elapsed_ = 0;
    uint32_t frameMs_ = 0;       // this frame's length in local milliseconds, 0 on the first
    uint32_t lastLocalMs_ = 0;   // the local clock at the previous frame
    bool     frameStarted_ = false;   // whether lastLocalMs_ holds a frame's time
    uint8_t  fadeBy_ = 0;   // fade RATE collected from effects (MIN), consumed once at frame start
    uint32_t fadeCarry_ = 0;               // the fade collected since the last pass, so a high frame rate does not over-fade
    uint32_t fadeDue_ = 0;                 // the reference frames' share elapsed since the last pass, 256 a whole one
    particles::FrameTime fadeTime_{60};    // elapsed-to-scale, the shared conversion
    uint32_t bufferGen_ = 0;   // bumped by every write to buffer_; see bufferGen()
    char statusBuf_[20] = {};  // "999×999×999" fits; owned (setStatus borrows the pointer)
    bool     hasLive_ = false;          // any enabled modifier animates per frame (gates the live pass)
    bool     rebuildPending_ = false;   // a modifier asked for a rebuild, which applyPending applies before the next frame
    uint8_t  staticCount_ = 0;          // enabled modifiers folded into the mapping at build time
    uint8_t* liveScratch_ = nullptr;    // snapshot for the live pass; allocated only when hasLive_
    size_t   liveScratchBytes_ = 0;
    std::atomic<uint32_t> workerAccumUs_{0};   ///< worker time since the last publish, added from the worker's thread
    uint32_t workerUs_ = 0;                    ///< the worker time per frame the last second averaged

    /// Whether the heap can afford an allocation: true when unlimited or the budget suffices.
    static bool canAllocate(size_t bytesNeeded);

    /// The channel count this layer's fixtures need, or 0 when nothing in the rig moves.
    uint8_t requiredChannels() const;

    /// Allocate the buffer for `count` lights, halving the box until it fits, and say so in the status when it shrank or failed.
    void allocateBuffer(nrOfLightsType count);
    /// Whether the buffer of `count` lights is held or allocated, false when the heap cannot afford it.
    bool tryAllocate(nrOfLightsType count);
    /// Halve each box dimension, down to 1, and return the light count of the smaller box.
    nrOfLightsType halveBox();
};

// EffectBase accessor implementations

inline Layer* EffectBase::layer() const { return static_cast<Layer*>(parent()); }
inline uint8_t* EffectBase::buffer() { return layer()->buffer().data(); }
inline lengthType EffectBase::width() const { return layer()->width(); }
inline lengthType EffectBase::height() const { return layer()->height(); }
inline lengthType EffectBase::depth() const { return layer()->depth(); }
inline uint8_t EffectBase::channelsPerLight() const { return layer()->channelsPerLight(); }

/// Write one non-color channel of light `index`, such as pan or tilt; never scaled by brightness, and a missing channel makes the write a no-op.
inline void effectSetChannel(Layer* l, nrOfLightsType index, uint8_t offset, uint8_t value) {
    if (offset == FixtureChannels::kAbsent || !l) return;
    const uint8_t cpl = l->channelsPerLight();
    if (offset >= cpl) return;
    Buffer& b = l->buffer();
    if (index >= b.count() || !b.data()) return;
    b.data()[static_cast<size_t>(index) * cpl + offset] = value;
}

inline void EffectBase::setPan(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().pan, value);
}
inline void EffectBase::setTilt(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().tilt, value);
}
inline void EffectBase::setZoom(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().zoom, value);
}
inline void EffectBase::setRotate(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().rotate, value);
}
inline void EffectBase::setGobo(nrOfLightsType index, uint8_t value) {
    effectSetChannel(layer(), index, layer()->fixtureChannels().gobo, value);
}
inline bool EffectBase::movable() const { return layer()->fixtureChannels().movable(); }
inline bool EffectBase::hasBeam() const {
    // Null-checked because a probe instance is parentless, and an unparented effect has no beam.
    const Layer* l = layer();
    if (!l) return false;
    const FixtureChannels& fc = l->fixtureChannels();
    return fc.gobo != FixtureChannels::kAbsent || fc.rotate != FixtureChannels::kAbsent;
}
inline nrOfLightsType EffectBase::nrOfLights() const { return layer()->buffer().count(); }
inline uint32_t EffectBase::elapsed() const { return layer()->elapsed(); }
inline uint32_t EffectBase::frameMs() const MM_NONBLOCKING { return layer()->frameMs(); }
inline draw::Canvas EffectBase::canvas() {
    Layer* l = layer();
    draw::Canvas cv = draw::Canvas::of(l->buffer(), l->width(), l->height(), l->depth());
    if (footprint() == Footprint::Region) {   // a mirrored effect draws its own lights as their own whole, wherever the device sits
        cv.origin = l->origin();
        cv.whole = l->whole();
    }
    return cv;
}

} // namespace mm
