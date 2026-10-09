/// @defgroup state_document_impl State document implementation
/// The walk that applies a document to the tree, one member at a time in document order. Public surface lives in StateDocument.h.
/// @{
#include "core/module/StateDocument.h"

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/module/Control.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"
#include "core/util/ModuleFactory.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

namespace {

using json::JsonDoc;
using json::JsonNode;
using json::JsonType;

// A control a script declares exists only once the next prepare has compiled the script, so a document naming it waits for that prepare.
struct Deferred {
    char module[MoonModule::kNameLen];
    char key[32];
    char* valueJson;   // `{"value":...}`, on the heap
    bool stored;       // from a stored file, so clamped and not marked dirty: it is what the file already holds
};
constexpr uint8_t kMaxDeferred = 64;   // far past a script's controls; more is refused by name rather than dropped
// On the heap only while something waits, so a device that never defers keeps the 3 KB.
Deferred* g_deferred = nullptr;
uint8_t g_deferredCount = 0;

// Free the table once nothing waits in it.
void releaseDeferredTable() {
    if (g_deferredCount) return;
    platform::free(g_deferred);
    g_deferred = nullptr;
}

// Forget what a failed document deferred, the entries from `from` on.
void dropDeferredSince(uint8_t from, StateDocumentResult& r) {
    for (uint8_t i = from; i < g_deferredCount; i++) platform::free(g_deferred[i].valueJson);
    g_deferredCount = from;
    r.deferred = 0;
    releaseDeferredTable();
}

bool defer(const char* module, const char* key, const char* valueJson, bool stored) {
    if (g_deferredCount >= kMaxDeferred) return false;
    if (!g_deferred && !(g_deferred = static_cast<Deferred*>(platform::alloc(sizeof(Deferred) * kMaxDeferred)))) return false;
    const size_t n = std::strlen(valueJson) + 1;
    char* copy = static_cast<char*>(platform::alloc(n));
    if (!copy) return false;
    std::memcpy(copy, valueJson, n);
    Deferred& d = g_deferred[g_deferredCount++];
    std::snprintf(d.module, sizeof(d.module), "%s", module);
    std::snprintf(d.key, sizeof(d.key), "%s", key);
    d.valueJson = copy;
    d.stored = stored;
    return true;
}

const ControlDescriptor* controlNamed(MoonModule& m, const char* key) {
    auto& cs = m.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, key) == 0) return &cs[i];
    return nullptr;
}

// A stored value, clamped to the control's bounds since a file an older build wrote may hold one past them; true when it changed the value.
bool storeClamped(const ControlDescriptor& c, const char* valueJson) {
    JsonSink before, after;
    writeControlValue(before, c, /*saving=*/true);
    if (applyControlValue(c, valueJson, "value", ApplyPolicy::Clamp) != ApplyResult::Ok) return false;
    writeControlValue(after, c, /*saving=*/true);
    return std::strcmp(before.data(), after.data()) != 0;
}

constexpr const char* kNameRule = "a module name has 1 to 15 characters";

// Keys a module object reads itself rather than as a control or a child.
bool reserved(const char* key) { return key[0] == '$' || std::strcmp(key, "type") == 0; }

void writeValue(JsonSink& sink, const JsonDoc& doc, const JsonNode* n);

// An array or an object, its members written in order.
void writeContainer(JsonSink& sink, const JsonDoc& doc, const JsonNode* n) {
    const bool isObject = n->type == JsonType::Object;
    sink.append(isObject ? "{" : "[");
    for (const JsonNode* c = doc.node(n->firstChild); c; c = doc.node(c->next)) {
        if (c != doc.node(n->firstChild)) sink.append(",");
        if (isObject) { sink.writeJsonString(c->key); sink.append(":"); }
        writeValue(sink, doc, c);
    }
    sink.append(isObject ? "}" : "]");
}

// Write a value node back out as JSON, the form the control write path parses.
void writeValue(JsonSink& sink, const JsonDoc& doc, const JsonNode* n) {
    switch (n->type) {
        case JsonType::Null:   sink.append("null"); break;
        case JsonType::Bool:   sink.append(n->intValue ? "true" : "false"); break;
        case JsonType::Int:    sink.appendf("%ld", n->intValue); break;
        case JsonType::String: sink.writeJsonString(n->str); break;
        case JsonType::Array:
        case JsonType::Object: writeContainer(sink, doc, n); break;
    }
}

MoonModule* childNamed(MoonModule* parent, const char* name) {
    for (uint8_t i = 0; i < parent->childCount(); i++) {
        MoonModule* c = parent->child(i);
        if (c && std::strcmp(c->name(), name) == 0) return c;
    }
    return nullptr;
}

