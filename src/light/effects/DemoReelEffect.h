#pragma once

#include "light/effects/EffectBase.h"

#include "core/util/ModuleFactory.h"      // enumerate + create the effects to cycle through
#include "light/powerfunctions/fonts.h"             // fonts::kFont4x6: the overlay font

namespace mm {

/// Showcase effect: cycles through other effects with a name overlay.
/// @card DemoReelEffect.gif
/// Author: MoonLight original, on Mark Kriegsman's FastLED DemoReel100 pattern, https://github.com/FastLED/FastLED/blob/master/examples/DemoReel100/DemoReel100.ino
///
/// Cycles through every other registered effect, advancing every `interval` seconds.
/// It hosts one live child at a time, created from the registry and parented to this Layer.
/// So the reel is an effect that swaps which effect it is over time.
///
/// Prior art: FastLED's DemoReel100 sketch, with the registry-driven, self-skipping variant ours.
///
/// @moreinfo
///
/// ## It sequences rather than composites
///
/// The Layer stack and its blend modes already composite, so the reel plays effects in turn.
/// The child's own controls stay at their defaults, and the reel exposes only the cycle controls.
///
/// ## The child rides the normal lifecycle
///
/// A swap tears the child down, deletes it, and creates the next one.
/// That child sees the Layer as its parent, so its buffer, extents and clock resolve to this target.
/// It runs the same create, defineControls, setup, prepare and loop lifecycle a Layer gives any effect.
class DemoReelEffect : public EffectBase {
public:
    /// Catalog tags: the demo reel.
    const char* tags() const override { return "💫"; }
    /// D3, since the reel produces a complete frame and extrudes the child itself before the overlay.
    Dim dimensions() const override { return Dim::D3; }

    /// Seconds each effect plays before the reel advances.
    uint8_t interval      = 8;
    /// Pick the next effect at random rather than in registry order.
    bool    shuffle       = false;
    /// Pick a fresh palette on each cycle, which showcases the palette set.
    bool    randomPalette = true;
    /// Overlay the playing effect's name, which is what makes the reel a showcase tool.
    bool    showName      = true;

    /// Publish the cycle interval, the order, the palette and the overlay.
    void defineControls() override {
        controls_.addControl("interval", interval, 1, 120);
        controls_.addControl("shuffle", shuffle);
        controls_.addControl("randomPalette", randomPalette);
        controls_.addControl("showName", showName);
    }

    /// List every eligible effect and stand the first one up.
    void prepare() override {
        buildEligibleList();
        // A rebuild re-creates the child, whose buffers were sized to the old grid.
        swapTo(cursor_ < eligibleCount_ ? cursor_ : 0);
    }

    /// Advance on the interval, run the hosted child, extrude it, then draw the name.
    void tick() MM_NONBLOCKING override {
        if (eligibleCount_ == 0 || !current_) return;

        // elapsed() is the Layer's clock, so the cadence holds at any frame rate.
        const uint32_t now = elapsed();
        if (now - lastSwitchMs_ >= static_cast<uint32_t>(interval) * 1000u) {
            lastSwitchMs_ = now;
            advance();
        }

        current_->tick();   // render the hosted effect into our Layer's buffer
        // Extruded here, before the overlay, from the dim swapTo cached.
        layer()->extrude(currentDim_);

        // On top of the hosted output, where draw::text clips to whatever the grid fits.
        if (showName && current_) {
            const draw::Canvas cv = canvas();
            draw::text(cv, fonts::kFont4x6, current_->name(), 0, 0, {255, 255, 255});
        }
    }

    /// Tear the hosted child down, then chain so this effect's own state is freed.
    void release() override {
        destroyCurrent();
        EffectBase::release();
    }

    /// Delete the hosted child with the reel.
    ~DemoReelEffect() override { destroyCurrent(); }

