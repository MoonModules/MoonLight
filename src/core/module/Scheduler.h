#pragma once

#include "core/module/MoonModule.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace mm {

/// The domain-neutral orchestrator: it owns the top-level modules, boots them, and drives every tick.
///
/// It also carries the tree-walk utilities, so deleting a subtree and uniquifying a name live once.
/// The bodies are in Scheduler.cpp, and the boot phases are documented there.
///
/// @moreinfo
///
/// ## Three cadences cover every module
///
/// `tick` is the hot path, paced here by yielding between iterations, and two slower ticks follow it.
/// Every top-level module ticks inline in one loop, and each drives its own children.
/// Effects animate off a synchronized clock, so their speed holds at any frame rate.
///
/// ## Rebuilding derived state
///
/// A rebuild walks the tree, rebuilding buffers, mappings and any compiled script.
/// `prepareTree` does it immediately and `requestPrepareTree` at the next frame boundary.
/// Prefer the request: the immediate walk runs compiled code on the calling task's stack.
/// A values-only reapply can follow it, for controls a script declares as it compiles.
///
/// ## Driving one control from anywhere
///
/// `setControl` names a module and a control, and applies the whole control-change reaction.
/// It parses, rebuilds the control list, fires the hook, marks dirty, and rebuilds the tree.
/// `getControlWide` reads at the control's own width, and `getControl` converts that to a byte.
/// Clamping suits a surface and breaks arithmetic, so an input mapping reads the wide one.
class Scheduler {
public:
    /// The persistence hook's type, kept as a function pointer so Scheduler needs no include.
    using LoadAllFn = void(*)(Scheduler*);
    /// Install the hook that overlays persisted values onto bound variables before any setup runs.
    void setLoadAllHook(LoadAllFn fn) { loadAllHook_ = fn; }

    /// Install the hook that reapplies values once, after the first rebuild.
    void setReapplyValuesHook(LoadAllFn fn) { reapplyValuesHook_ = fn; }

    /// The dirty hook's type, the same decoupling as the load hook above.
    using NoteDirtyFn = void(*)();
    /// Install the hook that schedules a debounced save after a control mutation.
    void setNoteDirtyHook(NoteDirtyFn fn) { noteDirtyHook_ = fn; }

    /// Register a top-level module, which the boot then walks in declared order.
    void addModule(MoonModule* mod);
    /// Run the boot phases over every registered module.
    void setup();
    /// One pass: tick every top-level module, then pace and publish the timing.
    void tick() MM_NONBLOCKING;
    /// Release every module, in reverse of the order they were added.
    void release();

    /// Milliseconds since setup, which is the clock every effect animates against.
    uint32_t elapsed() const;

    /// Rebuild derived state across the whole tree, immediately, on the calling thread.
    void prepareTree();

    /// Ask for a rebuild at the next frame boundary, which is safe from any task.
    void requestPrepareTree() { prepareRequested_.store(true, std::memory_order_relaxed); }

    /// Ask for a values-only reapply right after the next requested rebuild.
    void requestValuesReapply() { valuesReapplyRequested_.store(true, std::memory_order_relaxed); }

    /// Average microseconds per tick over the last second, which is the primary performance metric.
    uint32_t tickTimeUs() const { return tickTimeUs_; }
    /// Frames a second, derived from the tick time above.
    uint32_t fps() const { return tickTimeUs_ > 0 ? 1000000 / tickTimeUs_ : 0; }
    /// How many top-level modules are registered.
    uint8_t moduleCount() const { return moduleCount_; }
    /// One top-level module by index, or null past the end.
    MoonModule* module(uint8_t i) const { return i < moduleCount_ ? modules_[i] : nullptr; }

    /// Release and delete a whole subtree, children first.
    static void deleteTree(MoonModule* mod);

    /// Make this module's name unique across the tree, the caller having placed it there already.
    void ensureUniqueName(MoonModule* mod);

    /// Disambiguate every duplicated name in the tree, the first occurrence keeping its own.
    void deduplicateNamesInTree();

    /// The first module in tree-walk order with this name, or null.
    MoonModule* firstByName(const char* name);

    /// The single live Scheduler, reachable so a factory-created module can drive a control.
    static Scheduler* instance() { return instance_; }

    /// What `setControl` did, which each transport maps onto its own status codes.
    enum class SetControlResult : uint8_t {
        Ok,
        ModuleNotFound,   ///< no module with that name in the tree
        ControlNotFound,  ///< module exists but has no such control
        OutOfRange,       ///< numeric value outside the control's bounds
        Malformed,        ///< value didn't parse
        ReadOnly,         ///< tried to write a display-only control
    };

    /// Set one control by module and control name, applying the whole control-change reaction.
    SetControlResult setControl(const char* moduleName, const char* controlName,
                                const char* valueJson);

    /// The reaction to a control that changed: visibility rebuilt, the module told, and the tree re-prepared when the control reshapes it.
    void reactToControlChange(MoonModule* target, const char* controlName);

    /// Read one control as a byte, in the units a surface speaks, or false when there is none.
    bool getControl(const char* moduleName, const char* controlName, uint8_t& out) const;

    /// Read one control at its own width, signed, or false when there is no numeric reading.
    bool getControlWide(const char* moduleName, const char* controlName, int32_t& out) const;

private:
    /// Recurse one subtree, uniquifying each name as it goes.
    void walkAndEnsureUnique(MoonModule* mod);
    /// The first node in this subtree with the given name, or null.
    static MoonModule* firstInTree(MoonModule* mod, const char* name);

    static inline Scheduler* instance_ = nullptr;
    std::array<MoonModule*, 32> modules_{};
    uint8_t moduleCount_ = 0;
    // Atomic because a lost request means a script edit silently never applies.
    std::atomic<bool> prepareRequested_{false};
    LoadAllFn loadAllHook_ = nullptr;
    LoadAllFn reapplyValuesHook_ = nullptr;
    bool valuesReapplied_ = false;   ///< the hook fires once, after the first rebuild
    std::atomic<bool> valuesReapplyRequested_{false};   ///< consumed with the next requested rebuild
    NoteDirtyFn noteDirtyHook_ = nullptr;
    uint32_t startTime_ = 0;
    uint32_t lastLoop20ms_ = 0;
    uint32_t lastLoop1s_ = 0;
    uint32_t tickTimeUs_ = 0;
    uint32_t tickAccumUs_ = 0;
    uint32_t frameCount_ = 0;        ///< frames in the current window, which the average divides by
    uint32_t lastTimingUpdate_ = 0;  ///< when that window opened
};

// The Drivers master-state reads, whose absent-control defaults live here once per consumer.
/// Whether the lights are on, defaulting to on where the control is absent.
inline bool driversOn(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readBool("on", true) : true;   // absent → on
}
/// The master brightness, defaulting to zero where the control is absent.
inline uint8_t driversBrightness(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readUint8("brightness", 0) : 0;
}
/// The selected palette index, defaulting to zero where the control is absent.
inline uint8_t driversPalette(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readUint8("palette", 0) : 0;
}

} // namespace mm