const char* typeOf(const JsonDoc& doc, const JsonNode* obj) {
    const JsonNode* t = json::member(doc, obj, "type");
    return (t && t->type == JsonType::String) ? t->str : nullptr;
}

// The role a registered type declares, or false when no such type is registered.
bool registeredRole(const char* type, ModuleRole& role) {
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++)
        if (std::strcmp(ModuleFactory::typeName(i), type) == 0) { role = ModuleFactory::typeRole(i); return true; }
    return false;
}

struct Applier {
    Scheduler& s;
    const JsonDoc& doc;
    StateDocumentResult& r;
    StateSource source = StateSource::Request;
    bool structural = false;
    // The modules the document being applied creates, replaces or points at a new source, whose controls appear only at the rebuild.
    static constexpr uint8_t kMaxRebuilt = 16;
    const MoonModule* rebuilt[kMaxRebuilt] = {};
    uint8_t rebuiltCount = 0;
    char path[64] = {};
    size_t pathLen = 0;

    size_t push(const char* key) {
        const size_t at = pathLen;
        const int n = std::snprintf(path + pathLen, sizeof(path) - pathLen, "%s%s", pathLen ? "." : "", key);
        if (n > 0) pathLen = std::strlen(path);
        return at;
    }
    void pop(size_t at) { pathLen = at; path[at] = 0; }

    bool fail(const char* what) {
        r.ok = false;
        r.error = what;
        std::snprintf(r.where, sizeof(r.where), "%s", path);
        return false;
    }

    bool stored() const { return source != StateSource::Request; }
    bool boot() const { return source == StateSource::Boot; }

    // A stored file goes on past what this build cannot place: the failure is logged and counted, and the rest still applies.
    bool skip() {
        std::printf("state document: %s at %s, skipped\n", r.error, r.where);
        r.ok = true;
        r.error = "";
        r.where[0] = 0;
        r.skipped++;
        return true;
    }

    void markRebuilt(const MoonModule* m) {
        if (rebuiltCount < kMaxRebuilt) rebuilt[rebuiltCount++] = m;
    }
    // Past the table every module counts as rebuilt, so a large document defers rather than refuses.
    bool isRebuilt(const MoonModule* m) const {
        if (rebuiltCount >= kMaxRebuilt) return true;
        for (uint8_t i = 0; i < rebuiltCount; i++) if (rebuilt[i] == m) return true;
        return false;
    }

    void finishStructure(MoonModule* parent) {
        if (!boot()) parent->markDirty();   // a boot load restores what the file holds, so nothing is left to save
        structural = true;
        r.changes++;
    }

    // Bring a fresh module to life in the order the add path uses: bind, set up, then build or release. At boot the boot phases set up and build the whole tree after the load.
    void start(MoonModule* m) const {
        m->defineControls();
        if (boot()) return;
        m->setup();
        m->applyState();
    }

    const JsonNode* first(const JsonNode* obj) const { return doc.node(obj->firstChild); }
    const JsonNode* next(const JsonNode* n) const { return doc.node(n->next); }

    static bool nameFits(const char* name) { return name[0] && std::strlen(name) < MoonModule::kNameLen; }

    // What would fail at creation, found before anything changes: a type this build lacks, a role the parent refuses, a name the tree cannot hold.
    bool validate(MoonModule* m, const JsonNode* obj) {
        for (const JsonNode* member = first(obj); member; member = next(member)) {
            if (reserved(member->key)) continue;
            if (member->type == JsonType::Object && !validateMember(m, member)) return false;
            if (member->type == JsonType::Null && !validateRemoval(m, member)) return false;
        }
        return true;
    }

    // A `null` that names a module the user cannot remove fails here rather than halfway through the prune.
    bool validateRemoval(MoonModule* m, const JsonNode* member) {
        MoonModule* child = m ? childNamed(m, member->key) : nullptr;
        if (!child || child->userEditable()) return true;
        push(member->key);
        return fail("this module cannot be removed");
    }

    bool validateMember(MoonModule* m, const JsonNode* member) {
        const size_t at = push(member->key);
        MoonModule* child = m ? childNamed(m, member->key) : nullptr;
        const char* type = typeOf(doc, member);
        const bool fresh = type && (!child || std::strcmp(child->typeName(), type) != 0);
        if (fresh && !validateCreation(m, child, member, type)) return false;
        if (!fresh && !child) return fail("no such module, and no type to create it");
        // A child the document creates or re-types is new, so what goes under it is checked when it exists.
        if (!validate(fresh ? nullptr : child, member)) return false;
        pop(at);
        return true;
    }

