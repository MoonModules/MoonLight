#pragma once

#include "core/module/Control.h"
#include "core/util/ScratchBuffer.h"
#include "platform/platform.h"

#include <cstddef>
#include <cstring>

namespace mm {

/// A module's role, which identifies its type without RTTI and drives the UI's rendering.
enum class ModuleRole : uint8_t { Generic, Effect, Modifier, Driver, Layout, Layer, Service };

/// The lowercase role name, so the string cannot drift between one API route and another.
inline const char* roleName(ModuleRole role) {
    switch (role) {
        case ModuleRole::Effect:     return "effect";
        case ModuleRole::Modifier:   return "modifier";
        case ModuleRole::Driver:     return "driver";
        case ModuleRole::Layout:     return "layout";
        case ModuleRole::Layer:      return "layer";
        case ModuleRole::Service:    return "service";
        default:                     return "generic";
    }
}

/// The base class for everything in the system, from effects and drivers to system services.
///
/// It is the one deliberate hierarchy, so the UI renders any module with no per-module UI code.
/// The goal is the smallest possible base, since dozens load at once on a device without PSRAM.
///
/// Prior art: MoonLight's Node, a small base whose controls bind by reference.
///
/// @moreinfo
///
/// ## The lifecycle
///
/// `setup` and `release` bracket a module's life, and three tick rates run between them.
/// `defineControls` declares the controls, and `prepare` builds derived state.
/// That is what makes every config change apply live, with no reboot.
/// Controls bind by reference, so persisted values land before any setup runs.
///
/// ## Parent and child
///
/// Modules form a tree of parents and children, with no arbitrary graph.
/// The children array and its four mutators live once here, never overridden.
/// It starts empty, so a leaf allocates nothing, and grows on demand.
/// Children are told apart by role, which is also how a container filters them.
///
/// ## Enabled, and self-reporting
///
/// Every module carries an enabled flag, and each decides what disabled means.
/// A system module ignores it, so the user cannot lock themselves out.
/// Each module reports its instance size, its heap, and its tick time.
class MoonModule {
public:
    /// Allocate in PSRAM where the platform offers it.
    void* operator new(size_t size) { return platform::alloc(size); }
    /// Return the allocation above.
    void operator delete(void* ptr) noexcept { platform::free(ptr); }

    /// A module starts enabled, with no children and no controls.
    MoonModule() = default;
    /// Release the children array, the children themselves being the caller's.
    virtual ~MoonModule() { delete[] children_; }

    /// A module is identified by its place in the tree, so it is never copied.
    MoonModule(const MoonModule&) = delete;
    /// Nor copy-assigned.
    MoonModule& operator=(const MoonModule&) = delete;
    /// Nor moved, its children holding a pointer back to it.
    MoonModule(MoonModule&&) = delete;
    /// Nor move-assigned.
    MoonModule& operator=(MoonModule&&) = delete;

    /// One-time wiring, which by default sets up the children first.
    virtual void setup() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->setup(); }
    /// The hot tick, which by default ticks every enabled child and times each.
    virtual void tick() MM_NONBLOCKING { tickChildren(&MoonModule::tick); }
    /// The periodic tick for UI and network work, which by default ticks the children.
    virtual void tick20ms() MM_NONBLOCKING { tickChildren(&MoonModule::tick20ms); }
    /// The once-a-second tick for housekeeping, which by default ticks the children.
    virtual void tick1s() MM_NONBLOCKING { tickChildren(&MoonModule::tick1s); }
    /// Free everything this module holds, buffers and hardware alike, then the children.
    virtual void release() {
        // An override frees its own hardware; this base frees every buffer it registered.
        for (ScratchBufferBase* b = scratchBuffers_; b; b = b->next_) b->resizeBytes(0);
        // Then the children, in reverse, a module shutting down before those it owns.
        for (uint8_t i = childCount_; i > 0; i--) children_[i-1]->release();
    }

    /// Build or tear down each node by its own effective-enabled, which is the one lifecycle path.
    void applyState() {
        if (effectivelyEnabled()) {
            prepare();
            for (uint8_t i = 0; i < childCount_; i++) children_[i]->applyState();
        } else {
            release();   // recurses to children itself (reverse order)
        }
    }

    /// React once to the enabled flag flipping, for a one-shot that is not building state.
    virtual void onEnabled(bool /*newEnabled*/) {}

    /// React cheaply to one control changing, which runs on every change.
    virtual void onControlChanged(const char* /*controlName*/) {}

    /// Whether this control's change reshapes dimensions, and so triggers the build sweep.
    virtual bool affectsPrepare(const char* /*controlName*/) const { return false; }

