#pragma once

#include "core/module/Control.h"
#include "core/module/NetworkUse.h"   // what a module reports of its network traffic, as it reports its memory
#include "core/util/HeapBytes.h"   // what a module reports of its memory, per type
#include "core/util/ScratchBuffer.h"
#include "core/util/fnv.h"
#include "platform/platform_clock.h"   // micros
#include "platform/platform_heap.h"   // alloc, free
#include "platform/platform_system.h"   // bootRecord

#include <atomic>
#include <cstddef>
#include <cstring>

namespace mm {

/// A module's role, which identifies its type without RTTI and drives the UI's rendering.
enum class ModuleRole : uint8_t { Generic, Effect, Modifier, Driver, Layout, Layer, Service, DeviceGroup };

/// Which axes a module works in, so the layer can extrude lower-dimensional output across the rest.
enum class Dim : uint8_t {
    D1 = 1,
    D2 = 2,
    D3 = 3,
};

/// The lowercase role name, so the string cannot drift between one API route and another.
inline const char* roleName(ModuleRole role) {
    switch (role) {
        case ModuleRole::Effect:     return "effect";
        case ModuleRole::Modifier:   return "modifier";
        case ModuleRole::Driver:     return "driver";
        case ModuleRole::Layout:     return "layout";
        case ModuleRole::Layer:      return "layer";
        case ModuleRole::Service:    return "service";
        case ModuleRole::DeviceGroup: return "devicegroup";
        default:                     return "generic";
    }
}

/// How far a reshaping control change's rebuild reaches: the module itself, the container holding it, or the whole tree.
enum class PrepareScope : uint8_t { Self, Parent, Tree };

/// The base class for everything in the system, from effects and drivers to system services.
///
/// It is the one deliberate hierarchy, so the UI renders any module with no per-module UI code: @xref{parent-and-child}. The base is the smallest possible, since dozens load at once without PSRAM: @xref{enabled-and-self-reporting}.
///
/// @technical
///
/// ## The lifecycle
///
/// `setup` and `release` bracket a module's life, with three tick rates between them. `defineControls` declares the controls and `prepare` builds derived state, so every change applies live.
///
/// Controls bind by reference, so persisted values land before any setup runs.
///
/// ## Parent and child
///
/// Modules form a tree, with no arbitrary graph. The children array and its four mutators live once here, never overridden, and a leaf allocates nothing. Roles tell children apart, which is how a container filters them.
///
/// ## Enabled, and self-reporting
///
/// Every module carries an enabled flag, and each decides what disabled means: a system module ignores it, so the user cannot lock themselves out. Each module reports its instance size, heap and tick time.
class MoonModule {
public:
    /// Allocate in PSRAM where the platform offers it; noexcept, so a failed allocation yields nullptr rather than a constructor running on null.
    void* operator new(size_t size) noexcept { return platform::alloc(size); }
    /// Return the allocation above.
    void operator delete(void* ptr) noexcept { platform::free(ptr); }

    /// A module starts enabled, with no children and no controls.
    MoonModule() = default;
    /// Release the children array, the children themselves being the caller's.
    virtual ~MoonModule();

    /// A module is identified by its place in the tree, so it is never copied.
    MoonModule(const MoonModule&) = delete;
    /// Nor copy-assigned.
    MoonModule& operator=(const MoonModule&) = delete;
    /// Nor moved, its children holding a pointer back to it.
    MoonModule(MoonModule&&) = delete;
    /// Nor move-assigned.
    MoonModule& operator=(MoonModule&&) = delete;

    /// One-time wiring, which by default sets up the children first: @xref{the-lifecycle}.
    virtual void setup() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->setup(); }
    /// The hot tick, which by default ticks every enabled child and times each.
    virtual void tick() MM_NONBLOCKING { tickChildren(&MoonModule::tick); }
    /// The periodic tick for UI and network work, which by default ticks the children.
    virtual void tick20ms() MM_NONBLOCKING { tickChildren(&MoonModule::tick20ms); }
    /// The once-a-second tick for housekeeping, which by default ticks the children.
    virtual void tick1s() MM_NONBLOCKING { tickChildren(&MoonModule::tick1s); }
    /// Free everything this module holds, buffers and hardware alike, then the children; a released module reports nothing, so its status clears.
    virtual void release();

    /// Build or tear down each node by its own effective-enabled, which is the one lifecycle path.
    void applyState();