    bool validateCreation(MoonModule* parent, MoonModule* existing, const JsonNode* member, const char* type) {
        ModuleRole role;
        if (!registeredRole(type, role)) return fail("unknown type");
        if (parent && !parent->acceptsRole(role)) return fail("a module of this role cannot go here");
        if (existing) return existing->userEditable() || fail("this module cannot be replaced");
        return validateNewName(member);
    }

    // Names are unique across the tree, so a new one is free only if the document names it once and the prune pass frees any module holding it.
    bool validateNewName(const JsonNode* member) {
        const char* name = member->key;
        if (!nameFits(name)) return fail(kNameRule);
        if (typedElsewhere(member, name)) return fail("that name is used twice in the document");
        MoonModule* holder = s.firstByName(name);
        if (holder && survivesPrune(holder)) return fail("that name is used elsewhere in the tree");
        return true;
    }

    // Whether the document holds `name` as a module, an object with a type, anywhere but at `here`.
    bool typedElsewhere(const JsonNode* here, const char* name) const {
        for (int i = 0; i < doc.count; i++) {
            const JsonNode* n = doc.node(i);
            if (n != here && n->type == JsonType::Object && n->key && std::strcmp(n->key, name) == 0 && typeOf(doc, n)) return true;
        }
        return false;
    }

    // Whether `m` is still in the tree once the prune pass has run, by pruneChild's rules walked down from the top level.
    bool survivesPrune(MoonModule* m) const {
        MoonModule* chain[16];
        uint8_t depth = 0;
        for (MoonModule* p = m; p && depth < 16; p = p->parent()) chain[depth++] = p;
        const JsonNode* obj = doc.rootNode();
        for (int k = depth - 1; k >= 0; k--) {
            const JsonNode* into = nullptr;
            if (prunedAt(obj, chain[k], k == depth - 1, k == 0, into)) return false;
            if (!into) return true;   // the document reaches no further down
            obj = into;
        }
        return true;
    }

    // Whether the prune pass removes `c` from under `obj`, setting `into` to its own object when the walk goes on; a re-typed `target` keeps its name.
    bool prunedAt(const JsonNode* obj, MoonModule* c, bool top, bool target, const JsonNode*& into) const {
        const JsonNode* listed = json::member(doc, obj, c->name());
        if (!listed) return !top && replacesChildren(obj) && c->userEditable() && !c->isWiredByCode();
        if (listed->type != JsonType::Object) return listed->type == JsonType::Null;
        const char* type = typeOf(doc, listed);
        if (!target && type && std::strcmp(type, c->typeName()) != 0) return true;   // re-typed, so everything under it goes
        into = listed;
        return false;
    }

    MoonModule* create(MoonModule* parent, const char* name, const char* type) {
        if (!nameFits(name)) { fail(kNameRule); return nullptr; }
        // Names are unique across the tree, so a clash is refused, except for a stored file, which must come back: it takes a free name.
        char freeName[MoonModule::kNameLen];
        if (s.firstByName(name)) {
            if (!stored()) { fail("that name is used elsewhere in the tree"); return nullptr; }
            if (!s.freeName(name, freeName, sizeof(freeName))) { fail("no free name"); return nullptr; }
            name = freeName;
        }
        MoonModule* m = ModuleFactory::create(type);
        if (!m) { fail("unknown type"); return nullptr; }
        m->setName(name);
        if (!parent->acceptsRole(m->role())) { delete m; fail("a module of this role cannot go here"); return nullptr; }
        if (!parent->addChild(m)) { delete m; fail("the parent takes no more children"); return nullptr; }
        start(m);
        finishStructure(parent);
        markRebuilt(m);
        return m;
    }

    MoonModule* replace(MoonModule* parent, MoonModule* old, const char* type) {
        if (!old->userEditable()) { fail("this module cannot be replaced"); return nullptr; }
        MoonModule* fresh = ModuleFactory::create(type);
        if (!fresh) { fail("unknown type"); return nullptr; }
        if (!parent->acceptsRole(fresh->role())) { delete fresh; fail("a module of this role cannot go here"); return nullptr; }
        fresh->setName(old->name());   // the document addresses it by this name
        uint8_t index = 0;
        while (index < parent->childCount() && parent->child(index) != old) index++;
        parent->replaceChildAt(index, fresh);
        start(fresh);
        old->release();
        Scheduler::deleteTree(old);
        finishStructure(parent);
        markRebuilt(fresh);
        return fresh;
    }

