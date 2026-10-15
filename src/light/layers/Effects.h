#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // topOfType: the Layouts it finds when nothing injected one
#include "light/moonlive/MoonLivePalette.h"   // the scripted palette, run before the layers
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "platform/platform.h"

namespace mm {

/// The container holding every rendering layer, which `Drivers` composites into one output.
/// @card Effects.png
///
/// Each layer renders independently into its own buffer, over the physical topology the shared `Layouts` describes.
///
/// @technical
///
/// **Why a container.** Multi-layer composition needs one place that walks every layer in order. With a single child this is a pass-through, matching the one-layer pipeline exactly.
///
/// **It owns no buffer.** Each layer owns its buffer and `Drivers` owns the composited output. Three queries serve the compositor: the active layer, the enabled count, and a walk in order.
class Effects : public MoonModule {
public:
    /// The child role this container accepts, which is layers alone.
    const char* acceptsChildRoles() const override { return "layer"; }

    /// Wire the shared `Layouts` into every child layer, so each can size its buffer; idempotent, so adding a layer child is followed by calling it again.
    void setLayouts(Layouts* l) {
        layouts_ = l;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer) continue;
            static_cast<Layer*>(c)->setLayouts(layouts_);
        }
    }

    /// The shared `Layouts` this container hands to its children.
    Layouts* layouts() const { return layouts_; }

    /// Re-wire the children before they build, so a layer added through the API is wired too.
    void prepare() override {
        // Found by type when nothing injected it, so a tree built from a document needs no wiring.
        if (!layouts_)
            if (Scheduler* s = Scheduler::instance()) layouts_ = static_cast<Layouts*>(s->topOfType("Layouts"));
        setLayouts(layouts_);
    }

    /// Tick every enabled layer child, in container order; role-filtered, since a stray child ticked here would run at the wrong depth.
    void tick() MM_NONBLOCKING override {
        // First, so every layer this frame samples the same entries: Drivers ticks too late for it.
        MoonLivePalette::tickActive(platform::millis());
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer) continue;
            if (c->respectsEnabled() && !c->enabled()) continue;
            uint32_t start = platform::micros();
            c->tick();
            c->addAccumUs(platform::micros() - start);
        }
    }

    /// The first enabled layer, which `Drivers` reads for the physical dimensions; falls back to a disabled layer so geometry stays queryable while everything is off.
    Layer* activeLayer() const {
        MoonModule* fallback = nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer) continue;
            if (!fallback) fallback = c;
            if (c->enabled()) return static_cast<Layer*>(c);
        }
        return static_cast<Layer*>(fallback);  // nullptr if no Layer children
    }

    /// The first enabled layer, or null when none is enabled; output selection uses this one since a disabled layer's buffer would keep emitting its last frame.
    Layer* firstEnabledLayer() const {
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer || !c->enabled()) continue;
            return static_cast<Layer*>(c);
        }
        return nullptr;
    }

    /// How many layer children are enabled, which chooses the fast or the composite path.
    uint8_t enabledLayerCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (c && c->role() == ModuleRole::Layer && c->enabled()) n++;
        }
        return n;
    }

    /// Walk the enabled layers in composition order, bottom to top; the callback's second argument marks the bottom layer, which clears the buffer.
    template <typename Fn>
    void forEachEnabledLayer(Fn cb) const {
        bool first = true;
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (!c || c->role() != ModuleRole::Layer || !c->enabled()) continue;
            cb(static_cast<Layer*>(c), first);
            first = false;
        }
    }

private:
    Layouts* layouts_ = nullptr;
};

} // namespace mm
