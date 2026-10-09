/// @defgroup scheduler_impl Scheduler implementation
/// The module tree's owner: registration, naming, the tick order and control routing; the public surface lives in Scheduler.h.
/// @{
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"

#include "core/module/Control.h"    // applyControlValue + ApplyResult in setControl
#include "core/util/JsonUtil.h"   // mm::json::parseBool for the "enabled" pseudo-control
#include "platform/platform.h"

#include <cstdio>   // std::snprintf in freeName
#include <cstring>  // std::strcmp in firstInTree

namespace mm {

void Scheduler::addModule(MoonModule* mod) {
    if (!mod || moduleCount_ >= modules_.size()) return;
    modules_[moduleCount_++] = mod;
}

void Scheduler::setup() {
    instance_ = this;   // the one live Scheduler, reachable via Scheduler::instance()
    startTime_ = platform::millis();

    // Phase 1: bind each module's controls from scratch, so the persistence hook can apply file values to (name → variable pointer) descriptors.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->rebuildControls();
    }

    // Phase 2: persistence load. No-op if no hook is set.
    if (loadAllHook_) loadAllHook_(this);

    // Phase 2b: re-run defineControls with persisted values in place, so conditional hidden flags see the loaded state rather than the default.
    if (loadAllHook_) {
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->rebuildControls();
        }
    }

    // Phase 3: each module's own init, with the persisted values already in its members.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->setup();
    }

    // Phase 4: build derived state, buffers/peripherals, via the applyState() router, which per node builds when effectively-enabled and releases (release) when not, recursing the tree.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->applyState();
    }

    // Phase 5: a MoonLive script's declared controls exist only once phase 4 has compiled it, so the values the load held for them land now.
    applyDeferredControls(*this);

    lastLoop20ms_ = platform::millis();
    lastLoop1s_ = platform::millis();
    lastTimingUpdate_ = platform::millis();
}

void Scheduler::tick() MM_NONBLOCKING {
    uint32_t now = platform::millis();
    uint32_t tickStart = platform::micros();

    // A requested rebuild runs here at a frame boundary; the analyzer reports prepareTree transitively and that finding stays on purpose, the only signal if the gate is lost.
    if (prepareRequested_.exchange(false, std::memory_order_relaxed)) {
        prepareTree();
    }

    // Disabled modules do not tick, except system modules that override `respectsEnabled()` to return false so users can re-enable others through them.
    auto shouldRun = [](MoonModule* m) {
        return !m->respectsEnabled() || m->enabled();
    };
    for (uint8_t i = 0; i < moduleCount_; i++) {
        if (!shouldRun(modules_[i])) continue;
        uint32_t modStart = platform::micros();
        modules_[i]->tick();
        modules_[i]->addAccumUs(platform::micros() - modStart);
    }

    if (now - lastLoop20ms_ >= 20) {
        lastLoop20ms_ = now;
        for (uint8_t i = 0; i < moduleCount_; i++) {
            if (!shouldRun(modules_[i])) continue;
            uint32_t modStart = platform::micros();
            modules_[i]->tick20ms();
            modules_[i]->addAccumUs(platform::micros() - modStart);
        }
    }

    if (now - lastLoop1s_ >= 1000) {
        lastLoop1s_ = now;
        for (uint8_t i = 0; i < moduleCount_; i++) {
            if (!shouldRun(modules_[i])) continue;
            uint32_t modStart = platform::micros();
            modules_[i]->tick1s();
            modules_[i]->addAccumUs(platform::micros() - modStart);
        }
    }

    tickAccumUs_ += platform::micros() - tickStart;
    frameCount_++;

    // Every 1 second: compute averages, recurse into children
    if (now - lastTimingUpdate_ >= 1000) {
        tickTimeUs_ = frameCount_ > 0 ? tickAccumUs_ / frameCount_ : 0;

        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->publishTiming(frameCount_);
        }

        tickAccumUs_ = 0;
        frameCount_ = 0;
        lastTimingUpdate_ = now;
    }
}

