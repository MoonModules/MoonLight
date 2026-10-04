/// @defgroup scheduler_impl Scheduler implementation
/// The module tree's owner: registration, naming, the tick order and control routing.
/// Public surface and class layout live in Scheduler.h.
/// @{
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"

#include "core/module/Control.h"    // applyControlValue + ApplyResult in setControl
#include "core/util/JsonUtil.h"   // mm::json::parseBool for the "enabled" pseudo-control
#include "platform/platform.h"

#include <cstdio>   // std::snprintf in ensureUniqueName
#include <cstring>  // std::strcmp in firstInTree

namespace mm {

void Scheduler::addModule(MoonModule* mod) {
    if (!mod || moduleCount_ >= modules_.size()) return;
    modules_[moduleCount_++] = mod;
}

void Scheduler::setup() {
    instance_ = this;   // the one live Scheduler, reachable via Scheduler::instance()
    startTime_ = platform::millis();

    // Phase 1: bind each module's controls. After this, ControlList descriptors hold (name → variable pointer) so the persistence hook can apply file values.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->defineControls();
    }

    // Phase 2: persistence load. No-op if no hook is set.
    if (loadAllHook_) loadAllHook_(this);

    // Phase 2a: disambiguate any same-name modules introduced by persistence (positional load gives each freshly-created module the factory's display name; two Layer instances both get "Layer").
    // The /api/state UI sends names back as parent_id, so duplicates break "add child to the second one".
    // Walks the tree once; first occurrence keeps the name, later ones get " 2", " 3", … suffixes.
    deduplicateNamesInTree();

    // Phase 2b: re-run defineControls with persisted values in place so any conditional hidden flags (e.g. NetworkModule's static-IP fields depending on addressing_) are evaluated against the loaded state, not the default. rebuildControls clears the descriptor list before re-binding, so this is idempotent.
    if (loadAllHook_) {
        for (uint8_t i = 0; i < moduleCount_; i++) {
            modules_[i]->rebuildControls();
        }
    }

    // Phase 3: each module's own init.
    // Persisted values are already in member variables, so e.g.
    // NetworkModule sees the persisted ssid_, SystemModule sees an overlaid deviceName_ (or guards if empty to derive the MAC-based default).
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->setup();
    }

    // Phase 4: build derived state, buffers/peripherals, via the applyState() router, which per node builds when effectively-enabled and releases (release) when not, recursing the tree.
    for (uint8_t i = 0; i < moduleCount_; i++) {
        modules_[i]->applyState();
    }

    // Phase 5: re-apply saved VALUES, now that every module has prepared.
    // A schema that depends on prepare()'s own WORK does not exist during phase 2's load: a MoonLive script's declared controls appear only once the script has compiled, which prepare() just did, so their saved values had no control to land on and prepare() seeded them from the script's defaults.
    // Values only, and once: after boot the live values are the truth, and re-reading the file would undo the edit that triggered any later prepare.
    if (!valuesReapplied_) {
        valuesReapplied_ = true;
        if (reapplyValuesHook_) reapplyValuesHook_(this);
    }

    lastLoop20ms_ = platform::millis();
    lastLoop1s_ = platform::millis();
    lastTimingUpdate_ = platform::millis();
}

void Scheduler::tick() MM_NONBLOCKING {
    uint32_t now = platform::millis();
    uint32_t tickStart = platform::micros();

    // A rebuild asked for from another task happens HERE, at a frame boundary on the render thread.
    // The walk runs a scripted layout's compiled code, and that code's frame is ordinary stack on whichever task calls it, so doing it inline in an HTTP handler put a script on the web server's small stack instead of the render task's, which is the budget every other effect is measured against.
    // Deferring also means the pipeline is never rebuilt underneath a half-rendered frame. exchange, not test-then-clear: a request arriving between the two would be dropped.
    //
    // This gate is what keeps tick() honest about its MM_NONBLOCKING annotation. prepareTree allocates, reads the filesystem and runs the JIT, none of which belongs in a frame.
    // It runs only when another task asked for a rebuild, which is a user action (a control edit, a module added or removed, a script renamed), never a frame.
    // A steady-state tick pays one relaxed exchange.
    // The static analyzer cannot see that, so it reports the path transitively, and the finding stays in the report on purpose.
    // If prepareTree ever becomes reachable WITHOUT this gate, that report is the only thing that will say so.
    if (prepareRequested_.exchange(false, std::memory_order_relaxed)) {
        prepareTree();
        // The runtime twin of boot's phase 5 (see requestValuesReapply): same tick as the prepare, so the just-restored file cannot be rewritten by a dirty save in between.
        if (valuesReapplyRequested_.exchange(false, std::memory_order_relaxed) && reapplyValuesHook_)
            reapplyValuesHook_(this);
    }

    // Scheduler gates loop callbacks by `enabled()`, disabled modules don't tick.
    // System modules that need to keep running regardless (HttpServer, Network, Filesystem, so users can re-enable other modules through them) override `respectsEnabled()` to return false.
    // `onEnabled()` fires once per transition for custom start/stop semantics; see MoonModule::setEnabled().
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
    // Two passes: tear down all modules first (so a module's release can still safely observe sibling modules' state), then delete the trees. Otherwise the reverse-order release-then-delete pattern would leave a module's release looking at already-freed siblings, relevant for any cross-module cleanup work.
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
}

namespace {
void notifyTree(MoonModule* m, const char* path) {
    m->onFileChanged(path);
    for (uint8_t i = 0; i < m->childCount(); i++)
        if (MoonModule* c = m->child(i)) notifyTree(c, path);
}
}  // namespace