    bool remove(MoonModule* parent, MoonModule* m) {
        if (!m->userEditable()) return fail("this module cannot be removed");
        parent->removeChild(m);
        m->release();
        Scheduler::deleteTree(m);
        finishStructure(parent);
        return true;
    }

    // Every editable child, back to front since removing compacts the array.
    bool removeChildren(MoonModule* m) {
        for (int k = static_cast<int>(m->childCount()) - 1; k >= 0; k--) {
            MoonModule* c = m->child(static_cast<uint8_t>(k));
            if (c && c->userEditable() && !remove(m, c)) return false;
        }
        return true;
    }

    // What a failed control write says, or null when it took.
    static const char* failureOf(Scheduler::SetControlResult result) {
        switch (result) {
            case Scheduler::SetControlResult::Ok:              return nullptr;
            case Scheduler::SetControlResult::ModuleNotFound:  return "no such module";
            case Scheduler::SetControlResult::ControlNotFound: return "no such control";
            case Scheduler::SetControlResult::OutOfRange:      return "value out of range";
            case Scheduler::SetControlResult::Malformed:       return "value malformed";
            case Scheduler::SetControlResult::ReadOnly:        return "the control is read-only";
        }
        return "no such control";
    }

    // `{"value":...}`, the body the control write path takes; false when it does not fit.
    bool wrapValue(JsonSink& sink, const JsonNode* value) const {
        sink.append("{\"value\":");
        writeValue(sink, doc, value);
        sink.append("}");
        return !sink.overflowed();
    }

    bool setControl(MoonModule* m, const char* key, const JsonNode* value) {
        JsonSink sink;
        if (!wrapValue(sink, value)) return fail("out of memory");
        const Scheduler::SetControlResult result = s.setControl(m->name(), key, sink.data());
        const bool compiles = m->declaresControlsAtPrepare();
        if (result == Scheduler::SetControlResult::Ok && compiles && m->affectsPrepare(key)) markRebuilt(m);
        // A control a script declares exists only after the rebuild, so it waits for it; on a module the document leaves as it is, an unknown control is a mistake.
        if (result == Scheduler::SetControlResult::ControlNotFound && compiles && isRebuilt(m)) return deferControl(m, key, sink.data(), false);
        if (const char* why = failureOf(result)) return fail(why);
        r.changes++;
        return true;
    }

    bool deferControl(MoonModule* m, const char* key, const char* valueJson, bool fromFile) {
        if (!defer(m->name(), key, valueJson, fromFile)) return fail("too many controls wait for the rebuild");
        r.deferred++;
        return true;
    }

    static bool isChildMember(const JsonNode* member) { return member->type == JsonType::Object || member->type == JsonType::Null; }

    // A stored file sets its values twice around a rebuild, since a driver's `peripheral` decides which pin controls exist.
    bool applyStored(MoonModule* m, const JsonNode* obj) {
        uint64_t changed = 0;
        storeValues(m, obj, false, changed);
        m->rebuildControls();
        storeValues(m, obj, true, changed);
        for (const JsonNode* member = first(obj); member; member = next(member))
            if (!reserved(member->key) && isChildMember(member) && !applyMember(m, member)) return false;
        if (replacesChildren(obj)) reorder(m, obj);
        return true;
    }

    // One pass over the value members, `changed` holding a bit per member in document order; past 64 members a value reacts to the second pass alone.
    void storeValues(MoonModule* m, const JsonNode* obj, bool final, uint64_t& changed) {
        uint8_t index = 0;
        for (const JsonNode* member = first(obj); member; member = next(member)) {
            if (reserved(member->key) || isChildMember(member)) continue;
            const uint64_t bit = index < 64 ? uint64_t{1} << index : 0;
            index++;
            bool was = (changed & bit) != 0;
            const size_t at = push(member->key);
            if (storeValue(m, member, final, was) || skip()) pop(at);
            if (was) changed |= bit;
        }
    }

    // `changed` in and out: whether the first pass changed this value, which the second pass reacts to.
    bool storeValue(MoonModule* m, const JsonNode* member, bool final, bool& changed) {
        if (std::strcmp(member->key, "enabled") == 0) return storeEnabled(m, member, final);
        JsonSink sink;
        if (!wrapValue(sink, member)) return fail("out of memory");
        const ControlDescriptor* c = controlNamed(*m, member->key);
        if (!c) return storeUnknown(m, member->key, sink.data(), final);
        // A stored file sets only what the device itself saves: a live input an older build wrote stays as the device starts it.
        if (!isPersistable(*c)) return true;
        // At boot nothing reacts, so the value is set without comparing it to what it was.
        if (boot()) { applyControlValue(*c, sink.data(), "value", ApplyPolicy::Clamp); return true; }
        const bool live = c->live;
        changed = storeClamped(*c, sink.data()) || changed;
        if (final && changed) react(m, member->key, live);
        return true;
    }