void Scheduler::release() {
    // Two passes: release every module first so a release can still observe its siblings, then delete the trees.
    for (uint8_t i = moduleCount_; i > 0; i--) {
        modules_[i - 1]->release();
    }
    for (uint8_t i = moduleCount_; i > 0; i--) {
        deleteTree(modules_[i - 1]);
    }
    moduleCount_ = 0;
    instance_ = nullptr;
}

uint32_t Scheduler::elapsed() const {
    return platform::millis() - startTime_;
}

void Scheduler::prepareTree() {
    // This walk is the one a pending request asked for; a request made during it still stands.
    prepareRequested_.store(false, std::memory_order_relaxed);
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->applyState();
    }
    // Controls a state document named before their script compiled; this prepare compiled it.
    applyDeferredControls(*this);
    // Last, so a module reading across the tree sees every script's controls.
    for (uint8_t i = 0; i < moduleCount_; i++) modules_[i]->onTreePrepared();
}

namespace {
// One walk for every tree-wide notification, so each hook reaches the same set of modules.
template <class F>
void forEachInTree(MoonModule* m, F&& f) {
    f(*m);
    for (uint8_t i = 0; i < m->childCount(); i++)
        if (MoonModule* c = m->child(i)) forEachInTree(c, f);
}
}  // namespace

void Scheduler::notifyFileChanged(const char* path) {
    for (uint8_t i = 0; i < moduleCount_; i++)
        if (modules_[i]) forEachInTree(modules_[i], [path](MoonModule& m) { m.onFileChanged(path); });
}

void Scheduler::notifyListChanged(const MoonModule& owner) {
    for (uint8_t i = 0; i < moduleCount_; i++)
        if (modules_[i]) forEachInTree(modules_[i], [&owner](MoonModule& m) { m.onListChanged(owner); });
}

void Scheduler::deleteTree(MoonModule* mod) {
    if (!mod) return;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        deleteTree(mod->child(i));
    }
    delete mod;
}

MoonModule* Scheduler::topOfType(const char* typeName) const {
    for (uint8_t i = 0; typeName && i < moduleCount_; i++)
        if (modules_[i] && std::strcmp(modules_[i]->typeName(), typeName) == 0) return modules_[i];
    return nullptr;
}

// The separator is '-' because the name is a URL path segment; a suffix that would not fit is refused rather than truncated.
bool Scheduler::freeName(const char* base, char* out, size_t cap) {
    if (!base || !base[0] || std::strlen(base) >= MoonModule::kNameLen || cap < MoonModule::kNameLen) return false;
    char candidate[MoonModule::kNameLen];
    std::snprintf(candidate, sizeof(candidate), "%s", base);
    for (int suffix = 2; firstByName(candidate); suffix++) {
        const int n = std::snprintf(candidate, sizeof(candidate), "%s-%d", base, suffix);
        if (suffix >= 100 || n < 0 || n >= static_cast<int>(sizeof(candidate))) return false;
    }
    std::snprintf(out, cap, "%s", candidate);
    return true;
}


MoonModule* Scheduler::firstByName(const char* name) {
    if (!name || name[0] == 0) return nullptr;   // firstInTree strcmps name; a null would be UB
    for (uint8_t i = 0; i < moduleCount_; i++) {
        if (auto* m = firstInTree(modules_[i], name)) return m;
    }
    return nullptr;
}

Scheduler::SetControlResult Scheduler::setControl(const char* moduleName,
                                                 const char* controlName,
                                                 const char* valueJson) {
    MoonModule* target = firstByName(moduleName);
    if (!target) return SetControlResult::ModuleNotFound;

    // Module-level "enabled" pseudo-control, toggles the flag, then a full rebuild so the disabled subtree stops/starts ticking.
    if (std::strcmp(controlName, "enabled") == 0) {
        target->setEnabled(mm::json::parseBool(valueJson, "value"));
        target->markDirty();
        if (noteDirtyHook_) noteDirtyHook_();
        requestPrepareTree();
        // `enabled` rides the FULL state, not the per-leaf patch, so a full resync (the schema-change signal) is what tells the client.
        MoonModule::notifySchemaChanged();
        return SetControlResult::Ok;
    }

    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (std::strcmp(c.name, controlName) != 0) continue;

        // Per-type parse, validate and apply live in Control.cpp; a non-Ok result leaves the storage untouched.
        switch (applyControlValue(c, valueJson, "value")) {
            case ApplyResult::Ok:         break;
            case ApplyResult::OutOfRange: return SetControlResult::OutOfRange;
            case ApplyResult::Malformed:  return SetControlResult::Malformed;
            case ApplyResult::ReadOnly:   return SetControlResult::ReadOnly;
        }
        reactToControlChange(target, controlName);
        // A live control (ControlDescriptor::live) is driven continuously and is not configuration, so it never marks the tree dirty; `c` is the descriptor just applied.
        if (!c.live) {
            target->markDirty();
            if (noteDirtyHook_) noteDirtyHook_();
        }
        return SetControlResult::Ok;
    }
    return SetControlResult::ControlNotFound;
}