void Scheduler::notifyFileChanged(const char* path) {
    for (uint8_t i = 0; i < moduleCount_; i++)
        if (modules_[i]) notifyTree(modules_[i], path);
}

void Scheduler::deleteTree(MoonModule* mod) {
    if (!mod) return;
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        deleteTree(mod->child(i));
    }
    delete mod;
}

// Why this exists: ModuleFactory::create gives every freshly-created module a display name derived from its type ("NoiseEffect" → "Noise", "Layer" stays "Layer").
// When the user adds two Layers, both factory-default to "Layer"; the HTTP API uses names as parent_id, and findModuleByName does a first-match DFS, so the second Layer becomes unreachable.
// Same problem happens when persistence rebuilds the tree positionally on boot.
//
// Called from HttpServerModule after addChild (single-module add) and from deduplicateNamesInTree after persistence load (whole-tree pass).
void Scheduler::ensureUniqueName(MoonModule* mod) {
    if (!mod) return;
    const char* base = mod->name();
    if (!base || base[0] == 0) return;
    if (firstByName(base) == mod) return;  // the first occurrence keeps the name

    // `candidate` is sized to match MoonModule::name_[16], there's no point computing a longer name than setName can store.
    // The snprintf check below refuses to truncate, which means the practical cap depends on the base length.
    // 99 for ≤ 5-char bases, 9 for 12–13-char bases like "GlowParticles" or "PlasmaPalette" (where "GlowParticles-10" = 16 chars + NUL doesn't fit).
    // When the cap is hit we keep the duplicate name rather than truncate; first-match DFS lookups become ambiguous for that name but the engine doesn't crash.
    // This is unlikely in practice (10+ same-typed siblings on one tree), bump name_/candidate together if it ever bites.
    //
    // Separator is '-', not a space: the name becomes a URL path segment in the module API (DELETE / replace / move `/api/modules/<name>`); a space there needs URL-encoding and breaks the device's raw-path name lookup, so a device-created "Grid 2" couldn't be deleted.
    // '-' is URL-safe and readable.
    char candidate[16];
    for (int suffix = 2; suffix < 100; suffix++) {
        int n = std::snprintf(candidate, sizeof(candidate), "%s-%d", base, suffix);
        if (n < 0 || n >= static_cast<int>(sizeof(candidate))) return;  // doesn't fit name_
        if (firstByName(candidate) == nullptr) {
            mod->setName(candidate);
            return;
        }
    }
    // Loop exhausted (would mean 99 same-named siblings), degrade silently.
}

void Scheduler::deduplicateNamesInTree() {
    for (uint8_t i = 0; i < moduleCount_; i++) {
        walkAndEnsureUnique(modules_[i]);
    }
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
        // `enabled` rides the FULL state, not the per-leaf value patch, so the client only learns the new value from a full resync. Request one (the same signal a schema change sends); without it the client's cached state keeps the old `enabled` and reverts the toggle a second later.
        MoonModule::notifySchemaChanged();
        return SetControlResult::Ok;
    }

    auto& ctrls = target->controls();
    for (uint8_t i = 0; i < ctrls.count(); i++) {
        auto& c = ctrls[i];
        if (std::strcmp(c.name, controlName) != 0) continue;

        // Per-type parse + validate + apply lives in Control.cpp. A non-Ok result leaves the storage untouched, so there is no rollback to do.
        switch (applyControlValue(c, valueJson, "value")) {
            case ApplyResult::Ok:         break;
            case ApplyResult::OutOfRange: return SetControlResult::OutOfRange;
            case ApplyResult::Malformed:  return SetControlResult::Malformed;
            case ApplyResult::ReadOnly:   return SetControlResult::ReadOnly;
        }
        reactToControlChange(target, controlName);
        // LIVE STATE does not mark the tree dirty.
        // A control something drives continuously is not configuration (ControlDescriptor::live), and marking it re-stamped the debounce on every write: a 50 Hz writer kept the timer from ever expiring.
        // The module's file was never saved at all and a power cut lost everything in it, including the settings a person HAD chosen.
        // Skipping the mark here is what lets those settle and save normally.
        //
        // `c` is the descriptor just applied, so this reads the flag of the control that changed rather than asking the module.
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

/// Is this control a Bool (or the `enabled` pseudo-control, which is one)?
///
/// The byte reader needs it because a Bool alone reads back at FULL SCALE there.
/// A value of 1 cannot say whether it came from a bool or from a Uint8 holding 1.
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
    // DERIVED from the wide reader rather than repeating its per-type switch.
    // The two answer the same question in different units, and the difference is a rule, not a second lookup: a surface has 8 bits of travel.
    // A wider value clamps rather than truncating, and a Bool reads back 255 so a switch and a fader answer on one scale.
    //
    // They were two switches over the same ControlType list, which is the duplication the coding standards forbid.
    // A new control type had to be added to both, and only one of them was exercised by the surface.
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

    // 0/1 here, not the byte reader's 0/255: this answers in the control's OWN units, and a bool stores 0 or 1. A caller that wants surface units uses getControl.
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
            // A Pin is int8_t storage (ControlList::addPin), so it cannot share the Int32 case: reading four bytes from a one-byte control returns three bytes of whatever follows it.
            case ControlType::Pin:
                out = *static_cast<const int8_t*>(c.ptr);
                return true;
            // Same set the byte reader refuses: text, a file path, a password, a button. There is no number to give, so say so rather than inventing one.
            default:
                return false;
        }
    }
    return false;
}

void Scheduler::walkAndEnsureUnique(MoonModule* mod) {
    if (!mod) return;
    ensureUniqueName(mod);
    for (uint8_t i = 0; i < mod->childCount(); i++) {
        walkAndEnsureUnique(mod->child(i));
    }
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