    bool storeEnabled(MoonModule* m, const JsonNode* member, bool final) {
        const bool on = member->intValue != 0;
        if (!final || on == m->enabledSetting()) return true;
        m->setEnabled(on);
        if (!boot()) { m->markDirty(); structural = true; r.changes++; }
        return true;
    }

    // A script's controls appear when it compiles; any other unknown key is one this build lacks, dropped as the file is rewritten.
    bool storeUnknown(MoonModule* m, const char* key, const char* valueJson, bool final) {
        if (!final || !m->declaresControlsAtPrepare()) return true;
        return deferControl(m, key, valueJson, true);
    }

    void react(MoonModule* m, const char* key, bool live) {
        s.reactToControlChange(m, key);   // rebuilds the list, so no descriptor is read after it
        if (!live) m->markDirty();
        r.changes++;
    }

    bool replacesChildren(const JsonNode* obj) const {
        const JsonNode* patch = json::member(doc, obj, "$patch");
        return patch && patch->type == JsonType::String && std::strcmp(patch->str, "replace") == 0;
    }

    // Remove every child the document removes, through the whole subtree, before anything is created, so a name can move from one branch to another.
    bool prune(MoonModule* m, const JsonNode* obj) {
        const bool replaceChildren = replacesChildren(obj);
        for (int i = static_cast<int>(m->childCount()) - 1; i >= 0; i--) {   // back to front, since removing compacts the array
            MoonModule* c = m->child(static_cast<uint8_t>(i));
            if (c && !pruneChild(m, c, json::member(doc, obj, c->name()), replaceChildren)) return false;
        }
        return true;
    }

    // A module main.cpp wires keeps its type when a stored file names another there, since the file may predate it.
    bool keepsWired(const MoonModule* c) const { return stored() && c->isWiredByCode(); }

    // `"$patch":"replace"` removes what the document leaves out, except a module main.cpp wires, which every boot puts back: a document replaces content, not apparatus.
    bool pruneChild(MoonModule* m, MoonModule* c, const JsonNode* listed, bool replaceChildren) {
        const size_t at = push(c->name());
        const bool ok = pruneOne(m, c, listed, replaceChildren) || (stored() && skip());
        if (ok) pop(at);
        return ok;
    }

    // Remove the child, empty a re-typed one, or walk into a kept one.
    bool pruneOne(MoonModule* m, MoonModule* c, const JsonNode* listed, bool replaceChildren) {
        if (!listed) return !(replaceChildren && c->userEditable() && !c->isWiredByCode()) || remove(m, c);
        if (listed->type == JsonType::Null) return remove(m, c);
        if (listed->type != JsonType::Object) return true;
        const char* newType = typeOf(doc, listed);
        if (!newType || std::strcmp(newType, c->typeName()) == 0) return prune(c, listed);
        // A re-typed child is replaced in place, so everything under it goes now, freeing its names for elsewhere; a wired one keeps its type, which applyChild reports.
        return keepsWired(c) || removeChildren(c);
    }

    // Apply a module object's members to `m`, in document order, once prune() has removed what goes.
    bool apply(MoonModule* m, const JsonNode* obj) {
        if (stored()) return applyStored(m, obj);
        for (const JsonNode* member = first(obj); member; member = next(member))
            if (!reserved(member->key) && !applyMember(m, member)) return false;
        if (replacesChildren(obj)) reorder(m, obj);
        return true;
    }

    bool applyMember(MoonModule* m, const JsonNode* member) {
        const size_t at = push(member->key);
        bool ok = true;
        if (member->type == JsonType::Object) {
            ok = applyChild(m, member);
        } else if (member->type == JsonType::Null) {
            MoonModule* child = childNamed(m, member->key);
            ok = !child || remove(m, child);   // absent already: nothing to remove
        } else {
            ok = setControl(m, member->key, member);
        }
        if (!ok && stored()) ok = skip();
        if (ok) pop(at);
        return ok;
    }

    bool applyChild(MoonModule* m, const JsonNode* member) {
        MoonModule* child = childNamed(m, member->key);
        const char* type = typeOf(doc, member);
        const bool retyped = child && type && std::strcmp(child->typeName(), type) != 0;
        if (!child && !type) return fail("no such module, and no type to create it");
        if (retyped && keepsWired(child)) return fail("a module wired in code keeps its type");
        if (!child) child = create(m, member->key, type);
        else if (retyped) child = replace(m, child, type);
        return child && apply(child, member);
    }

