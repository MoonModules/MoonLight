/// The bodies of Layer.h with logic beyond a line: the cold path (layout, mapping, buffer), the live pass and the extrusion, so a change to any of them rebuilds one object.

#include "light/layers/Layer.h"

#include <cstdint>  // INT32_MAX: the bound on the warp's 16.16 steps
#include <cstdio>
#include <cstring>  // std::memcpy, std::memset

namespace mm {

Layer::~Layer() { if (liveScratch_) platform::free(liveScratch_); }

void Layer::defineControls() {
    static constexpr const char* kBlendModeOptions[] = {"alpha", "additive", "multiply", "screen", "darken", "lighten",
                                                        "difference", "exclusion"};
    static_assert(sizeof(kBlendModeOptions) / sizeof(kBlendModeOptions[0]) == kBlendModeCount, "one option per mode");
    controls_.addSelect("blendMode", blendMode, kBlendModeOptions, kBlendModeCount);
    controls_.addControl("opacity", opacity, 0, 255);
    // Cascade to the children, preserving the base behavior this overrode.
    MoonModule::defineControls();
}

void Layer::prepare() {
    // Decided here, on the cold path, so the tick tests a bool: a bare layer and one under a non-container parent apply their own pending work.
    selfApplies_ = !parent() || !parent()->acceptsRole(ModuleRole::Layer);
    // Restart discards the elapsed gap, so the first tick after a re-prepare cannot jump a trail.
    fadeTime_.reset();
    frameStarted_ = false;
    fadeCarry_ = fadeDue_ = 0;
    // No layouts wired reads the same as every layout disabled: the layer ends up empty.
    const nrOfLightsType physicalCount = layouts_ ? layouts_->totalLightCount() : 0;

    // Tear the old state down: a stale LUT beside a zero-byte buffer makes blendMap fault.
    if (physicalCount == 0) {
        physicalWidth_ = physicalHeight_ = physicalDepth_ = 0;
        width_ = height_ = depth_ = 0;
        lut_.free();
        buffer_.free();
        setDynamicBytes({});
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

void Layer::applyPending() {
    if (rebuildPending_) {
        rebuildPending_ = false;
        applyState();
    }
    for (uint8_t i = 0; i < childCount(); i++)
        if (child(i)->role() == ModuleRole::Effect) static_cast<EffectBase*>(child(i))->applyPending();
}

void Layer::ensureLiveScratch() {
    const size_t bytes = hasLive_ ? buffer_.bytes() : 0;
    if (bytes == liveScratchBytes_ && (bytes != 0) == (liveScratch_ != nullptr)) return;
    if (liveScratch_) { platform::free(liveScratch_); liveScratch_ = nullptr; }
    liveScratchBytes_ = 0;
    if (bytes == 0) return;                       // no live modifier → no scratch held
    liveScratch_ = static_cast<uint8_t*>(platform::alloc(bytes));
    if (liveScratch_) liveScratchBytes_ = bytes;  // alloc-fail → applyLivePass no-ops, static frame shows
}

/// One light of the live pass, inlined into applyLivePass's loop so it stays a step, not a call (coding-standards, "A function split out of a per-light loop").
inline __attribute__((always_inline)) void Layer::gatherLiveCell(uint8_t* buf, const Coord3D& cell, const Coord3D& logical) {
    Coord3D src = cell;
    for (uint8_t i = 0; i < childCount(); i++) {
        if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
        auto* m = static_cast<ModifierBase*>(child(i));
        if (m->hasModifyLive()) m->modifyLive(src, logical);
    }
    const size_t cpl = channelsPerLight_;
    const size_t dstIdx = (static_cast<size_t>(cell.z) * height_ * width_ +
                           static_cast<size_t>(cell.y) * width_ + cell.x) * cpl;
    if (src.isInside(logical)) {
        const size_t srcIdx = (static_cast<size_t>(src.z) * height_ * width_ +
                               static_cast<size_t>(src.y) * width_ + src.x) * cpl;
        std::memcpy(buf + dstIdx, liveScratch_ + srcIdx, cpl);
    } else {
        std::memset(buf + dstIdx, 0, cpl);   // source off-box → dark
    }
}

void Layer::applyLivePass() {
    uint8_t* buf = buffer_.data();
    if (!buf || !liveScratch_) return;   // scratch is sized on the cold path (ensureLiveScratch)
    const size_t cpl = channelsPerLight_;
    const size_t bytes = static_cast<size_t>(width_) * height_ * depth_ * cpl;
    if (bytes == 0 || bytes > liveScratchBytes_) return;   // hot path NEVER allocates
    std::memcpy(liveScratch_, buf, bytes);   // snapshot the source frame

    const Coord3D logical{width_, height_, depth_};
    if (warpAffine(logical)) return;   // a chain of rotations, stepped per row
    for (lengthType z = 0; z < depth_; z++)
        for (lengthType y = 0; y < height_; y++)
            for (lengthType x = 0; x < width_; x++) gatherLiveCell(buf, Coord3D{x, y, z}, logical);
}

bool Layer::warpAffine(const Coord3D& logical) {
    LiveAffine t;
    if (!composeLive(t, logical) || !stepsFit(t)) return false;
    const size_t slice = static_cast<size_t>(width_) * height_ * channelsPerLight_;
    for (lengthType z = 0; z < depth_; z++)
        for (lengthType y = 0; y < height_; y++)
            warpRow(t, y, liveScratch_ + static_cast<size_t>(z) * slice, buffer_.data() + static_cast<size_t>(z) * slice);
    return true;
}

bool Layer::composeLive(LiveAffine& t, const Coord3D& logical) const {
    for (uint8_t i = 0; i < childCount(); i++) {
        if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
        const auto* m = static_cast<const ModifierBase*>(child(i));
        if (!m->hasModifyLive()) continue;
        LiveAffine mi;
        if (!m->liveAffine(mi, logical)) return false;
        t = mi.after(t);
    }
    return true;
}

bool Layer::stepsFit(const LiveAffine& t) const {
    constexpr int64_t kLimit = INT32_MAX - 0x8000;   // the rounding is added to the start
    for (const int64_t x : {int64_t{0}, int64_t{width_}})
        for (const int64_t y : {int64_t{0}, int64_t{height_}}) {
            const int64_t sx = t.a * x + t.b * y + t.c, sy = t.d * x + t.e * y + t.f;
            if (sx > kLimit || sx < -kLimit || sy > kLimit || sy < -kLimit) return false;
        }
    return true;
}

void Layer::warpRow(const LiveAffine& t, lengthType y, const uint8_t* src, uint8_t* slice) const {
    const size_t cpl = channelsPerLight_;
    uint8_t* dst = slice + static_cast<size_t>(y) * width_ * cpl;
    int32_t sx = static_cast<int32_t>(int64_t{t.b} * y + t.c + 0x8000), sy = static_cast<int32_t>(int64_t{t.e} * y + t.f + 0x8000);   // the row's first source, rounding folded in
    for (lengthType x = 0; x < width_; x++, sx += t.a, sy += t.d, dst += cpl) {
        const int32_t ix = sx >> 16, iy = sy >> 16;
        if (ix < 0 || ix >= width_ || iy < 0 || iy >= height_) { std::memset(dst, 0, cpl); continue; }
        const uint8_t* from = src + (static_cast<size_t>(iy) * width_ + ix) * cpl;
        if (cpl == 3) { dst[0] = from[0]; dst[1] = from[1]; dst[2] = from[2]; }
        else std::memcpy(dst, from, cpl);
    }
}

void Layer::extrude(Dim effectDim) MM_NONBLOCKING {
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

EffectBase* Layer::soleEffect() const MM_NONBLOCKING {
    EffectBase* source = nullptr;
    for (uint8_t i = 0; i < childCount(); i++) {
        MoonModule* c = child(i);
        if (!c->enabled() || (c->role() != ModuleRole::Modifier && c->role() != ModuleRole::Effect)) continue;
        if (c->role() == ModuleRole::Modifier || source) return nullptr;
        source = static_cast<EffectBase*>(c);
    }
    return source;
}

bool Layer::soleLayer() const MM_NONBLOCKING {
    const MoonModule* p = parent();
    if (!p) return true;
    uint8_t layers = 0;
    for (uint8_t i = 0; i < p->childCount(); i++)
        if (p->child(i)->role() == ModuleRole::Layer && p->child(i)->enabled()) layers++;
    return layers == 1;
}

void Layer::placeByLayout(nrOfLightsType* out, size_t cells, Coord3D box) const {
    for (size_t c = 0; c < cells; c++) out[c] = kNoDriver;
    if (!layouts_) return;
    struct Ctx { nrOfLightsType* out; size_t cells; Coord3D box; };
    Ctx ctx{out, cells, box};
    // A gap stays dark and claims no cell, so a light at the same place shows; a position outside the box has no cell.
    layouts_->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
        auto* k = static_cast<Ctx*>(c);
        const Coord3D pos{x, y, z};
        if (!pos.isInside(k->box)) return;   // checked per axis: a negative axis would wrap to a valid cell
        const uint64_t cell = pos.indexIn(k->box);
        if (cell < k->cells && k->out[cell] == kNoDriver) k->out[cell] = idx;
    }, kDropGap, &ctx});
}

Coord3D Layer::foldBox() {
    // Each modifier stashes its output size, so the per-light fold reads the box at its stage.
    staticCount_ = 0;
    hasLive_ = false;
    Coord3D box{physicalWidth_, physicalHeight_, physicalDepth_};
    for (uint8_t i = 0; i < childCount(); i++) {
        if (child(i)->role() != ModuleRole::Modifier || !child(i)->enabled()) continue;
        auto* m = static_cast<ModifierBase*>(child(i));
        if (m->hasModifyLive()) { hasLive_ = true; continue; }   // dynamic: per-frame, not baked
        m->modifyLogicalSize(box);
        clampLogical(box);
        staticCount_++;
    }
    return box;
}

void Layer::rebuildLUT() {
    lutSkipped_ = false;
    clearStatus();  // re-evaluated below if a degrade path is taken

    // Final logical box = the running box after the last static modifier.
    const Coord3D logical = foldBox();
    width_ = logical.x; height_ = logical.y; depth_ = logical.z;

    const Coord3D phys{physicalWidth_, physicalHeight_, physicalDepth_};
    const nrOfLightsType boxCount    = cellCount(phys);
    const nrOfLightsType logicalCount = cellCount(logical);
    const nrOfLightsType driverCount = physicalLightCount();   // == Layouts::totalLightCount()
    const bool dense = (driverCount == boxCount);
    // A gap fills a cell but must not receive its color, so the identity map is wrong here.
    const bool anyGap = layouts_ && layouts_->hasGaps();

    // The frame-rate floor for the common case: box cell i is driver light i. Keep it first.
    if (staticCount_ == 0 && dense && !anyGap && isNaturalOrder()) {
        lut_.setIdentity(boxCount);
        allocateBuffer(boxCount);
        return;
    }

    // Each physical light contributes at most one destination, so driverCount is the ceiling.
    if (!buildFoldedLUT(logical, logicalCount, driverCount)) {
        // Degrade to identity rather than crashing, at the cost of lighting a gapped layout.
        lutSkipped_ = true;
        setStatus("modifier mapping skipped: not enough memory", Severity::Warning);
        width_ = physicalWidth_; height_ = physicalHeight_; depth_ = physicalDepth_;
        lut_.setIdentity(boxCount);
        allocateBuffer(boxCount);
        return;
    }
    allocateBuffer(logicalCount);
}

bool Layer::isNaturalOrder() const {
    struct Ctx { Coord3D box; bool ok; };
    Ctx ctx{{physicalWidth_, physicalHeight_, physicalDepth_}, true};
    // A gap reaches this callback too, since gapCb is null: out of place it breaks the order like any light.
    layouts_->placeLights(CoordSink{[](void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
        auto* k = static_cast<Ctx*>(c);
        if (!k->ok) return;   // once a mismatch is found the answer is settled; skip the rest
        // A light outside the box breaks the order however its index would wrap, so it answers false rather than being skipped.
        if (!Coord3D{x, y, z}.isAtIndex(driverIdx, k->box)) k->ok = false;
    }, nullptr, &ctx});
    return ctx.ok;
}

namespace {

/// One callback does both passes through the placeLights ctx, so it captures nothing.
struct FoldCtx {
    Layer* self;   // for the dynamic child list (the modifier chain)
    MappingLUT* lut;
    Coord3D logical; nrOfLightsType logicalCount;  // final box, for the flatten + guard
    bool place;    // the second pass, which places what the first counted
};

/// Whether `pos` survives every static modifier's fold, moved to its logical source.
bool foldStatic(Layer* self, Coord3D& pos) {
    for (uint8_t i = 0; i < self->childCount(); i++) {
        if (self->child(i)->role() != ModuleRole::Modifier || !self->child(i)->enabled()) continue;
        auto* m = static_cast<ModifierBase*>(self->child(i));
        if (m->hasModifyLive()) continue;                 // dynamic: not in the static fold
        if (!m->modifyLogical(pos)) return false;         // rejected: no logical source
    }
    return true;
}

void foldCoord(void* c, nrOfLightsType driverIdx, lengthType x, lengthType y, lengthType z) {
    auto* f = static_cast<FoldCtx*>(c);
    Coord3D pos{x, y, z};
    if (!foldStatic(f->self, pos)) return;
    if (!pos.isInside(f->logical)) return;                                    // defensive
    const uint64_t cell = pos.indexIn(f->logical);
    if (cell >= f->logicalCount) return;                                     // defensive, before the index narrows
    const auto li = static_cast<nrOfLightsType>(cell);
    if (f->place) f->lut->place(li, driverIdx);
    else f->lut->count(li);
}

}  // namespace

bool Layer::buildFoldedLUT(const Coord3D& logical,
                           nrOfLightsType logicalCount, nrOfLightsType driverCount) {
    if (logicalCount == 0 || driverCount == 0) { lut_.setIdentity(0); return true; }
    if (!lut_.beginBuild(logicalCount)) return false;

    FoldCtx fctx{this, &lut_, logical, logicalCount, /*place=*/false};
    layouts_->placeLights(CoordSink{foldCoord, kDropGap, &fctx});
    if (!lut_.startRuns()) return false;
    fctx.place = true;
    layouts_->placeLights(CoordSink{foldCoord, kDropGap, &fctx});
    lut_.endBuild();
    return true;
}

bool Layer::canAllocate(size_t bytesNeeded) {
    size_t availableHeap = platform::freeHeap();
    if (availableHeap == 0) return true; // host: unlimited
    size_t internalHeap = platform::freeInternalHeap();
    if (internalHeap > 0 && internalHeap <= platform::HEAP_RESERVE) return false;
    size_t budget = availableHeap > platform::HEAP_RESERVE ? availableHeap - platform::HEAP_RESERVE : 0;
    return budget >= bytesNeeded && platform::maxAllocBlock() >= bytesNeeded;
}

uint8_t Layer::requiredChannels() const {
    const FixtureChannels& f = fixture_;
    uint8_t top = 0;
    for (uint8_t o : {f.pan, f.tilt, f.zoom, f.rotate, f.gobo})
        if (o != FixtureChannels::kAbsent && o + 1 > top) top = static_cast<uint8_t>(o + 1);
    return top;
}

bool Layer::tryAllocate(nrOfLightsType count) {
    if (!buffer_.holds(count, channelsPerLight_) && !canAllocate(static_cast<size_t>(count) * channelsPerLight_)) return false;
    if (buffer_.allocate(count, channelsPerLight_)) return true;
    // allocate refused despite the canAllocate check, so degrade
    std::printf("  DEGRADE  buffer_.allocate failed for %u lights\n", static_cast<unsigned>(count));
    return false;
}

nrOfLightsType Layer::halveBox() {
    // Halve each dimension, which quarters a plane and eighths a cube.
    width_ = width_ > 1 ? width_ / 2 : 1;
    height_ = height_ > 1 ? height_ / 2 : 1;
    depth_ = depth_ > 1 ? depth_ / 2 : 1;
    std::printf("  DEGRADE  buffer too large, reducing to %dx%dx%d\n",
                static_cast<int>(width_), static_cast<int>(height_), static_cast<int>(depth_));
    return static_cast<nrOfLightsType>(width_) * height_ * depth_;
}

void Layer::allocateBuffer(nrOfLightsType count) {
    // Widened before the allocation: changing the width afterwards resizes it under its holder.
    if (const uint8_t need = requiredChannels(); need > channelsPerLight_) channelsPerLight_ = need;

    // Try to allocate buffer, halve dimensions if needed
    bool reduced = false;
    while (count > 0) {
        if (tryAllocate(count)) {
            setDynamicBytes(buffer_.heap() + lut_.heap());
            if (reduced) setStatus("buffer reduced: not enough memory", Severity::Warning);
            return;
        }
        count = halveBox();
        reduced = true;
        if (width_ <= 8 && height_ <= 8) break; // minimum
    }
    if (!buffer_.allocate(count, channelsPerLight_)) {
        std::printf("  DEGRADE  buffer_.allocate failed at minimum size %u\n",
                    static_cast<unsigned>(count));
        setStatus("buffer allocation failed: not enough memory", Severity::Error);
    } else if (reduced) {
        setStatus("buffer reduced: not enough memory", Severity::Warning);
    }
    setDynamicBytes(buffer_.heap() + lut_.heap());
}

}  // namespace mm