    /// Declare every control, purely and idempotently, since this is re-run whenever a Select changes.
    virtual void defineControls() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->defineControls(); }

    /// A pad the silicon fixed, named by the signal it carries.
    struct FixedPin { uint8_t gpio; const char* role; };
    /// Report the pads this module drives that no control names, so the pin map sees them.
    virtual uint8_t fixedPins(FixedPin* /*out*/, uint8_t /*max*/) const { return 0; }

    /// Clear and rebuild this module's controls and its descendants', re-evaluating what is hidden.
    void rebuildControls() {
        // Every value patch passes here too, so hash and resync only on a real change.
        const uint32_t before = schemaSignature();
        clearControlsRecursive();
        defineControls();
        if (schemaChangedHook_ && schemaSignature() != before) schemaChangedHook_();
    }

    /// Hash the schema across this subtree, excluding values, so a resync fires only on a real change.
    uint32_t schemaSignature() const {
        uint32_t h = 2166136261u;
        mixSchema(h);
        return h;
    }
    /// Mix this node's schema into the running hash, then its children's.
    void mixSchema(uint32_t& h) const {
        auto mix = [&h](uint32_t v) { h = (h ^ v) * 16777619u; };
        auto mixStr = [&mix](const char* s) { for (const char* p = s; p && *p; p++) mix(static_cast<uint8_t>(*p)); mix(0u); };
        mix(controls_.count());
        for (uint8_t i = 0; i < controls_.count(); i++) {
            const ControlDescriptor& c = controls_[i];
            mixStr(c.name);
            mix(static_cast<uint32_t>(c.type));
            mix(static_cast<uint32_t>(c.min));
            mix(static_cast<uint32_t>(c.max));
            mix((c.hidden ? 1u : 0u) | (c.readonly ? 2u : 0u) | (static_cast<unsigned>(c.minMode) << 2));
            if (c.type == ControlType::Select && c.aux) {
                // Hash the strings, so an in-place rename still changes the signature.
                const char* const* opts = reinterpret_cast<const char* const*>(c.aux);
                for (int32_t o = 0; o < c.max; o++) mixStr(opts[o]);
            } else {
                mix(static_cast<uint32_t>(c.aux));   // Progress total / other aux — a value change differs
            }
        }
        for (uint8_t i = 0; i < childCount_; i++) children_[i]->mixSchema(h);
    }

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
    void clearControlsRecursive() {
        controls_.clear();
        for (uint8_t i = 0; i < childCount_; i++) children_[i]->clearControlsRecursive();
    }

    /// Build this node's derived state for the current controls, the acquire half of the lifecycle.
    virtual void prepare() {}

    /// Read the first output light as RGB, or false where this module has no output.
    virtual bool firstOutputRgb(uint8_t /*out*/[3]) const { return false; }

    /// This module's human label, which the user may rename.
    const char* name() const { return name_; }
    /// Set the label, truncating it to the buffer.
    void setName(const char* n) {
        if (!n) { name_[0] = 0; return; }
        size_t len = std::strlen(n);
        if (len >= sizeof(name_)) len = sizeof(name_) - 1;
        std::memcpy(name_, n, len);
        name_[len] = 0;
    }

    /// The stable factory key, which lives in flash rather than per instance.
    const char* typeName() const { return typeName_; }
    /// Set the factory key, which must have static lifetime.
    void setTypeName(const char* tn) { typeName_ = tn ? tn : ""; }

    /// This module's own enabled flag, which ignores its ancestors.
    bool enabled() const MM_NONBLOCKING { return enabled_; }
    /// Set the flag, firing the transition hook only on a real change.
    void setEnabled(bool e) {
        if (enabled_ == e) return;
        enabled_ = e;
        onEnabled(e);
    }

    /// Whether the enabled flag gates this module's ticks, which a system module declines.
    virtual bool respectsEnabled() const MM_NONBLOCKING { return true; }

    /// True unless this module or a gating ancestor is disabled, which the lifecycle keys off.
    bool effectivelyEnabled() const {
        for (const MoonModule* m = this; m; m = m->parent())
            if (m->respectsEnabled() && !m->enabled()) return false;
        return true;
    }

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
    void setParent(MoonModule* p) { parent_ = p; }

    /// Mark this module as wired by code, so a file that predates it cannot trim it away.
    void markWiredByCode() { wiredByCode_ = true; }
    /// Whether the boot wiring created this module, rather than a file or the user.
    bool isWiredByCode() const { return wiredByCode_; }

    /// This module's controls, which its own `defineControls` fills.
    ControlList& controls() { return controls_; }
    /// The controls, for a reader such as the serializer.
    const ControlList& controls() const { return controls_; }

    /// Read a boolean control by name, or the given default where it is absent.
    bool readBool(const char* name, bool dflt) const {
        for (uint8_t i = 0; i < controls_.count(); i++) {
            const ControlDescriptor& c = controls_[i];
            if (c.ptr && c.type == ControlType::Bool && std::strcmp(c.name, name) == 0)
                return *static_cast<const bool*>(c.ptr);
        }
        return dflt;
    }
    /// Read a byte-backed control by name, or the given default where it is absent.
    uint8_t readUint8(const char* name, uint8_t dflt) const {
        for (uint8_t i = 0; i < controls_.count(); i++) {
            const ControlDescriptor& c = controls_[i];   // a slider, a dropdown index, a palette index
            if (c.ptr && std::strcmp(c.name, name) == 0 &&
                (c.type == ControlType::Uint8 || c.type == ControlType::Select ||
                 c.type == ControlType::Palette))
                return *static_cast<const uint8_t*>(c.ptr);
        }
        return dflt;
    }

    /// Role for type identification (no RTTI needed).
    virtual ModuleRole role() const MM_NONBLOCKING { return ModuleRole::Generic; }

    /// Emoji tags for the module picker, beyond the chip the UI derives from the role.
    virtual const char* tags() const { return ""; }

    /// The roles this module accepts as children, which is what the add-child picker offers.
    virtual const char* acceptsChildRoles() const { return ""; }

    /// Whether a child of this role may go here, read from acceptsChildRoles(); the rule every path that adds a child enforces.
    bool acceptsRole(ModuleRole childRole) const {
        const char* csv = acceptsChildRoles();
        if (!csv || !csv[0]) return false;
        const char* want = roleName(childRole);
        const size_t wantLen = std::strlen(want);
        for (const char* p = csv; *p;) {
            const char* comma = std::strchr(p, ',');
            const size_t len = comma ? static_cast<size_t>(comma - p) : std::strlen(p);
            if (len == wantLen && std::strncmp(p, want, len) == 0) return true;
            if (!comma) break;
            p = comma + 1;
        }
        return false;
    }

    /// Whether some controls appear only once prepare has run, as a script's do once it compiles; a state document waits for that prepare to set them.
    virtual bool declaresControlsAtPrepare() const { return false; }

    /// A file changed through the API or a restore; a module that keeps something read from files updates what concerns it.
    virtual void onFileChanged(const char* /*path*/) {}

    /// Whether the user may delete or replace this module, which a load-bearing child declines.
    virtual bool userEditable() const { return true; }

    /// Whether a written config may re-apply live, which a module with one-shot setup declines.
    virtual bool appliesConfigLive() const { return true; }

    /// Park any worker of this module's that reads the tree, returning once it is idle.
    virtual void quiesce() {}

    /// Park both workers that read the tree, which every structural mutator calls first.
    void quiesceForMutation() {
        quiesce();
        if (quiesceRenderHook_) quiesceRenderHook_();
    }

    /// Append a child, growing the array on demand.
    bool addChild(MoonModule* child) {
        if (!child) return false;
        quiesceForMutation();   // the realloc below would pull the array from under a worker
        if (childCount_ == childCapacity_) {
            uint8_t newCap = childCapacity_ == 0 ? 4 : childCapacity_ * 2;
            auto** newArr = new MoonModule*[newCap];
            for (uint8_t i = 0; i < childCount_; i++) newArr[i] = children_[i];
            delete[] children_;
            children_ = newArr;
            childCapacity_ = newCap;
        }
        child->setParent(this);
        children_[childCount_++] = child;
        return true;
    }

    /// Remove a child, which the caller then releases and deletes.
    bool removeChild(MoonModule* child) {
        // Locate it first: a no-op removal must not needlessly park the render worker.
        uint8_t idx = 0;
        for (; idx < childCount_; idx++) if (children_[idx] == child) break;
        if (idx >= childCount_) return false;
        quiesceForMutation();   // the caller deletes the child next, so no worker may be in its tick
        child->setParent(nullptr);
        for (uint8_t j = idx; j + 1 < childCount_; j++) children_[j] = children_[j + 1];
        childCount_--;
        return true;
    }

    /// Swap in a fresh child at this position, returning the old one for the caller to delete.
    MoonModule* replaceChildAt(uint8_t i, MoonModule* fresh) {
        quiesceForMutation();   // the caller deletes the child swapped out, as with removeChild
        if (i >= childCount_ || !fresh) return nullptr;
        MoonModule* old = children_[i];
        if (old) old->setParent(nullptr);
        fresh->setParent(this);
        children_[i] = fresh;
        return old;
    }

    /// Move a child to an absolute position, the siblings between shifting toward the gap.
    bool moveChildTo(MoonModule* child, uint8_t newIndex) {
        if (newIndex >= childCount_) return false;
        // Reject a no-op before parking anything: a move that changes nothing needs no guard.
        uint8_t idx = 0;
        for (; idx < childCount_; idx++) if (children_[idx] == child) break;
        if (idx >= childCount_ || idx == newIndex) return false;
        // Permuting under an index-based worker loop can tick a child twice or skip one.
        quiesceForMutation();
        if (newIndex > idx) {
            // Shift left to fill the gap
            for (uint8_t j = idx; j < newIndex; j++) children_[j] = children_[j + 1];
        } else {
            // Shift right to make room
            for (uint8_t j = idx; j > newIndex; j--) children_[j] = children_[j - 1];
        }
        children_[newIndex] = child;
        return true;
    }

    /// How many children this module holds.
    uint8_t childCount() const { return childCount_; }
    /// One child by index, or null past the end.
    MoonModule* child(uint8_t i) const { return i < childCount_ ? children_[i] : nullptr; }

    /// This module's instance size, which registration sets once.
    size_t classSize() const { return classSize_ > 0 ? classSize_ : sizeof(MoonModule); }
    /// Record the instance size, which the factory does at registration.
    void setClassSize(size_t s) { classSize_ = s; }
    /// The heap this module has allocated, which its build sets.
    size_t dynamicBytes() const { return dynamicBytes_; }
    /// Record the heap total, for a module that allocates outside a scratch buffer.
    void setDynamicBytes(size_t b) { dynamicBytes_ = b; }

    // The buffer's own hooks, never a module's surface, so friendship keeps the contract structural.
    friend class ScratchBufferBase;