    // Unlisted children (those main.cpp wires) keep their places and listed ones fill theirs in document order, each moved only when out of place.
    void reorder(MoonModule* m, const JsonNode* obj) {
        const uint8_t count = m->childCount();
        auto** order = static_cast<MoonModule**>(platform::alloc(count * sizeof(MoonModule*)));
        if (!order) return;
        for (uint8_t i = 0; i < count; i++) order[i] = m->child(i);
        const JsonNode* member = first(obj);
        for (uint8_t i = 0; i < count; i++) {
            if (!listsModule(obj, order[i])) continue;   // unlisted: stays where it is
            if (MoonModule* listed = nextListed(m, member)) order[i] = listed;
        }
        for (uint8_t i = 0; i < count; i++)
            if (m->child(i) != order[i] && m->moveChildTo(order[i], i)) finishStructure(m);   // an order is state, saved and counted like any change
        platform::free(order);
    }

    bool listsModule(const JsonNode* obj, const MoonModule* c) const {
        const JsonNode* listed = json::member(doc, obj, c->name());
        return listed && listed->type == JsonType::Object;
    }

    // The next child the document lists from `member` on, in its order, with `member` moved past it.
    MoonModule* nextListed(MoonModule* m, const JsonNode*& member) const {
        MoonModule* found = nullptr;
        for (; member && !found; member = next(member))
            if (!reserved(member->key) && member->type == JsonType::Object) found = childNamed(m, member->key);
        return found;
    }

    // One pass over the root's members: 0 checks what creation needs (skipped for a stored file), 1 removes, 2 applies the rest.
    bool runPass(int pass, const JsonNode* root) {
        if (pass == 0 && stored()) return true;
        for (const JsonNode* member = first(root); member; member = next(member)) {
            if (reserved(member->key)) continue;   // a root `$` key is the file's own, such as a preset's `$slot`
            const size_t at = push(member->key);
            const bool ok = runTop(pass, member);
            if (!ok && !stored()) return false;
            if (!ok) skip();
            pop(at);
        }
        return true;
    }

    bool runTop(int pass, const JsonNode* member) {
        MoonModule* top = topNamed(member->key);
        if (!top) return fail("no such top-level module");
        if (member->type != JsonType::Object) return fail("a top-level module takes an object");
        if (pass == 0) return validate(top, member);
        return pass == 1 ? prune(top, member) : apply(top, member);
    }

    // The top level is the fixed set main.cpp wires, so a document only reaches into it.
    MoonModule* topNamed(const char* name) const {
        for (uint8_t m = 0; m < s.moduleCount(); m++) {
            MoonModule* candidate = s.module(m);
            if (candidate && std::strcmp(candidate->name(), name) == 0) return candidate;
        }
        return nullptr;
    }
};


// A secret stays on the device: a document is shown, copied and shared, and applying one without the key leaves the secret as it is.
bool holdsSecret(const ControlDescriptor& c) {
    if (c.type == ControlType::Password) return true;
    return c.type == ControlType::List && c.ptr && static_cast<const ListSource*>(c.ptr)->listHoldsSecrets();
}

// One module as a member, `"name":{...}`, which applied gives back exactly this subtree; `withType` for a module a document may create, `withSecrets` for the device's own config file.
void writeMember(JsonSink& sink, MoonModule& m, bool withType, bool withSecrets) {
    sink.writeJsonString(m.name());
    sink.append(":{");
    if (withType) {
        sink.append("\"type\":");
        sink.writeJsonString(m.typeName());
        sink.append(",");
    }
    // Exactly this subtree: children it does not list are removed when it is applied.
    sink.append("\"$patch\":\"replace\"");
    auto& cs = m.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (!isPersistable(cs[i]) || (holdsSecret(cs[i]) && !withSecrets)) continue;
        sink.append(",");
        sink.writeJsonString(cs[i].name);
        sink.append(":");
        writeControlValue(sink, cs[i], /*saving=*/true);
    }
    sink.appendf(",\"enabled\":%s", m.enabledSetting() ? "true" : "false");
    for (uint8_t i = 0; i < m.childCount(); i++) {
        MoonModule* c = m.child(i);
        if (!c) continue;
        sink.append(",");
        writeMember(sink, *c, true, withSecrets);
    }
    sink.append("}");
}