    /// React once to the enabled flag flipping, for a one-shot that is not building state.
    virtual void onEnabled(bool /*newEnabled*/) {}

    /// React to a demand on this module turning on or off, at the frame boundary after it, where allocating is allowed: a provider counts its demands again, as Demand.h describes.
    virtual void onDemandChanged() {}
    /// The module this one runs outside its children, such as a reel's current effect, so a walk over the tree reaches it; null for most.
    virtual const MoonModule* hosted() const { return nullptr; }

    /// React cheaply to one control changing, which runs on every change.
    virtual void onControlChanged(const char* /*controlName*/) {}

    /// Whether this control's change reshapes dimensions, and so triggers the build sweep.
    virtual bool affectsPrepare(const char* /*controlName*/) const { return false; }
    /// How far that build sweep reaches: the whole tree unless this module knows its change stays closer, a reader of what changed re-preparing itself in onTreePrepared.
    virtual PrepareScope prepareScope() const { return PrepareScope::Tree; }

    /// Declare every control, purely and idempotently, since this is re-run whenever a Select changes.
    virtual void defineControls() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->defineControls(); }

    /// A pad the silicon fixed, named by the signal it carries.
    struct FixedPin { uint8_t gpio; const char* role; };
    /// Report the pads this module drives that no control names, so the pin map sees them.
    virtual uint8_t fixedPins(FixedPin* /*out*/, uint8_t /*max*/) const { return 0; }

    /// A device on the board's I2C bus, at its 7-bit address, named by what it is.
    struct I2cDevice { uint8_t addr; const char* role; };
    /// Report the devices this module drives on the I2C bus, so a bus scan names what answered.
    virtual uint8_t i2cDevices(I2cDevice* /*out*/, uint8_t /*max*/) const { return 0; }

    /// Clear and rebuild this module's controls and its descendants', re-evaluating what is hidden.
    void rebuildControls();

    /// Size every control array in the subtree to its controls, once a rebuild has declared them.
    void shrinkControlsRecursive();

    /// Hash the schema across this subtree, excluding values, so a resync fires only on a real change.
    uint32_t schemaSignature() const;
    /// Mix this node's schema into the running hash, then its children's.
    void mixSchema(Fnv1a& f) const;

    /// The schema-changed hook's type, a function pointer so core needs no web-layer include.
    using SchemaChangedFn = void (*)();
    /// Install the hook that resyncs clients after a schema change.
    static void setSchemaChangedHook(SchemaChangedFn fn) { schemaChangedHook_ = fn; }

    /// The quiesce-render hook's type, the same decoupling from the light domain.
    using QuiesceRenderFn = void (*)();
    /// Install the hook that stops the render worker before a structural mutation.
    static void setQuiesceRenderHook(QuiesceRenderFn fn) { quiesceRenderHook_ = fn; }
    /// Fire the resync directly, for a change the control rebuild does not cover.
    static void notifySchemaChanged() { if (schemaChangedHook_) schemaChangedHook_(); }

    /// The values-changed hook's type, the same decoupling as the schema hook.
    using ValuesChangedFn = void (*)(MoonModule*);
    /// Install the hook that pushes a module's changed values ahead of the periodic patch.
    static void setValuesChangedHook(ValuesChangedFn fn) { valuesChangedHook_ = fn; }
    /// Send this module's changed values on the next transport tick rather than the next second, for a value someone watches move.
    void notifyValuesChanged() { if (valuesChangedHook_) valuesChangedHook_(this); }

    /// Stop the render worker directly, for a mutation that frees memory outside the child array.
    static void notifyQuiesceRender() { if (quiesceRenderHook_) quiesceRenderHook_(); }
    /// Clear this module's controls and every descendant's.
    void clearControlsRecursive();

    /// Build this node's derived state for the current controls, the acquire half of the lifecycle.
    virtual void prepare() {}

    /// A name's buffer, terminator included: the longest stripped type name with headroom; setName truncates past it.
    static constexpr uint8_t kNameLen = 16;
    /// This module's human label, which the user may rename.
    const char* name() const { return name_; }
    /// Set the label, truncating it to the buffer.
    void setName(const char* n);

    /// The stable factory key, which lives in flash rather than per instance.
    const char* typeName() const { return typeName_; }
    /// Set the factory key, which must have static lifetime.
    void setTypeName(const char* tn) { typeName_ = tn ? tn : ""; }

    /// This module's own enabled flag, which ignores its ancestors, and is off while safe mode holds it.
    bool enabled() const MM_NONBLOCKING { return enabled_ && !held_; }
    /// The enabled flag as set, which persistence saves whatever safe mode holds.
    bool enabledSetting() const { return enabled_; }
    /// Whether safe mode holds this module, off whatever its flag says.
    bool held() const { return held_; }

    /// How many failed boots in a row start safe mode.
    static constexpr uint8_t kSafeModeRestarts = 2;
    /// The failed boots before this one, in a row: crashes, brownouts, and boots that lost their power within a minute.
    static uint8_t failedBoots() { return platform::bootRecord().abnormalRestarts; }
    /// Whether this boot runs in safe mode, after failed boots in a row, so a setting or a supply at fault can be fixed from the UI.
    static bool safeMode() { return failedBoots() >= kSafeModeRestarts; }
    /// Whether safe mode holds this module, counting it as disabled while its saved flag stays, for one that can crash the device or overload its supply.
    virtual bool heldInSafeMode() const { return false; }
    /// Set the flag, firing the transition hook only on a real change, with the workers parked first, as every structural mutator does.
    void setEnabled(bool e);
    /// The effect enabled or added last, the one someone just switched to, or null once it leaves the tree.
    static MoonModule* lastEnabledEffect() { return lastEnabledEffect_; }
    /// React once a rebuild walk has finished, for a top-level module that reads across the tree.
    virtual void onTreePrepared() {}

    /// Whether the enabled flag gates this module's ticks, which a system module declines.
    virtual bool respectsEnabled() const MM_NONBLOCKING { return true; }

    /// True unless this module or a gating ancestor is disabled, which the lifecycle keys off.
    bool effectivelyEnabled() const;

    /// Whether this module shows in the UI, which a pure engine with no controls declines.
    virtual bool appearsInUi() const { return true; }

    /// Whether this module's state has been touched since the last save.
    bool dirty() const { return dirty_; }
    /// Mark the state touched, which the persistence layer observes.
    void markDirty() { dirty_ = true; }
    /// Clear the mark, once the state has been written.
    void clearDirty() { dirty_ = false; }

    /// This module's parent, or null at the top level.
    MoonModule* parent() const { return parent_; }
    /// Set the parent, which the child mutators do.
    void setParent(MoonModule* p);

    /// Mark this module as wired by code, so a file that predates it cannot trim it away.
    void markWiredByCode() { wiredByCode_ = true; }
    /// Whether the boot wiring created this module, rather than a file or the user.
    bool isWiredByCode() const { return wiredByCode_; }

    /// This module's controls, which its own `defineControls` fills.
    ControlList& controls() { return controls_; }
    /// The controls, for a reader such as the serializer.
    const ControlList& controls() const { return controls_; }

    /// Read a boolean control by name, or the given default where it is absent.
    bool readBool(const char* name, bool dflt) const;
    /// Read a byte-backed control by name, or the given default where it is absent.
    uint8_t readUint8(const char* name, uint8_t dflt) const;

    /// Role for type identification (no RTTI needed).
    virtual ModuleRole role() const MM_NONBLOCKING { return ModuleRole::Generic; }

    /// Emoji tags for the module picker, beyond the chip the UI derives from the role.
    virtual const char* tags() const { return ""; }

    /// The roles this module accepts as children, which is what the add-child picker offers.
    virtual const char* acceptsChildRoles() const { return ""; }

    /// Whether a child of this role may go here, read from acceptsChildRoles(); the rule every path that adds a child enforces.
    bool acceptsRole(ModuleRole childRole) const;

    /// Whether some controls appear only once prepare has run, as a script's do once it compiles; a state document waits for that prepare to set them.
    virtual bool declaresControlsAtPrepare() const { return false; }

    /// A file changed through the API or a restore; a module that keeps something read from files updates what concerns it.
    virtual void onFileChanged(const char* /*path*/) {}

    /// A list control's rows changed in `owner`; a module that resolves something from that list re-resolves it.
    virtual void onListChanged(const MoonModule& /*owner*/) {}

    /// A module was renamed from `from` to `to`; a module that keeps another module's name, such as a control binding, rewrites it.
    virtual void onModuleRenamed(const char* /*from*/, const char* /*to*/) {}

    /// Whether the user may delete or replace this module, which a load-bearing child declines.
    virtual bool userEditable() const { return true; }

    /// Whether a written config may re-apply live, which a module with one-shot setup declines.
    virtual bool appliesConfigLive() const { return true; }

    /// Park any worker of this module's that reads the tree, returning once it is idle.
    virtual void quiesce() {}

    /// Park both workers that read the tree, which every structural mutator calls first, and move the tree's modification count (`Scheduler::structureVersion`).
    void quiesceForMutation();

    /// Append a child, growing the array on demand.
    bool addChild(MoonModule* child);

    /// Remove a child, which the caller then releases and deletes.
    bool removeChild(MoonModule* child);

    /// Swap in a fresh child at this position, returning the old one for the caller to delete.
    MoonModule* replaceChildAt(uint8_t i, MoonModule* fresh);

    /// Move a child to an absolute position, the siblings between shifting toward the gap.
    bool moveChildTo(MoonModule* child, uint8_t newIndex);

    /// How many children this module holds.
    uint8_t childCount() const { return childCount_; }
    /// One child by index, or null past the end.
    MoonModule* child(uint8_t i) const { return i < childCount_ ? children_[i] : nullptr; }

    /// Report the services this module offers on the network and the traffic it exchanges, from counters it already keeps; a module with no network use reports nothing.
    virtual void reportNetworkUse(NetworkUse& /*use*/) const {}

    /// This module's instance size, which registration sets once.
    size_t classSize() const { return classSize_ > 0 ? classSize_ : sizeof(MoonModule); }
    /// Record the instance size, which the factory does at registration.
    void setClassSize(size_t s) { classSize_ = s; }
    /// The heap this module has allocated, which its build sets.
    size_t dynamicBytes() const { return dynamicBytes_; }
    /// The same heap with the part of it in PSRAM, for the memory sheet's columns per type.
    HeapBytes heapBytes() const { return {dynamicBytes_, psramBytes_}; }
    /// Record the heap total, for a module that allocates outside a scratch buffer: `heapOf` per allocation says which part is PSRAM.
    void setDynamicBytes(const HeapBytes& h) { dynamicBytes_ = h.total; psramBytes_ = h.psram; }

    /// The first of the resources this module holds, for a service that counts what they demand.
    const ModuleResource* resources() const { return resources_; }

    // The resources' and the buffers' own hooks, never a module's surface, so friendship keeps the contract structural.
    friend class ModuleResource;
    friend class ScratchBufferBase;
