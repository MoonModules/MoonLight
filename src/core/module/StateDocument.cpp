/// @defgroup state_document_impl State document implementation
/// The walk that applies a document to the tree, one member at a time in document order.
/// Public surface lives in StateDocument.h.
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

bool defer(const char* module, const char* key, const char* valueJson) {
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
    return true;
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
        parent->markDirty();
        structural = true;
        r.changes++;
    }

    // Bring a fresh module to life in the order the add path uses: bind, set up, then build or release.
    static void start(MoonModule* m) {
        m->defineControls();
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
        if (!listed) return !top && replacesChildren(obj) && c->userEditable();
        if (listed->type != JsonType::Object) return listed->type == JsonType::Null;
        const char* type = typeOf(doc, listed);
        if (!target && type && std::strcmp(type, c->typeName()) != 0) return true;   // re-typed, so everything under it goes
        into = listed;
        return false;
    }

    MoonModule* create(MoonModule* parent, const char* name, const char* type) {
        if (!nameFits(name)) { fail(kNameRule); return nullptr; }
        // Names are unique across the tree, so a name in use elsewhere would be renamed on creation, and the same document would never find it again.
        if (s.firstByName(name)) { fail("that name is used elsewhere in the tree"); return nullptr; }
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

    bool setControl(MoonModule* m, const char* key, const JsonNode* value) {
        JsonSink sink;
        sink.append("{\"value\":");
        writeValue(sink, doc, value);
        sink.append("}");
        if (sink.overflowed()) return fail("out of memory");
        const Scheduler::SetControlResult result = s.setControl(m->name(), key, sink.data());
        const bool compiles = m->declaresControlsAtPrepare();
        if (result == Scheduler::SetControlResult::Ok && compiles && m->affectsPrepare(key)) markRebuilt(m);
        // A control a script declares exists only after the rebuild, so it waits for it; on a module the document leaves as it is, an unknown control is a mistake.
        if (result == Scheduler::SetControlResult::ControlNotFound && compiles && isRebuilt(m)) return deferControl(m, key, sink.data());
        if (const char* why = failureOf(result)) return fail(why);
        r.changes++;
        return true;
    }

    bool deferControl(MoonModule* m, const char* key, const char* valueJson) {
        if (!defer(m->name(), key, valueJson)) return fail("too many controls wait for the rebuild");
        r.deferred++;
        return true;
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

    bool pruneChild(MoonModule* m, MoonModule* c, const JsonNode* listed, bool replaceChildren) {
        const bool kept = listed && listed->type == JsonType::Object;
        const bool removed = listed ? listed->type == JsonType::Null : replaceChildren && c->userEditable();
        const char* newType = kept ? typeOf(doc, listed) : nullptr;
        const size_t at = push(c->name());
        bool ok = true;
        if (removed) ok = remove(m, c);
        // A re-typed child is replaced in place, so everything under it goes now, freeing its names for elsewhere.
        else if (newType && std::strcmp(newType, c->typeName()) != 0) ok = removeChildren(c);
        else if (kept) ok = prune(c, listed);
        if (ok) pop(at);
        return ok;
    }

    // Apply a module object's members to `m`, in document order, once prune() has removed what goes.
    bool apply(MoonModule* m, const JsonNode* obj) {
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
        if (ok) pop(at);
        return ok;
    }

    bool applyChild(MoonModule* m, const JsonNode* member) {
        MoonModule* child = childNamed(m, member->key);
        const char* type = typeOf(doc, member);
        if (!child && !type) return fail("no such module, and no type to create it");
        if (!child) child = create(m, member->key, type);
        else if (type && std::strcmp(child->typeName(), type) != 0) child = replace(m, child, type);
        return child && apply(child, member);
    }

    // Exactly the listed children, in the document's order.
    void reorder(MoonModule* m, const JsonNode* obj) {
        uint8_t index = 0;
        for (const JsonNode* member = first(obj); member; member = next(member)) {
            MoonModule* c = (reserved(member->key) || member->type != JsonType::Object) ? nullptr : childNamed(m, member->key);
            if (!c) continue;
            if (m->moveChildTo(c, index)) finishStructure(m);   // an order is state, saved and counted like any change
            index++;
        }
    }

    // One pass over the root's members: 0 checks what creation needs, 1 removes, 2 applies the rest.
    bool runPass(int pass, const JsonNode* root) {
        for (const JsonNode* member = first(root); member; member = next(member)) {
            if (reserved(member->key)) continue;   // a root `$` key is the file's own, such as a preset's `$slot`
            const size_t at = push(member->key);
            MoonModule* top = topNamed(member->key);
            if (!top) return fail("no such top-level module");
            if (member->type != JsonType::Object) return fail("a top-level module takes an object");
            const bool ok = pass == 0 ? validate(top, member) : pass == 1 ? prune(top, member) : apply(top, member);
            if (!ok) return false;
            pop(at);
        }
        return true;
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

// One module as a member, `"name":{...}`, which applied gives back exactly this subtree; `withType` for a module a document may create.
void writeMember(JsonSink& sink, MoonModule& m, bool withType) {
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
        if (!isPersistable(cs[i]) || holdsSecret(cs[i])) continue;
        sink.append(",");
        sink.writeJsonString(cs[i].name);
        sink.append(":");
        writeControlValue(sink, cs[i], /*saving=*/true);
    }
    sink.appendf(",\"enabled\":%s", m.enabled() ? "true" : "false");
    for (uint8_t i = 0; i < m.childCount(); i++) {
        MoonModule* c = m.child(i);
        if (!c) continue;
        sink.append(",");
        writeMember(sink, *c, true);
    }
    sink.append("}");
}

}  // namespace

StateDocumentResult applyStateDocument(Scheduler& scheduler, const char* text) {
    StateDocumentResult r;
    const uint8_t deferredBefore = g_deferredCount;
    JsonDoc doc;
    if (!json::parse(text, doc)) { r.ok = false; r.error = doc.outOfMemory ? "out of memory" : "malformed JSON"; return r; }
    const JsonNode* root = doc.rootNode();
    if (root->type != JsonType::Object) { r.ok = false; r.error = "a document is a JSON object"; return r; }

    Applier a{scheduler, doc, r};
    for (int pass = 0; pass < 3 && a.runPass(pass, root); pass++) {}
    // A failed document leaves nothing waiting for the rebuild: what came before the failure stays applied, but no deferred write lands after it.
    if (!r.ok) dropDeferredSince(deferredBefore, r);
    // One prepare and one full resync for the whole document, however much it changed.
    if (a.structural) {
        scheduler.requestPrepareTree();
        MoonModule::notifySchemaChanged();
    }
    // The scripts compile at that prepare, and the controls they declare are set right after it.
    if (r.deferred) scheduler.requestPrepareTree();
    if (r.changes) scheduler.noteDirty();
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

void writeStateMember(JsonSink& sink, MoonModule& m) {
    // Its ancestors are the path to it, written as plain names, so the member merges into what is around it.
    constexpr uint8_t kMaxDepth = 16;
    MoonModule* path[kMaxDepth];
    uint8_t depth = 0;
    for (MoonModule* a = m.parent(); a && depth < kMaxDepth; a = a->parent()) path[depth++] = a;
    for (uint8_t i = depth; i-- > 0;) {
        sink.writeJsonString(path[i]->name());
        sink.append(":{");
    }
    writeMember(sink, m, /*withType=*/depth > 0);   // a top-level module is never created
    for (uint8_t i = 0; i < depth; i++) sink.append("}");
}

void applyDeferredControls(Scheduler& scheduler) {
    for (uint8_t i = 0; i < g_deferredCount; i++) {
        Deferred& d = g_deferred[i];
        if (scheduler.setControl(d.module, d.key, d.valueJson) != Scheduler::SetControlResult::Ok)
            std::printf("state document: %s.%s did not apply after the rebuild\n", d.module, d.key);
        platform::free(d.valueJson);
    }
    g_deferredCount = 0;
    releaseDeferredTable();
}

}  // namespace mm

/// @}
