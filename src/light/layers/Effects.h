#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // topOfType: the Layouts it finds by type, when unset
#include "core/util/WorkerPool.h"    // the layers rendered side by side
#include "light/moonlive/MoonLivePalette.h"   // the scripted palette, run before the layers
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "platform/platform_clock.h"   // millis, micros
#include "platform/platform_system.h"   // cpuCores

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
        // Found by type when unset, so a tree built from a document wires itself.
        if (!layouts_)
            if (Scheduler* s = Scheduler::instance()) layouts_ = static_cast<Layouts*>(s->topOfType("Layouts"));
        setLayouts(layouts_);
    }

    /// Start the render workers: one thread a core, the render loop among them.
    void setup() override {
        MoonModule::setup();
        // The stack, priority and core a FreeRTOS worker would take, under the drivers, audio and network on the second core; no board starts one yet (renderThreads 1).
        pool_.start(static_cast<uint8_t>(platform::cpuCores() - 1), 12288, 4, 1);
    }

    /// How many render workers run beside the render loop: a computer's cores less one, none on a device.
    uint8_t renderWorkers() const { return pool_.workers(); }

    /// The layers' time per frame over the last second, by lane: on the render loop, on a render worker (averaged over the workers), and the render loop waiting for the last worker.
    struct LayerLoad {
        uint32_t hereUs = 0;     ///< layers the render loop ran itself
        uint32_t workerUs = 0;   ///< layers per render worker
        uint32_t joinUs = 0;     ///< the render loop waiting for the workers
    };
    /// The layer load the last second averaged.
    LayerLoad layerLoad() const { return load_; }
    /// Average the pool's load with the module times, over the same frames.
    void publishTiming(uint32_t frameCount) MM_NONBLOCKING override {
        MoonModule::publishTiming(frameCount);
        const WorkerPool::Load l = pool_.takeLoad();
        const uint32_t frames = frameCount ? frameCount : 1, workers = pool_.workers() ? pool_.workers() : 1;
        load_ = {l.callerUs / frames, l.workersUs / frames / workers, l.joinUs / frames};
    }

    /// Tick every enabled layer child, side by side on the render workers; role-filtered, since a stray child ticked here would run at the wrong depth.
    void tick() MM_NONBLOCKING override {
        // First, so every layer this frame samples the same entries: Drivers ticks too late for it.
        Palettes::applyRequest();
        MoonLivePalette::tickActive(platform::millis());
        // A rebuild a modifier asked for, and an effect's own structural work, run here, one layer at a time, before the layers render side by side.
        for (uint8_t i = 0; i < childCount(); i++)
            if (MoonModule* c = child(i); c && c->role() == ModuleRole::Layer) static_cast<Layer*>(c)->applyPending();
        composited_ = enabledLayerCount() > 1;
        pool_.run(childCount(), &tickChild, this);   // returns once every layer has rendered, so the composite reads finished buffers
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
    /// Render child `i` when it is a layer due this frame, timing it; `worker` says which lane its time is booked to.
    static void tickChild(void* ctx, uint8_t i, bool worker) MM_NONBLOCKING {
        auto& self = *static_cast<Effects*>(ctx);
        MoonModule* c = self.child(i);
        if (!c || c->role() != ModuleRole::Layer) return;
        if (c->respectsEnabled() && !c->enabled()) return;
        // An invisible layer of a composite is skipped; a lone layer is read as it is, so it always renders.
        if (self.composited_ && !static_cast<Layer*>(c)->contributes()) return;
        const uint32_t start = platform::micros();
        c->tick();
        const uint32_t us = platform::micros() - start;
        // A layer the render loop ran is Effects' children's time; one a worker ran is on another thread, booked to the worker's lane.
        self.addChildTimeUs(c, us, !worker);
        if (worker) static_cast<Layer*>(c)->addWorkerUs(us);
    }

    Layouts* layouts_ = nullptr;
    WorkerPool pool_;           ///< the render workers beside the render loop
    LayerLoad load_;            ///< the layer load of the last second
    bool composited_ = false;   ///< whether this frame blends several layers, set before they render
};

} // namespace mm