// Open the members leading down to `m`, from the top level, each as `"name":{`; the depth, which is how many braces close them.
uint8_t openPath(JsonSink& sink, MoonModule* m) {
    constexpr uint8_t kMaxDepth = 16;
    MoonModule* path[kMaxDepth];
    uint8_t depth = 0;
    for (MoonModule* a = m; a && depth < kMaxDepth; a = a->parent()) path[depth++] = a;
    for (uint8_t i = depth; i-- > 0;) {
        sink.writeJsonString(path[i]->name());
        sink.append(":{");
    }
    return depth;
}

void closePath(JsonSink& sink, uint8_t depth) {
    for (uint8_t i = 0; i < depth; i++) sink.append("}");
}

// One prepare and one full resync for the whole document, however much it changed; the scripts compile at that prepare, and the controls they declare are set right after it.
void settle(Scheduler& scheduler, bool structural, const StateDocumentResult& r) {
    if (structural) {
        scheduler.requestPrepareTree();
        MoonModule::notifySchemaChanged();
    }
    if (r.deferred) scheduler.requestPrepareTree();
    if (r.changes) scheduler.noteDirty();
}

}  // namespace

StateDocumentResult applyStateDocument(Scheduler& scheduler, const char* text, StateSource source) {
    StateDocumentResult r;
    const uint8_t deferredBefore = g_deferredCount;
    JsonDoc doc;
    if (!json::parse(text, doc)) { r.ok = false; r.error = doc.outOfMemory ? "out of memory" : "malformed JSON"; return r; }
    const JsonNode* root = doc.rootNode();
    if (root->type != JsonType::Object) { r.ok = false; r.error = "a document is a JSON object"; return r; }

    Applier a{scheduler, doc, r, source};
    for (int pass = 0; pass < 3 && a.runPass(pass, root); pass++) {}
    // A failed document leaves nothing waiting for the rebuild: what came before the failure stays applied, but no deferred write lands after it.
    if (!r.ok) dropDeferredSince(deferredBefore, r);
    // At boot the phases after the load set up, build and prepare the whole tree, and the file already holds what was loaded.
    if (source != StateSource::Boot) settle(scheduler, a.structural, r);
    return r;
}


void writeStateResult(JsonSink& sink, const StateDocumentResult& r) {
    if (r.ok) {
        sink.appendf("{\"ok\":true,\"changes\":%u", static_cast<unsigned>(r.changes));
    } else {
        sink.append("{\"error\":");
        sink.writeJsonString(r.error);
        sink.append(",\"at\":");
        sink.writeJsonString(r.where);
        sink.appendf(",\"changes\":%u", static_cast<unsigned>(r.changes));
    }
    if (r.deferred) sink.appendf(",\"deferred\":%u", static_cast<unsigned>(r.deferred));
    sink.append("}");
}

StateDocumentResult applyStateAt(Scheduler& scheduler, MoonModule& m, const char* body, StateSource source) {
    JsonSink sink;
    sink.append("{");
    const uint8_t depth = openPath(sink, &m);
    sink.append(body);
    closePath(sink, depth);
    sink.append("}");
    if (sink.overflowed()) {
        StateDocumentResult r;
        r.ok = false;
        r.error = "out of memory";
        return r;
    }
    return applyStateDocument(scheduler, sink.data(), source);
}

void writeStateMember(JsonSink& sink, MoonModule& m, bool withSecrets) {
    // Its ancestors are the path to it, written as plain names, so the member merges into what is around it.
    const uint8_t depth = openPath(sink, m.parent());
    writeMember(sink, m, /*withType=*/depth > 0, withSecrets);   // a top-level module is never created
    closePath(sink, depth);
}