private:
    /// Move the heap total from what a buffer held to what it holds now, which a buffer does on every resize.
    void moveDynamicBytes(const HeapBytes& from, const HeapBytes& to) {
        setDynamicBytes(heapBytes() - from + to);
    }

    /// Add a resource to the intrusive list release walks, so a module with none pays one pointer.
    void registerResource(ModuleResource* r) {
        r->next_ = resources_;   // push-front, O(1)
        resources_ = r;
    }
    /// Unlink a resource from that list.
    void deregisterResource(ModuleResource* r);
public:

    /// How much a status message matters, which picks the icon the UI shows.
    enum class Severity : uint8_t {
        Status,   ///< ℹ️ neutral info, current state ("connected")
        Warning,  ///< ⚠️ silent degradation ("buffer reduced")
        Error,    ///< ❌ something failed ("WiFi auth failed")
    };
    /// The short message this module wants the user to see, or null for none.
    const char* status() const { return status_; }
    /// How much that message matters.
    Severity severity() const { return severity_; }
    /// Set the message, whose storage the caller owns since the slot does not copy.
    void setStatus(const char* msg, Severity sev = Severity::Status) {
        status_ = msg;
        severity_ = sev;
    }
    /// Clear the message.
    void clearStatus() { status_ = nullptr; severity_ = Severity::Status; }

    /// Average microseconds per tick over the last second, which parents measure for children.
    uint32_t tickTimeUs() const { return tickTimeUs_; }
    /// The longest single tick of the last second, which an average hides: one slow frame a second is a stutter at any average.
    uint32_t worstTimeUs() const { return worstUs_; }
    /// Add one tick's time to the running total, from whichever core ticked it: the drivers tick on the encode task and on the main loop.
    void addAccumUs(uint32_t us) MM_NONBLOCKING {
        accumUs_.fetch_add(us, std::memory_order_relaxed);
        uint32_t peak = peakUs_.load(std::memory_order_relaxed);
        while (us > peak && !peakUs_.compare_exchange_weak(peak, us, std::memory_order_relaxed)) {}
    }

    /// The module's own time per frame: its total less the time its own thread spent inside its children's ticks, which a child on another thread leaves in.
    uint32_t ownTimeUs() const { return ownTimeUs_; }

    /// Average the accumulated time into the published figures, then recurse; a module that counts time of its own, such as a wait, averages it here too.
    virtual void publishTiming(uint32_t frameCount) MM_NONBLOCKING {
        const uint32_t accum = accumUs_.exchange(0, std::memory_order_relaxed);
        const uint32_t kids = childAccumUs_.exchange(0, std::memory_order_relaxed);
        tickTimeUs_ = frameCount > 0 ? accum / frameCount : 0;
        ownTimeUs_ = frameCount > 0 && accum > kids ? (accum - kids) / frameCount : 0;
        worstUs_ = peakUs_.exchange(0, std::memory_order_relaxed);
        for (uint8_t i = 0; i < childCount_; i++) {
            children_[i]->publishTiming(frameCount);
        }
    }