    /// Test seam: how many effects the reel found to cycle through.
    uint8_t eligibleCountForTest() const { return eligibleCount_; }
    /// Test seam: the type name of the effect currently playing.
    const char* currentTypeForTest() const { return current_ ? current_->typeName() : nullptr; }
    /// Test seam: advance the reel without waiting out the interval.
    void advanceForTest() { advance(); }

private:
    /// The eligible list's ceiling: a byte per effect, so it stays inline.
    static constexpr uint8_t kMaxEligible = 64;
    uint8_t eligible_[kMaxEligible] = {};   ///< registry indices of every Effect-role type but this one
    uint8_t eligibleCount_ = 0;          ///< how many of those were found
    uint8_t cursor_ = 0;                 ///< which entry of `eligible_` is playing
    MoonModule* current_ = nullptr;      ///< the live hosted effect, owned and deleted on swap
    Dim currentDim_ = Dim::D3;           ///< the child's dimensionality, cached to drive the extrude
    uint32_t lastSwitchMs_ = 0;          ///< when the reel last advanced
    Random8 rng_;                        ///< the shuffle and palette picks

    /// Collect every Effect-role type in the registry except this one.
    void buildEligibleList() {
        eligibleCount_ = 0;
        const uint8_t n = ModuleFactory::typeCount();
        for (uint8_t i = 0; i < n && eligibleCount_ < kMaxEligible; i++) {
            if (ModuleFactory::typeRole(i) != ModuleRole::Effect) continue;
            const char* name = ModuleFactory::typeName(i);
            if (name && std::strcmp(name, "DemoReelEffect") == 0) continue;  // never host ourselves
            eligible_[eligibleCount_++] = i;
        }
    }

    /// Move to the next effect, in registry order or at random.
    void advance() {
        if (eligibleCount_ == 0) return;
        uint8_t next;
        if (shuffle && eligibleCount_ > 1) {
            do { next = rng_.below(eligibleCount_); } while (next == cursor_);  // don't repeat in place
        } else {
            next = static_cast<uint8_t>((cursor_ + 1) % eligibleCount_);
        }
        // This overrides the global palette while the reel runs, which a later rebuild restores.
        if (randomPalette && palettes::kCount > 0) Palettes::setActive(rng_.below(palettes::kCount));
        swapTo(next);
    }

    /// Tear the current child down and stand up the one at `which`, wired to our Layer.
    void swapTo(uint8_t which) {
        destroyCurrent();
        if (which >= eligibleCount_) return;
        cursor_ = which;
        const char* typeName = ModuleFactory::typeName(eligible_[which]);
        MoonModule* mod = ModuleFactory::create(typeName);
        if (!mod) return;
        // Parented to the Layer, or the child's buffer and extents fail to resolve.
        mod->setParent(layer());
        mod->defineControls();
        mod->setup();
        mod->applyState();   // build if effectively-enabled (walks to the Layer parent), else release
        current_ = mod;
        // From the factory, never a downcast: an Effect-role type need not be an EffectBase.
        const uint8_t d = ModuleFactory::typeDim(eligible_[which]);
        currentDim_ = (d == 1) ? Dim::D1 : (d == 2) ? Dim::D2 : Dim::D3;
        lastSwitchMs_ = elapsed();
        // No clear on a switch, so the incoming effect settles over the outgoing one.
        refreshStatus();
    }

    /// Release and delete the hosted child, the Scheduler's ownership pattern.
    void destroyCurrent() {
        if (!current_) return;
        current_->release();
        delete current_;
        current_ = nullptr;
    }

    /// Show which effect is playing, and where it sits in the reel.
    void refreshStatus() {
        if (current_) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "playing: %s (%u/%u)",
                          current_->name(), static_cast<unsigned>(cursor_ + 1),
                          static_cast<unsigned>(eligibleCount_));
            setStatus(statusBuf_);
        }
    }

    char statusBuf_[48] = {};   ///< setStatus holds the pointer, so this outlives the call
};

}  // namespace mm