namespace {

// The flat file's values directly under `prefix`, each as `,"key":value`.
void writeFlatValues(JsonSink& out, const JsonDoc& doc, const char* prefix) {
    const JsonNode* root = doc.rootNode();
    const size_t pl = std::strlen(prefix);
    for (const JsonNode* n = doc.node(root->firstChild); n; n = doc.node(n->next)) {
        if (std::strncmp(n->key, prefix, pl) != 0) continue;
        const char* rest = n->key + pl;
        if (std::strchr(rest, '.') || std::strcmp(rest, "type") == 0 || std::strcmp(rest, "$name") == 0) continue;
        out.append(",");
        out.writeJsonString(rest);
        out.append(":");
        writeValue(out, doc, n);
    }
}

// The most children a flat level holds, one bit each in the taken mask.
constexpr uint8_t kMaxFlatChildren = 32;

// The first untaken live child of `type`, marked taken; each live child matches one saved child of its type, in order.
MoonModule* takeLive(MoonModule* live, const char* type, uint32_t& taken) {
    for (uint8_t c = 0; live && c < live->childCount() && c < kMaxFlatChildren; c++) {
        MoonModule* lc = live->child(c);
        if (!lc || (taken & (1u << c)) || std::strcmp(lc->typeName(), type) != 0) continue;
        taken |= 1u << c;
        return lc;
    }
    return nullptr;
}

// A saved child's name: its saved `$name`, else the live child's it matched, since main.cpp names what it wires, else its type's default, else its type.
const char* flatName(const JsonDoc& doc, const char* childPrefix, const MoonModule* match, const char* type) {
    char key[64];
    std::snprintf(key, sizeof(key), "%s$name", childPrefix);
    const JsonNode* saved = json::member(doc, doc.rootNode(), key);
    if (saved && saved->type == JsonType::String && saved->str[0]) return saved->str;
    if (match) return match->name();
    const char* fallback = ModuleFactory::defaultNameOf(type);
    return fallback ? fallback : type;
}

// `base`, or `base-2`, `base-3` and on, until none of the `count` earlier siblings holds it.
void distinctName(char* out, size_t len, const char* base, const char (*used)[MoonModule::kNameLen], uint8_t count) {
    std::snprintf(out, len, "%s", base);
    for (int k = 2; k < 100; k++) {
        bool clash = false;
        for (uint8_t u = 0; u < count && !clash; u++) clash = std::strcmp(used[u], out) == 0;
        if (!clash) return;
        std::snprintf(out, len, "%.12s-%d", base, k);
    }
}

// The flat file's members under `prefix` as one module object, its children after its values; `live` is the module already in the tree, which names the children main.cpp wired.
void writeFlatNode(JsonSink& out, const JsonDoc& doc, const char* prefix, MoonModule* live, const char* type, const char* name) {
    out.writeJsonString(name);
    out.append(":{");
    if (type) {
        out.append("\"type\":");
        out.writeJsonString(type);
        out.append(",");
    }
    out.append("\"$patch\":\"replace\"");
    writeFlatValues(out, doc, prefix);
    // The sibling names on the heap, since this runs on the boot stack once per level.
    uint32_t taken = 0;
    auto* used = static_cast<char (*)[MoonModule::kNameLen]>(platform::alloc(kMaxFlatChildren * MoonModule::kNameLen));
    for (uint8_t i = 0; used && i < kMaxFlatChildren; i++) {
        char key[64], childPrefix[48];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        std::snprintf(key, sizeof(key), "%stype", childPrefix);
        const JsonNode* t = json::member(doc, doc.rootNode(), key);
        if (!t || t->type != JsonType::String) break;
        MoonModule* match = takeLive(live, t->str, taken);
        distinctName(used[i], sizeof(used[i]), flatName(doc, childPrefix, match, t->str), used, i);
        out.append(",");
        writeFlatNode(out, doc, childPrefix, match, t->str, used[i]);
    }
    platform::free(used);
    out.append("}");
}

// A flat file always records its module's `enabled` at the root, where a document holds only its module.
bool flatRoot(const JsonDoc& doc) {
    const JsonNode* root = doc.rootNode();
    return root && root->type == JsonType::Object && json::member(doc, root, "enabled");
}

}  // namespace

bool isFlatConfig(const char* text) {
    JsonDoc doc;
    return json::parse(text, doc) && flatRoot(doc);
}

bool flatToStateDocument(const char* text, MoonModule& top, JsonSink& out) {
    JsonDoc doc;
    if (!json::parse(text, doc) || !flatRoot(doc)) return false;
    out.append("{");
    writeFlatNode(out, doc, "", &top, nullptr, top.name());
    out.append("}");
    return !out.overflowed();
}

void applyDeferredControls(Scheduler& scheduler) {
    for (uint8_t i = 0; i < g_deferredCount; i++) {
        Deferred& d = g_deferred[i];
        bool ok = true;
        if (d.stored) {
            // What the file holds, so clamped, reacted to when it changed something, and never marked for a save.
            MoonModule* m = scheduler.firstByName(d.module);
            const ControlDescriptor* c = m ? controlNamed(*m, d.key) : nullptr;
            ok = c != nullptr;
            if (c && storeClamped(*c, d.valueJson)) scheduler.reactToControlChange(m, d.key);
        } else {
            ok = scheduler.setControl(d.module, d.key, d.valueJson) == Scheduler::SetControlResult::Ok;
        }
        if (!ok) std::printf("state document: %s.%s did not apply after the rebuild\n", d.module, d.key);
        platform::free(d.valueJson);
    }
    g_deferredCount = 0;
    releaseDeferredTable();
}

}  // namespace mm

/// @}