protected:
    ControlList controls_;

    /// Charge one child's tick to the child, the one place a child's time is booked. A child ticked on this module's own thread also counts as children's time, which this module's own time excludes; `onThisThread` is false for a child on another thread.
    void addChildTimeUs(MoonModule* c, uint32_t us, bool onThisThread = true) MM_NONBLOCKING {
        c->addAccumUs(us);
        if (onThisThread) childAccumUs_.fetch_add(us, std::memory_order_relaxed);
    }

    /// Which children a tick covers: every one, only a role, or every role but one.
    enum class RoleFilter : uint8_t { All, Only, Except };
    /// Tick the selected children through the one enabled gate and timing loop core owns.
    void tickChildren(void (MoonModule::*fn)() MM_NONBLOCKING, RoleFilter filter = RoleFilter::All,
                      ModuleRole role = ModuleRole::Generic, bool onThisThread = true) MM_NONBLOCKING {
        // The annotation rides the pointer type, so an unannotated method stays a compile error.
        for (uint8_t i = 0; i < childCount_; i++) {
            MoonModule* c = children_[i];
            if (filter == RoleFilter::Only   && c->role() != role) continue;
            if (filter == RoleFilter::Except && c->role() == role) continue;
            if (!c->respectsEnabled() || c->enabled()) {
                uint32_t start = platform::micros();
                (c->*fn)();
                addChildTimeUs(c, platform::micros() - start, onThisThread);
            }
        }
    }