private:
    /// Adjust the heap total by a signed delta, which a buffer does on every resize.
    void addDynamicBytes(std::ptrdiff_t delta) {
        dynamicBytes_ = static_cast<size_t>(static_cast<std::ptrdiff_t>(dynamicBytes_) + delta);
    }

    /// Add a buffer to the intrusive list release walks, so a module with none pays one pointer.
    void registerScratchBuffer(ScratchBufferBase* b) {
        b->next_ = scratchBuffers_;   // push-front, O(1)
        scratchBuffers_ = b;
    }
    /// Unlink a buffer from that list.
    void deregisterScratchBuffer(ScratchBufferBase* b) {
        for (ScratchBufferBase** p = &scratchBuffers_; *p; p = &(*p)->next_) {
            if (*p == b) { *p = b->next_; return; }   // unlink
        }
    }
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
    /// Add one tick's time to the running total.
    void addAccumUs(uint32_t us) MM_NONBLOCKING { accumUs_ += us; }

    /// Average the accumulated time into the published figure, then recurse.
    void publishTiming(uint32_t frameCount) {
        tickTimeUs_ = frameCount > 0 ? accumUs_ / frameCount : 0;
        accumUs_ = 0;
        for (uint8_t i = 0; i < childCount_; i++) {
            children_[i]->publishTiming(frameCount);
        }
    }

