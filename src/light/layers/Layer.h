#pragma once

#include "light/util/FixtureChannels.h"   // motion-channel offsets an effect writes through
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/effects/EffectBase.h"
#include "light/layers/MappingLUT.h"
#include "light/layers/BlendMap.h"   // BlendOp, for blendOp()
#include "light/modifiers/ModifierBase.h"
#include "light/powerfunctions/draw.h"        // draw::fade, the collected once-per-frame fade
#include "light/powerfunctions/particles.h"   // particles::FrameTime, the elapsed-to-scale conversion
#include "platform/platform.h"

#include <cstdio>
#include <cstring>  // std::memcpy in extrude()

namespace mm {

/// One rendering layer: a buffer, a mapping onto physical lights, and the effects that fill it.
///
/// The unit the render loop iterates, turning coordinates into lights.
/// Effects write the buffer, modifiers reshape it, and `Drivers` composites the stack.
/// @card Layer.png
///
/// @moreinfo
///
/// ## What it owns
///
/// A buffer sized to the logical box, a `MappingLUT` onto physical positions, and two child lists.
///
/// ## Compositing happens elsewhere
///
/// `blendMode` and `opacity` are inert here: a layer cannot know its place in the stack.
/// `Drivers` reads both, plus the child order, and composites.
///
/// ## The buffer persists
///
/// Nothing clears it per frame, which is what makes trails possible.
///
/// ## Two paths
///
/// The cold path folds the box through the static modifiers, then builds the table.
/// The hot path runs each effect, extrudes, then applies the live modifiers.
///
/// Details: [the supporting page](https://moonmodules.org/MoonLight/moonmodules/light/supporting.html#layer-details).
class Layer : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Layer; }
    /// The child roles a layer accepts: effects that write it, modifiers that reshape it.
    const char* acceptsChildRoles() const override { return "effect,modifier"; }

    /// Release the live-pass scratch this layer allocated.
    ~Layer() override { if (liveScratch_) platform::free(liveScratch_); }


    // Index order is fixed by kBlendModeOptions, so a persisted preset keeps its meaning.
    /// How this layer composites onto those below it, as an index into `kBlendModeOptions`.
    uint8_t blendMode = 1;     // 1 = additive
    /// How strongly this layer composites, from 0 for invisible to 255 for full.
    uint8_t opacity = 255;

    /// Publish the two composition controls the `Drivers` container reads.
    void defineControls() override {
        static constexpr const char* kBlendModeOptions[] = {"alpha", "additive"};
        controls_.addSelect("blendMode", blendMode, kBlendModeOptions, 2);
        controls_.addControl("opacity", opacity, 0, 255);
        // Cascade to the children, preserving the base behavior this overrode.
        MoonModule::defineControls();
    }

    // Index order must match kBlendModeOptions.
    /// The `BlendMap` op this layer's `blendMode` selects, read by `Drivers`.
    BlendOp blendOp() const {
        return blendMode == 1 ? BlendOp::Additive : BlendOp::Alpha;
    }

    /// Point this layer at the shared `Layouts` describing the physical topology.
    void setLayouts(Layouts* lg) { layouts_ = lg; }
    /// The active `Layouts`, for a consumer that needs per-light coordinates.
    Layouts* layouts() const { return layouts_; }
    // Rejecting zero at the one entry point lets every effect and draw primitive assume cpl >= 1.
    /// Set channels per light: 3 for RGB, 4 for RGBW, more for a fixture profile.
    void setChannelsPerLight(uint8_t cpl) { if (cpl > 0) channelsPerLight_ = cpl; }

    // Every offset is absent by default, so setPan() is a no-op on a plain LED strip.
    /// Set where this layer's fixtures keep their motion channels.
    void setFixtureChannels(const FixtureChannels& fc) { fixture_ = fc; }
    /// Where this layer's fixtures keep their motion channels.
    const FixtureChannels& fixtureChannels() const { return fixture_; }

    /// Cold path: size the box from the layouts, build the mapping, and clear the buffer.
    void prepare() override {
        // Restart discards the elapsed gap, so the first tick after a re-prepare cannot jump a trail.
        fadeTime_.reset();
        fadeCarry_ = 0;
        // No layouts wired reads the same as every layout disabled: the layer ends up empty.
        const nrOfLightsType physicalCount = layouts_ ? layouts_->totalLightCount() : 0;

        // Tear the old state down: a stale LUT beside a zero-byte buffer makes blendMap fault.
        if (physicalCount == 0) {
            physicalWidth_ = physicalHeight_ = physicalDepth_ = 0;
            width_ = height_ = depth_ = 0;
            lut_.free();
            buffer_.free();
            setDynamicBytes(0);
            // Clear the status string AND the flag: a stale flag reports a LUT already freed.
            lutSkipped_ = false;
            clearStatus();
            return;   // applyState() recurses to the effects next
        }

        // A gap counts toward the box, occupying a real position, so one callback handles both.
        struct DimCtx { lengthType maxX, maxY, maxZ; };
        DimCtx dctx{0, 0, 0};
        layouts_->placeLights(CoordSink{[](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* d = static_cast<DimCtx*>(ctx);
            if (x > d->maxX) d->maxX = x;
            if (y > d->maxY) d->maxY = y;
            if (z > d->maxZ) d->maxZ = z;
        }, nullptr, &dctx});
        physicalWidth_ = dctx.maxX + 1;
        physicalHeight_ = dctx.maxY + 1;
        physicalDepth_ = dctx.maxZ + 1;

        rebuildLUT();
        // One clear on the cold path, so a freshly added effect starts black rather than inheriting.
        buffer_.clear();
        ensureLiveScratch();   // size the live-pass snapshot here, on the cold path

        // Only when rebuildLUT left the status clear: a degrade path's warning must win over this.
        if (status() == nullptr) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%u×%u×%u",
                          static_cast<unsigned>(width_),
                          static_cast<unsigned>(height_),
                          static_cast<unsigned>(depth_));
            setStatus(statusBuf_);
        }

        // applyState() recurses to the effects next, which allocate against the LUT built here.
    }

    void tick() MM_NONBLOCKING override {
        // Gated per child here because the layer iterates its own children, not through the Scheduler.
        elapsed_ = platform::millis();
        // Advance on every frame: a frozen clock spends a whole idle gap at once and wipes the trail.
        const uint32_t frameScale = fadeTime_.advance(elapsed_);
        if (fadeBy_ > 0) {
            fadeCarry_ += static_cast<uint32_t>(fadeBy_) * frameScale;
            uint32_t amt = fadeCarry_ / particles::FrameTime::kOne;
            fadeBy_ = 0;
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
            eff->addAccumUs(platform::micros() - start);
        }
        // After the effect pass, so the frame's buffer is fully written before any modifier acts.
        bool rebuild = false;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            m->tick();
            rebuild |= m->consumeNeedsRebuild();
        }
        // One rebuild per frame however many modifiers asked, and never from inside a tick.
        if (rebuild) { applyState(); return; }

        // Skipped when nothing is live, so a static-only chain pays nothing for this.
        if (hasGrid && hasLive_) { applyLivePass(); bufferGen_++; }
    }

    // Allocating here is what lets the render path only copy, never allocate.
    /// Size the live-pass snapshot to the current buffer, or free it when nothing is live.
    void ensureLiveScratch() {
        const size_t bytes = hasLive_ ? buffer_.bytes() : 0;
        if (bytes == liveScratchBytes_ && (bytes != 0) == (liveScratch_ != nullptr)) return;
        if (liveScratch_) { platform::free(liveScratch_); liveScratch_ = nullptr; }
        liveScratchBytes_ = 0;
        if (bytes == 0) return;                       // no live modifier → no scratch held
        liveScratch_ = static_cast<uint8_t*>(platform::alloc(bytes));
        if (liveScratch_) liveScratchBytes_ = bytes;  // alloc-fail → applyLivePass no-ops, static frame shows
    }

    // A backward gather, the textbook reason image warping samples backward: no destination tears.
    /// Remap the buffer through the live modifiers, once per frame.
    void applyLivePass() {
        uint8_t* buf = buffer_.data();
        if (!buf || !liveScratch_) return;   // scratch is sized on the cold path (ensureLiveScratch)
        const size_t cpl = channelsPerLight_;
        const size_t bytes = static_cast<size_t>(width_) * height_ * depth_ * cpl;
        if (bytes == 0 || bytes > liveScratchBytes_) return;   // hot path NEVER allocates
        std::memcpy(liveScratch_, buf, bytes);   // snapshot the source frame

        const Coord3D logical{width_, height_, depth_};
        for (lengthType z = 0; z < depth_; z++) {
            for (lengthType y = 0; y < height_; y++) {
                for (lengthType x = 0; x < width_; x++) {
                    Coord3D src{x, y, z};
                    for (uint8_t i = 0; i < childCount(); i++) {
                        if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
                        auto* m = static_cast<ModifierBase*>(child(i));
                        if (m->hasModifyLive()) m->modifyLive(src, logical);
                    }
                    const size_t dstIdx = (static_cast<size_t>(z) * height_ * width_ +
                                           static_cast<size_t>(y) * width_ + x) * cpl;
                    if (src.x >= 0 && src.x < width_ && src.y >= 0 && src.y < height_ &&
                        src.z >= 0 && src.z < depth_) {
                        const size_t srcIdx = (static_cast<size_t>(src.z) * height_ * width_ +
                                               static_cast<size_t>(src.y) * width_ + src.x) * cpl;
                        std::memcpy(buf + dstIdx, liveScratch_ + srcIdx, cpl);
                    } else {
                        std::memset(buf + dstIdx, 0, cpl);   // source off-box → dark
                    }
                }
            }
        }
    }

    // Real work happens only when the effect declares fewer axes than the layout has.
    /// Copy the effect's written slice across the axes it does not iterate.
    void extrude(Dim effectDim) {
        if (effectDim == Dim::D3) return;
        uint8_t* buf = buffer_.data();
        if (!buf) return;
        const size_t cpl = channelsPerLight_;
        const size_t rowBytes = static_cast<size_t>(width_) * cpl;
        const size_t sliceBytes = rowBytes * height_;

        // A 1D effect expands into 2D by adding columns: its output is the first column.
        if (effectDim == Dim::D1 && width_ > 1) {
            for (lengthType y = 0; y < height_; y++) {
                const uint8_t* src = buf + static_cast<size_t>(y) * rowBytes;   // the x=0 pixel
                for (lengthType x = 1; x < width_; x++) {
                    std::memcpy(buf + static_cast<size_t>(y) * rowBytes + static_cast<size_t>(x) * cpl,
                                src, cpl);
                }
            }
        }
        // A 2D effect expands into 3D by adding depth slices behind the front face.
        if (depth_ > 1) {
            for (lengthType z = 1; z < depth_; z++) {
                std::memcpy(buf + z * sliceBytes, buf, sliceBytes);
            }
        }
    }

    /// The logical light data every effect writes into.
    Buffer& buffer() { return buffer_; }
    /// The logical light data, for a reader that does not write it.
    const Buffer& buffer() const { return buffer_; }
    /// The mapping from logical cells to physical light positions.
    const MappingLUT& lut() const { return lut_; }

    // Effects see logical dimensions
    /// The logical box width, which is what effects iterate.
    lengthType width() const { return width_; }
    /// The logical box height.
    lengthType height() const { return height_; }
    /// The logical box depth.
    lengthType depth() const { return depth_; }
    /// Bytes per light: 3 for RGB, 4 for RGBW, more when fixtures carry motion channels.
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    /// Milliseconds at the start of this frame, the clock every effect animates against.
    uint32_t elapsed() const { return elapsed_; }

    // Every amount is a rate, without exception: an effect wanting the buffer blank calls draw::fill.
    /// Ask for a fade of `amt`/255 per reference frame, collected as the gentlest across effects.
    void fadeToBlackBy(uint8_t amt) { fadeBy_ = fadeBy_ ? (amt < fadeBy_ ? amt : fadeBy_) : amt; }

    // Every new writer of buffer_ bumps it too, the discipline the fade already follows.
    /// How many times anything has written the shared buffer, so a holder knows it is untouched.
    uint32_t bufferGen() const { return bufferGen_; }

    /// How many physical lights the mapping covers.
    nrOfLightsType physicalLightCount() const {
        return layouts_ ? layouts_->totalLightCount() : 0;
    }

    // A driver describing the LED shape reads these rather than caching a startup value.
    /// The physical box width, before any modifier reshapes it.
    lengthType physicalWidth() const { return physicalWidth_; }
    /// The physical box height.
    lengthType physicalHeight() const { return physicalHeight_; }
    /// The physical box depth.
    lengthType physicalDepth() const { return physicalDepth_; }

    /// Whether a mapping was wanted but could not be built, so the layer degraded to identity.
    bool lutSkipped() const { return lutSkipped_; }

    // Precondition: the physical dimensions are set, so this is called from prepare.
    /// Fold the box through the static modifiers and build the mapping, on the cold path.
    void rebuildLUT() {
        lutSkipped_ = false;
        clearStatus();  // re-evaluated below if a degrade path is taken

        // Each modifier stashes its output size, so the per-light fold reads the box at its stage.
        uint8_t staticCount = 0;
        hasLive_ = false;
        Coord3D box{physicalWidth_, physicalHeight_, physicalDepth_};
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
            auto* m = static_cast<ModifierBase*>(child(i));
            if (m->hasModifyLive()) { hasLive_ = true; continue; }   // dynamic: per-frame, not baked
            m->modifyLogicalSize(box);
            clampLogical(box);
            staticCount++;
        }

        // Final logical box = the running box after the last static modifier.
        Coord3D logical = box;
        width_ = logical.x; height_ = logical.y; depth_ = logical.z;

        const Coord3D phys{physicalWidth_, physicalHeight_, physicalDepth_};
        const nrOfLightsType boxCount    = cellCount(phys);
        const nrOfLightsType logicalCount = cellCount(logical);
        const nrOfLightsType driverCount = physicalLightCount();   // == Layouts::totalLightCount()
        const bool dense = (driverCount == boxCount);
        // A gap fills a cell but must not receive its color, so the identity map is wrong here.
        const bool anyGap = layouts_ && layouts_->hasBlackPixels();

        // The frame-rate floor for the common case: box cell i is driver light i. Keep it first.
        if (staticCount == 0 && dense && !anyGap && isNaturalOrder()) {
            lut_.setIdentity(boxCount);
            allocateBuffer(boxCount);
            return;
        }

        // Each physical light contributes at most one destination, so driverCount is the ceiling.
        if (!buildFoldedLUT(logical, logicalCount, driverCount)) {
            // Degrade to identity rather than crashing, at the cost of lighting a gapped layout.
            lutSkipped_ = true;
            setStatus("modifier mapping skipped — not enough memory", Severity::Warning);
            width_ = physicalWidth_; height_ = physicalHeight_; depth_ = physicalDepth_;
            lut_.setIdentity(boxCount);
            allocateBuffer(boxCount);
            return;
        }
        allocateBuffer(logicalCount);
    }

    // Sentinel: a box cell that is not a real light (no driver index).
    static constexpr nrOfLightsType kNoDriver = static_cast<nrOfLightsType>(-1);

    // Measured over the same coords the build walks, so the coords stay the single source.
    /// Whether the layout emits lights in box order, which is what validates the dense fast path.
    bool isNaturalOrder() const {
        struct Ctx { lengthType w, h; bool ok; };
        Ctx ctx{physicalWidth_, physicalHeight_, true};
        // Reached only for a gap-free layout, so blackCb is null.
        layouts_->placeLights(CoordSink{[](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* k = static_cast<Ctx*>(c);
            if (!k->ok) return;   // once a mismatch is found the answer is settled; skip the rest
            nrOfLightsType box = static_cast<nrOfLightsType>(z) * k->w * k->h
                               + static_cast<nrOfLightsType>(y) * k->w + x;
            if (driverIdx != box) k->ok = false;
        }, nullptr, &ctx});
        return ctx.ok;
    }

    // A counting-sort CSR build: folding scatters, while setMapping demands sequential writes.
    bool buildFoldedLUT(const Coord3D& logical,
                        nrOfLightsType logicalCount, nrOfLightsType driverCount) {
        if (logicalCount == 0 || driverCount == 0) { lut_.setIdentity(0); return true; }

        // Each physical light yields at most one destination, the tight overflow-free ceiling.
        auto* counts = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(logicalCount + 1) * sizeof(nrOfLightsType)));
        auto* dests = static_cast<nrOfLightsType*>(
            platform::alloc(static_cast<size_t>(driverCount) * sizeof(nrOfLightsType)));
        if (!counts || !dests) {
            if (counts) platform::free(counts);
            if (dests) platform::free(dests);
            return false;
        }
        for (nrOfLightsType i = 0; i <= logicalCount; i++) counts[i] = 0;

        // One callback does both passes through the placeLights ctx, so it captures nothing.
        struct FoldCtx {
            Layer* self;   // for the dynamic child list (the modifier chain)
            Coord3D logical; nrOfLightsType logicalCount;  // final box, for the flatten + guard
            nrOfLightsType* counts;   // pass A: per-cell count.  pass B: per-cell write cursor.
            nrOfLightsType* dests;    // pass B only.
            nrOfLightsType destCap;   // what dests holds, which pass B must not exceed
            bool scatter;
        } fctx{this, logical, logicalCount, counts, dests, driverCount, /*scatter=*/false};

        auto onCoord = [](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
            auto* f = static_cast<FoldCtx*>(c);
            Coord3D pos{x, y, z};
            Layer* self = f->self;
            for (uint8_t i = 0; i < self->childCount(); i++) {
                if (self->child(i)->role() != ModuleRole::Modifier || !self->child(i)->enabled()) continue;
                auto* m = static_cast<ModifierBase*>(self->child(i));
                if (m->hasModifyLive()) continue;                 // dynamic: not in the static fold
                if (!m->modifyLogical(pos)) return;               // rejected: no logical source
            }
            if (pos.x < 0 || pos.x >= f->logical.x || pos.y < 0 || pos.y >= f->logical.y ||
                pos.z < 0 || pos.z >= f->logical.z) return;                          // defensive
            const nrOfLightsType li =
                static_cast<nrOfLightsType>(pos.z) * static_cast<nrOfLightsType>(f->logical.x) * static_cast<nrOfLightsType>(f->logical.y) +
                static_cast<nrOfLightsType>(pos.y) * static_cast<nrOfLightsType>(f->logical.x) +
                static_cast<nrOfLightsType>(pos.x);
            if (li >= f->logicalCount) return;                                       // defensive
            // The bound makes a disagreement between the two passes cost a destination, not memory.
            if (f->scatter) {
                const nrOfLightsType slot = f->counts[li];
                if (slot >= f->destCap) return;
                f->dests[slot] = driverIdx;
                f->counts[li]++;
            } else {
                f->counts[li]++;                                    // pass A: bump the count
            }
        };

        // A gap is dropped from the LUT and stays black: a wire slot present, with no source.
        static constexpr CoordCallback kDropGap =
            [](void*, nrOfLightsType, lengthType, lengthType, lengthType) {};

        // Pass A: count.
        layouts_->placeLights(CoordSink{onCoord, kDropGap, &fctx});

        // Prefix-sum counts → offsets (counts[li] becomes the start of cell li's run).
        nrOfLightsType running = 0;
        for (nrOfLightsType i = 0; i < logicalCount; i++) {
            nrOfLightsType c = counts[i];
            counts[i] = running;
            running += c;
        }
        counts[logicalCount] = running;   // total destinations

        // Pass B: scatter. counts[] is now the per-cell write cursor.
        fctx.scatter = true;
        layouts_->placeLights(CoordSink{onCoord, kDropGap, &fctx});

        // Each cell's cursor now holds its end offset, which is the next cell's start.
        if (!lut_.build(logicalCount, running)) {   // running == total destinations
            platform::free(counts);
            platform::free(dests);
            return false;
        }
        nrOfLightsType start = 0;
        for (nrOfLightsType i = 0; i < logicalCount; i++) {
            nrOfLightsType end = counts[i];          // end of cell i's run
            lut_.setMapping(i, &dests[start], static_cast<nrOfLightsType>(end - start));
            start = end;
        }
        lut_.finalize();
        platform::free(counts);
        platform::free(dests);
        return true;
    }

    // Cells in a box (the flat light count). 0 on any 0-extent axis.
    /// How many cells a box holds.
    static nrOfLightsType cellCount(const Coord3D& box) {
        return static_cast<nrOfLightsType>(box.x) * static_cast<nrOfLightsType>(box.y) *
               static_cast<nrOfLightsType>(box.z);
    }

    // A zero-width logical box would blank the layer, leaving no source for any effect.
    /// Hold a folded box inside its legal bounds, so a modifier cannot size it away.
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
    uint32_t elapsed_ = 0;
    uint8_t  fadeBy_ = 0;   // fade RATE collected from effects (MIN), consumed once at frame start
    uint32_t fadeCarry_ = 0;               // sub-unit fade remainder, so a high frame rate does not over-fade
    particles::FrameTime fadeTime_{60};    // elapsed-to-scale, the shared conversion
    uint32_t bufferGen_ = 0;   // bumped by every write to buffer_; see bufferGen()
    char statusBuf_[20] = {};  // "999×999×999" fits; owned (setStatus borrows the pointer)
    bool     hasLive_ = false;          // any enabled modifier animates per frame (gates the live pass)
    uint8_t* liveScratch_ = nullptr;    // snapshot for the live pass; allocated only when hasLive_
    size_t   liveScratchBytes_ = 0;

    // Check if heap can afford an allocation (returns true if unlimited or enough budget)
    static bool canAllocate(size_t bytesNeeded) {
        size_t availableHeap = platform::freeHeap();
        if (availableHeap == 0) return true; // desktop: unlimited
        size_t internalHeap = platform::freeInternalHeap();
        if (internalHeap > 0 && internalHeap <= platform::HEAP_RESERVE) return false;
        size_t budget = availableHeap > platform::HEAP_RESERVE ? availableHeap - platform::HEAP_RESERVE : 0;
        return budget >= bytesNeeded && platform::maxAllocBlock() >= bytesNeeded;
    }

    /// The channel count this layer's fixtures need, or 0 when nothing in the rig moves.
    uint8_t requiredChannels() const {
        const FixtureChannels& f = fixture_;
        uint8_t top = 0;
        for (uint8_t o : {f.pan, f.tilt, f.zoom, f.rotate, f.gobo})
            if (o != FixtureChannels::kAbsent && o + 1 > top) top = static_cast<uint8_t>(o + 1);
        return top;
    }

    void allocateBuffer(nrOfLightsType count) {
        // Widened before the allocation: changing the width afterwards resizes it under its holder.
        if (const uint8_t need = requiredChannels(); need > channelsPerLight_) channelsPerLight_ = need;

        // Try to allocate buffer, halve dimensions if needed
        bool reduced = false;
        while (count > 0) {
            size_t needed = static_cast<size_t>(count) * channelsPerLight_;
            if (canAllocate(needed)) {
                if (buffer_.allocate(count, channelsPerLight_)) {
                    setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
                    if (reduced) setStatus("buffer reduced — not enough memory", Severity::Warning);
                    return;
                }
                // allocate refused despite the canAllocate check, so degrade
                std::printf("  DEGRADE  buffer_.allocate failed for %u lights\n",
                            static_cast<unsigned>(count));
            }
            // Halve: reduce to sqrt of count (halve each dimension)
            width_ = width_ > 1 ? width_ / 2 : 1;
            height_ = height_ > 1 ? height_ / 2 : 1;
            depth_ = depth_ > 1 ? depth_ / 2 : 1;
            count = static_cast<nrOfLightsType>(width_) * height_ * depth_;
            reduced = true;
            std::printf("  DEGRADE  buffer too large, reducing to %dx%dx%d\n",
                        static_cast<int>(width_), static_cast<int>(height_), static_cast<int>(depth_));
            if (width_ <= 8 && height_ <= 8) break; // minimum
        }
        if (!buffer_.allocate(count, channelsPerLight_)) {
            std::printf("  DEGRADE  buffer_.allocate failed at minimum size %u\n",
                        static_cast<unsigned>(count));
            setStatus("buffer allocation failed — not enough memory", Severity::Error);
        } else if (reduced) {
            setStatus("buffer reduced — not enough memory", Severity::Warning);
        }
        setDynamicBytes(buffer_.bytes() + lut_.memoryUsed());
    }
};

// EffectBase accessor implementations
inline Layer* EffectBase::layer() const { return static_cast<Layer*>(parent()); }
inline uint8_t* EffectBase::buffer() { return layer()->buffer().data(); }
inline lengthType EffectBase::width() const { return layer()->width(); }
inline lengthType EffectBase::height() const { return layer()->height(); }
inline lengthType EffectBase::depth() const { return layer()->depth(); }
inline uint8_t EffectBase::channelsPerLight() const { return layer()->channelsPerLight(); }

// Never scaled by brightness, and a missing channel makes the write a no-op.
/// Write one non-color channel of light `index`, such as pan or tilt.
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
inline draw::Canvas EffectBase::canvas() {
    Layer* l = layer();
    return draw::Canvas::of(l->buffer(), l->width(), l->height(), l->depth());
}

} // namespace mm
