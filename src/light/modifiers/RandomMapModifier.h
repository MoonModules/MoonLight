#pragma once

#include "light/modifiers/ModifierBase.h"

#include "platform/platform.h"     // alloc / free / millis

namespace mm {

/// Modifier remapping every light through a random 1:1 permutation.
///
/// @moreinfo
///
/// Randomly remaps every light to another light, a true 1:1 permutation (every light goes somewhere, no gaps or duplicates), and reshuffles on a `bpm` timer.
/// A static fold whose mapping changes on a beat: modifyLogical applies the permutation (the box is unchanged, each light maps to exactly one other).
/// The bpm tick bumps the generation and rebuilds the Layer's mapping on a beat boundary, the path a control change takes, scoped to one Layer. A permutation is a discrete reshuffle rather than smooth motion, so a beat-gated rebuild is the right cost, not a per-frame remap.
///
/// ## How often it reshuffles
///
/// The rate control is reshuffles per minute, from none up to one a second.
/// At zero the permutation freezes and never reshuffles again.
///
/// Cost: each beat re-runs the Layer's mapping rebuild on the render thread (a transient one-frame hitch), bounded by bpm≤60.
/// The permutation buffer is a member sized to the box, (re)allocated only on a grid resize, never per frame.
/// An alloc failure degrades to identity passthrough.
///
/// Sparse layouts: the permutation is over box indices, so a real light can map to a non-light cell (dropped → dark).
/// Acceptable for v1.
/// Author: MoonLight, https://github.com/MoonModules/projectMM/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h
class RandomMapModifier : public ModifierBase {
public:
    /// The catalog tags this modifier carries.
    const char* tags() const override { return "💫"; }
    /// Shuffles within a box that spans all three axes.
    Dim dimensions() const override { return Dim::D3; }
    /// Reshuffles per minute (0–60); 6 ≈ every 10 s; 0 = frozen.
    uint8_t bpm = 6;

    /// Releases the permutation buffers.
    ~RandomMapModifier() override { releasePerm(); }

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
    }

    // The rate is read live each tick, so changing it needs no rebuild.
    /// Whether a change here forces the mapping to rebuild.
    bool affectsPrepare(const char* /*controlName*/) const override { return false; }

    // A remap leaves the box unchanged but needs it for the permutation, stash it.
    /// Resize the logical box this modifier presents to the effect.
    void modifyLogicalSize(Coord3D& size) override { box_ = size; }

    // A coordinate flattens to an index, looks up its permuted one, and unflattens back.
    /// Transform one light's logical position, false dropping it from the mapping.
    bool modifyLogical(Coord3D& pos) const override {
        const nrOfLightsType boxCount =
            static_cast<nrOfLightsType>(box_.x) * static_cast<nrOfLightsType>(box_.y) *
            static_cast<nrOfLightsType>(box_.z);
        ensurePermutation(boxCount);

        const lengthType w = box_.x > 0 ? box_.x : 1;
        const lengthType h = box_.y > 0 ? box_.y : 1;
        const nrOfLightsType idx =
            static_cast<nrOfLightsType>(pos.z) * static_cast<nrOfLightsType>(w) * static_cast<nrOfLightsType>(h) +
            static_cast<nrOfLightsType>(pos.y) * static_cast<nrOfLightsType>(w) +
            static_cast<nrOfLightsType>(pos.x);

        // Permuted index, or identity if the permutation is unavailable (OOM/empty).
        const nrOfLightsType mapped = (perm_ && idx < permCount_) ? perm_[idx] : idx;
        // Unflatten back to a coordinate in the box.
        pos.x = static_cast<lengthType>(mapped % w);
        pos.y = static_cast<lengthType>((mapped / w) % h);
        pos.z = static_cast<lengthType>(mapped / (static_cast<nrOfLightsType>(w) * h));
        return true;   // a permutation never rejects — every light maps somewhere
    }

    // On each beat the generation bumps, which is what makes the next rebuild reshuffle.
    void tick() MM_NONBLOCKING override {
        const uint32_t now = platform::millis();
        if (lastElapsed_ == 0) lastElapsed_ = now;   // first tick: no dt jump
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        if (bpm == 0) return;                   // frozen — keep the current permutation
        // The raw product accumulates and divides only at the read, so a sub-millisecond step survives.
        phaseNum_ += static_cast<uint64_t>(dt) * bpm;
        const uint64_t beat = phaseNum_ / 60000u;
        if (beat != lastBeat_) {
            lastBeat_ = beat;
            reshuffle();              // bump the generation; the Layer's rebuild applies it
            needsRebuild_ = true;     // Layer::tick() reads + clears this, one rebuild/frame
        }
    }

    /// Whether a beat has asked for a fresh permutation, which the Layer polls and coalesces.
    bool consumeNeedsRebuild() override {
        const bool r = needsRebuild_;
        needsRebuild_ = false;
        return r;
    }

    /// Bump the generation so the next rebuild reshuffles, which a test can drive directly.
    void reshuffle() { generation_++; }

private:
    // Rebuilt when the box count changed or a beat bumped the generation.
    void ensurePermutation(nrOfLightsType boxCount) const {
        if (boxCount == 0) { releasePerm(); return; }
        if (boxCount != permCount_ || !perm_) {
            releasePerm();
            perm_ = static_cast<nrOfLightsType*>(
                platform::alloc(static_cast<size_t>(boxCount) * sizeof(nrOfLightsType)));
            if (!perm_) { permCount_ = 0; return; }   // OOM → mapToPhysical passes through identity
            permCount_ = boxCount;
            builtGen_ = generation_ - 1;              // force a shuffle below
        }
        if (builtGen_ != generation_) {
            shuffle();
            builtGen_ = generation_;
        }
    }

    // Fisher-Yates from the identity, so the result is always a true bijection.
    void shuffle() const {
        for (nrOfLightsType i = 0; i < permCount_; i++) perm_[i] = i;
        rngState_ = 0xBADF00Du ^ (generation_ * 2654435761u);
        // i from high to low; swap perm_[i-1] with perm_[j], j in [0, i).
        for (nrOfLightsType i = permCount_; i > 1; i--) {
            const nrOfLightsType j = static_cast<nrOfLightsType>(rand() % i);
            const nrOfLightsType tmp = perm_[i - 1];
            perm_[i - 1] = perm_[j];
            perm_[j] = tmp;
        }
    }

    // LCG (glibc constants), same as ParticlesEffect, fast, integer, deterministic.
    uint32_t rand() const {
        rngState_ = rngState_ * 1103515245u + 12345u;
        return rngState_ >> 8;   // drop the low bits (poor randomness in an LCG)
    }

    void releasePerm() const {
        if (perm_) { platform::free(perm_); perm_ = nullptr; }
        permCount_ = 0;
    }

    /// Mutable: mapToPhysical is const but lazily (re)builds the permutation.
    Coord3D box_;   // stashed in modifyLogicalSize (the box the permutation is over)
    mutable nrOfLightsType* perm_ = nullptr;
    mutable nrOfLightsType  permCount_ = 0;
    mutable uint32_t        rngState_ = 0xBADF00Du;
    mutable uint32_t        generation_ = 1;   // bumped per beat; shuffle keys off it
    mutable uint32_t        builtGen_ = 0;     // generation the current perm_ was built for

    /// Dt*bpm accumulator (numerator; one beat per 60000).
    uint64_t phaseNum_ = 0;
    uint64_t lastBeat_ = 0;
    uint32_t lastElapsed_ = 0;
    /// Set on a beat, consumed by Layer::tick (coalesced rebuild).
    bool     needsRebuild_ = false;
};

} // namespace mm