protected:
    ControlList controls_;

    /// Which children a tick covers: every one, only a role, or every role but one.
    enum class RoleFilter : uint8_t { All, Only, Except };
    /// Tick the selected children through the one enabled gate and timing loop core owns.
    // The annotation rides the pointer type, so an unannotated method stays a compile error.
    void tickChildren(void (MoonModule::*fn)() MM_NONBLOCKING, RoleFilter filter = RoleFilter::All,
                      ModuleRole role = ModuleRole::Generic) MM_NONBLOCKING {
        for (uint8_t i = 0; i < childCount_; i++) {
            MoonModule* c = children_[i];
            if (filter == RoleFilter::Only   && c->role() != role) continue;
            if (filter == RoleFilter::Except && c->role() == role) continue;
            if (!c->respectsEnabled() || c->enabled()) {
                uint32_t start = platform::micros();
                (c->*fn)();
                c->addAccumUs(platform::micros() - start);
            }
        }
    }

private:
    // Sized to the longest stripped name with headroom; setName truncates past it.
    char name_[16] = {};
    const char* typeName_ = "";  ///< points into flash, never copied per instance
    bool enabled_ = true;
    bool dirty_ = false;
    bool wiredByCode_ = false;
    MoonModule* parent_ = nullptr;
    MoonModule** children_ = nullptr;
    uint8_t childCount_ = 0;
    uint8_t childCapacity_ = 0;
    size_t classSize_ = 0;
    size_t dynamicBytes_ = 0;
    ScratchBufferBase* scratchBuffers_ = nullptr;  ///< head of the intrusive free-on-disable list
    const char* status_ = nullptr;  ///< the message `status()` reads
    Severity severity_ = Severity::Status;
    uint32_t tickTimeUs_ = 0;
    uint32_t accumUs_ = 0;

    // One function pointer for the whole process, as with the persistence hook.
    static inline SchemaChangedFn schemaChangedHook_ = nullptr;
    static inline ValuesChangedFn valuesChangedHook_ = nullptr;
    static inline QuiesceRenderFn quiesceRenderHook_ = nullptr;
};

} // namespace mm