void Scheduler::reactToControlChange(MoonModule* target, const char* controlName) {
    // The list rebuilds first, so defineControls() re-evaluates conditional visibility for the new value.
    target->rebuildControls();
    target->onControlChanged(controlName);
    if (target->affectsPrepare(controlName)) requestPrepareTree();
}

namespace {

/// Is this control a Bool (or the `enabled` pseudo-control), the one type the byte reader scales to full range?
bool boolTyped(MoonModule* target, const char* controlName) {
    if (std::strcmp(controlName, "enabled") == 0) return true;
    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++)
        if (std::strcmp(ctrls[i].name, controlName) == 0)
            return ctrls[i].type == ControlType::Bool;
    return false;
}

}  // namespace

bool Scheduler::getControl(const char* moduleName, const char* controlName,
                           uint8_t& out) const {
    // Derived from the wide reader: a surface has 8 bits of travel, so a wider value clamps and a Bool reads back 255.
    int32_t wide = 0;
    if (!getControlWide(moduleName, controlName, wide)) return false;

    // A Bool is 0/1 at its own width; the surface wants it at full scale.
    MoonModule* target = const_cast<Scheduler*>(this)->firstByName(moduleName);
    if (target && boolTyped(target, controlName)) { out = wide ? 255 : 0; return true; }

    out = static_cast<uint8_t>(wide < 0 ? 0 : (wide > 255 ? 255 : wide));
    return true;
}

bool Scheduler::getControlWide(const char* moduleName, const char* controlName,
                               int32_t& out) const {
    if (!moduleName || !controlName) return false;
    MoonModule* target = const_cast<Scheduler*>(this)->firstByName(moduleName);
    if (!target) return false;

    // 0/1 here, not the byte reader's 0/255: this answers in the control's own units.
    if (std::strcmp(controlName, "enabled") == 0) {
        out = target->enabled() ? 1 : 0;
        return true;
    }

    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        const auto& c = ctrls[i];
        if (std::strcmp(c.name, controlName) != 0) continue;
        if (!c.ptr) return false;
        switch (c.type) {
            case ControlType::Bool:
                out = *static_cast<const bool*>(c.ptr) ? 1 : 0;
                return true;
            case ControlType::Uint8:
            case ControlType::Select:
            case ControlType::Palette:
                out = *static_cast<const uint8_t*>(c.ptr);
                return true;
            case ControlType::Uint16:
                out = *static_cast<const uint16_t*>(c.ptr);
                return true;
            case ControlType::Int16:
                out = *static_cast<const int16_t*>(c.ptr);
                return true;
            case ControlType::Int32:
                out = *static_cast<const int32_t*>(c.ptr);
                return true;
            // A Pin is int8_t storage (ControlList::addPin), so it cannot share the Int32 case.
            case ControlType::Pin:
                out = *static_cast<const int8_t*>(c.ptr);
                return true;
            // Text, a file path, a password and a button hold no number.
            default:
                return false;
        }
    }
    return false;
}


MoonModule* Scheduler::firstInTree(MoonModule* mod, const char* name) {
    if (!mod) return nullptr;
    if (mod->name() && std::strcmp(mod->name(), name) == 0) return mod;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        if (auto* m = firstInTree(mod->child(i), name)) return m;
    }
    return nullptr;
}

} // namespace mm

/// @}
