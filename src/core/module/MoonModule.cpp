/// MoonModule's bodies that run off the render path: the lifecycle walks, the child mutators, the control rebuild and naming. The per-frame members stay inline in the header (coding-standards.md, File shape).

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // structureVersion: quiesceForMutation moves the tree's modification count

#include <cstring>

namespace mm {

MoonModule::~MoonModule() {
    if (lastEnabledEffect_ == this) lastEnabledEffect_ = nullptr;
    delete[] children_;
}

void MoonModule::release() {
    clearStatus();
    // An override frees its own hardware; this base lets go of every resource it registered, its buffers among them.
    for (ModuleResource* r = resources_; r; r = r->next_) r->releaseResource();
    // Then the children, in reverse, a module shutting down before those it owns.
    for (uint8_t i = childCount_; i > 0; i--) children_[i-1]->release();
}

void MoonModule::applyState() {
    if (effectivelyEnabled()) {
        prepare();
        for (uint8_t i = 0; i < childCount_; i++) children_[i]->applyState();
    } else {
        release();   // recurses to children itself (reverse order)
    }
}

void MoonModule::rebuildControls() {
    // Every value patch passes here too, so hash and resync only on a real change.
    const uint32_t before = schemaSignature();
    clearControlsRecursive();
    defineControls();
    shrinkControlsRecursive();
    if (schemaChangedHook_ && schemaSignature() != before) schemaChangedHook_();
}

void MoonModule::shrinkControlsRecursive() {
    controls_.shrinkToFit();
    for (uint8_t i = 0; i < childCount_; i++) children_[i]->shrinkControlsRecursive();
}

uint32_t MoonModule::schemaSignature() const {
    Fnv1a f;
    mixSchema(f);
    return f.h;
}

void MoonModule::mixSchema(Fnv1a& f) const {
    auto mix = [&f](uint32_t v) { f.add(&v, sizeof(v)); };
    auto mixStr = [&f](const char* s) { if (s) f.add(s, std::strlen(s)); f.add(0); };
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
            mix(static_cast<uint32_t>(c.aux));   // a Progress total or another aux value, so a change differs
        }
    }
    for (uint8_t i = 0; i < childCount_; i++) children_[i]->mixSchema(f);
}

void MoonModule::clearControlsRecursive() {
    controls_.clear();
    for (uint8_t i = 0; i < childCount_; i++) children_[i]->clearControlsRecursive();
}

void MoonModule::setName(const char* n) {
    if (!n) { name_[0] = 0; return; }
    size_t len = std::strlen(n);
    if (len >= sizeof(name_)) len = sizeof(name_) - 1;
    std::memcpy(name_, n, len);
    name_[len] = 0;
}

void MoonModule::setEnabled(bool e) {
    if (enabled_ == e) return;
    quiesceForMutation();   // a worker may be in this module's tick, and onEnabled may release what that tick uses
    enabled_ = e;
    if (e && role() == ModuleRole::Effect) lastEnabledEffect_ = this;
    onEnabled(e);
}

bool MoonModule::effectivelyEnabled() const {
    for (const MoonModule* m = this; m; m = m->parent())
        if (m->respectsEnabled() && !m->enabled()) return false;
    return true;
}

void MoonModule::setParent(MoonModule* p) {
    parent_ = p;
    // Decided as it joins the tree, where the type is complete, rather than on every tick.
    held_ = p && safeMode() && heldInSafeMode();
    // Joining counts as being switched on, and leaving forgets it.
    if (p && role() == ModuleRole::Effect) lastEnabledEffect_ = this;
    else if (!p && lastEnabledEffect_ == this) lastEnabledEffect_ = nullptr;
}

bool MoonModule::readBool(const char* name, bool dflt) const {
    for (uint8_t i = 0; i < controls_.count(); i++) {
        const ControlDescriptor& c = controls_[i];
        if (c.ptr && c.type == ControlType::Bool && std::strcmp(c.name, name) == 0)
            return *static_cast<const bool*>(c.ptr);
    }
    return dflt;
}

uint8_t MoonModule::readUint8(const char* name, uint8_t dflt) const {
    for (uint8_t i = 0; i < controls_.count(); i++) {
        const ControlDescriptor& c = controls_[i];   // a slider, a dropdown index, a palette index
        if (c.ptr && std::strcmp(c.name, name) == 0 &&
            (c.type == ControlType::Uint8 || c.type == ControlType::Select ||
             c.type == ControlType::Palette))
            return *static_cast<const uint8_t*>(c.ptr);
    }
    return dflt;
}

bool MoonModule::acceptsRole(ModuleRole childRole) const {
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

void MoonModule::quiesceForMutation() {
    if (Scheduler* sched = Scheduler::instance()) sched->bumpStructureVersion();
    quiesce();
    if (quiesceRenderHook_) quiesceRenderHook_();
}

bool MoonModule::addChild(MoonModule* child) {
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

bool MoonModule::removeChild(MoonModule* child) {
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

MoonModule* MoonModule::replaceChildAt(uint8_t i, MoonModule* fresh) {
    quiesceForMutation();   // the caller deletes the child swapped out, as with removeChild
    if (i >= childCount_ || !fresh) return nullptr;
    MoonModule* old = children_[i];
    if (old) old->setParent(nullptr);
    fresh->setParent(this);
    children_[i] = fresh;
    return old;
}

bool MoonModule::moveChildTo(MoonModule* child, uint8_t newIndex) {
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

void MoonModule::deregisterResource(ModuleResource* r) {
    for (ModuleResource** p = &resources_; *p; p = &(*p)->next_) {
        if (*p == r) { *p = r->next_; return; }   // unlink
    }
}

}  // namespace mm