private:
    char name_[kNameLen] = {};
    const char* typeName_ = "";  ///< points into flash, never copied per instance
    bool enabled_ = true;
    bool held_ = false;   ///< safe mode holds it
    static inline MoonModule* lastEnabledEffect_ = nullptr;   ///< see lastEnabledEffect()
    bool dirty_ = false;
    bool wiredByCode_ = false;
    Severity severity_ = Severity::Status;   ///< beside the flags, so the byte shares their word
    MoonModule* parent_ = nullptr;
    MoonModule** children_ = nullptr;
    uint8_t childCount_ = 0;
    uint8_t childCapacity_ = 0;
    uint32_t psramBytes_ = 0;   ///< the part of dynamicBytes_ in PSRAM: in the padding before classSize_ on a 64-bit host, 4 bytes more on a 32-bit board
    size_t classSize_ = 0;
    size_t dynamicBytes_ = 0;
    ModuleResource* resources_ = nullptr;  ///< head of the intrusive let-go-on-release list
    const char* status_ = nullptr;  ///< the message `status()` reads
    uint32_t tickTimeUs_ = 0;
    uint32_t ownTimeUs_ = 0;
    uint32_t worstUs_ = 0;
    std::atomic<uint32_t> peakUs_{0};   ///< the longest tick in the current second, relaxed like accumUs_
    std::atomic<uint32_t> childAccumUs_{0};   ///< the children's time on this module's own thread
    std::atomic<uint32_t> accumUs_{0};   ///< a statistic, so relaxed: no other memory is ordered by it

    /// The process-wide hooks the transport installs, one function pointer each, as with the persistence hook.
    static inline SchemaChangedFn schemaChangedHook_ = nullptr;
    static inline ValuesChangedFn valuesChangedHook_ = nullptr;
    static inline QuiesceRenderFn quiesceRenderHook_ = nullptr;
};

} // namespace mm
