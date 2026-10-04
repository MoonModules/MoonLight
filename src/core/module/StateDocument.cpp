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
    char module[16];
    char key[32];
    char* valueJson;   // `{"value":...}`, on the heap
};
constexpr uint8_t kMaxDeferred = 64;   // far past a script's controls; more is refused by name rather than dropped
Deferred g_deferred[kMaxDeferred];
uint8_t g_deferredCount = 0;

bool defer(const char* module, const char* key, const char* valueJson) {
    if (g_deferredCount >= kMaxDeferred) return false;
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

// Keys a module object reads itself rather than as a control or a child.
bool reserved(const char* key) { return key[0] == '$' || std::strcmp(key, "type") == 0; }

// Write a value node back out as JSON, the form the control write path parses.
void writeValue(JsonSink& sink, const JsonDoc& doc, const JsonNode* n) {
    switch (n->type) {
        case JsonType::Null:   sink.append("null"); break;
        case JsonType::Bool:   sink.append(n->intValue ? "true" : "false"); break;
        case JsonType::Int:    sink.appendf("%ld", n->intValue); break;
        case JsonType::String: sink.writeJsonString(n->str); break;
        case JsonType::Array:
        case JsonType::Object: {
            const bool isObject = n->type == JsonType::Object;
            sink.append(isObject ? "{" : "[");
            bool first = true;
            for (int i = n->firstChild; i >= 0;) {
                const JsonNode* c = doc.node(i);
                if (!c) break;
                if (!first) sink.append(",");
                if (isObject) { sink.writeJsonString(c->key); sink.append(":"); }
                writeValue(sink, doc, c);
                first = false;
                i = c->next;
            }
            sink.append(isObject ? "}" : "]");
            break;
        }
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

    MoonModule* create(MoonModule* parent, const char* name, const char* type) {
        if (!name[0] || std::strlen(name) >= 16) { fail("a module name has 1 to 15 characters"); return nullptr; }
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

    bool setControl(MoonModule* m, const char* key, const JsonNode* value) {
        JsonSink sink;
        sink.append("{\"value\":");
        writeValue(sink, doc, value);
        sink.append("}");
        if (sink.overflowed()) return fail("out of memory");
        const Scheduler::SetControlResult result = s.setControl(m->name(), key, sink.data());
        if (result == Scheduler::SetControlResult::Ok && m->declaresControlsAtPrepare() && m->affectsPrepare(key)) markRebuilt(m);
        // A control a script declares exists only after the rebuild, so it waits for it; on a module the document leaves as it is, an unknown control is a mistake.
        if (result == Scheduler::SetControlResult::ControlNotFound && m->declaresControlsAtPrepare() && isRebuilt(m)) {
            if (!defer(m->name(), key, sink.data())) return fail("too many controls wait for the rebuild");
            r.deferred++;
            return true;
        }
        switch (result) {
            case Scheduler::SetControlResult::Ok:              r.changes++; return true;
            case Scheduler::SetControlResult::ModuleNotFound:  return fail("no such module");
            case Scheduler::SetControlResult::ControlNotFound: return fail("no such control");
            case Scheduler::SetControlResult::OutOfRange:      return fail("value out of range");
            case Scheduler::SetControlResult::Malformed:       return fail("value malformed");
            case Scheduler::SetControlResult::ReadOnly:        return fail("the control is read-only");
        }
        return fail("no such control");
    }

    bool replacesChildren(const JsonNode* obj) const {
        const JsonNode* patch = json::member(doc, obj, "$patch");
        return patch && patch->type == JsonType::String && std::strcmp(patch->str, "replace") == 0;
    }

    // Remove every child the document removes, through the whole subtree, before anything is created, so a name can move from one branch to another.
    bool prune(MoonModule* m, const JsonNode* obj) {
        const bool replaceChildren = replacesChildren(obj);
        // Back to front, since removing compacts the array.
        for (int i = static_cast<int>(m->childCount()) - 1; i >= 0; i--) {
            MoonModule* c = m->child(static_cast<uint8_t>(i));
            if (!c) continue;
            const JsonNode* listed = json::member(doc, obj, c->name());
            const bool kept = listed && listed->type == JsonType::Object;
            const bool removed = listed ? listed->type == JsonType::Null : replaceChildren && c->userEditable();
            const size_t at = push(c->name());
            if (removed && !remove(m, c)) return false;
            if (kept && !prune(c, listed)) return false;
            pop(at);
        }
        return true;
    }

    // Apply a module object's members to `m`, in document order, once prune() has removed what goes.
    bool apply(MoonModule* m, const JsonNode* obj) {
        const bool replaceChildren = replacesChildren(obj);
        for (int i = obj->firstChild; i >= 0;) {
            const JsonNode* member = doc.node(i);
            if (!member) break;
            i = member->next;
            if (reserved(member->key)) continue;
            const size_t at = push(member->key);
            if (member->type == JsonType::Object) {
                MoonModule* child = childNamed(m, member->key);
                const char* type = typeOf(doc, member);
                if (!child) {
                    if (!type) return fail("no such module, and no type to create it");
                    child = create(m, member->key, type);
                } else if (type && std::strcmp(child->typeName(), type) != 0) {
                    child = replace(m, child, type);
                }
                if (!child || !apply(child, member)) return false;
            } else if (member->type == JsonType::Null) {
                MoonModule* child = childNamed(m, member->key);
                if (child && !remove(m, child)) return false;   // absent already: nothing to remove
            } else if (!setControl(m, member->key, member)) {
                return false;
            }
            pop(at);
        }
        if (replaceChildren) {
            // Exactly the listed children, in the document's order.
            uint8_t index = 0;
            for (int i = obj->firstChild; i >= 0;) {
                const JsonNode* member = doc.node(i);
                if (!member) break;
                i = member->next;
                if (reserved(member->key) || member->type != JsonType::Object) continue;
                if (MoonModule* c = childNamed(m, member->key)) {
                    if (m->moveChildTo(c, index)) structural = true;
                    index++;
                }
            }
        }
        return true;
    }
};

// A preset file's pad, the one root key that is not a module, so a preset file applies as it is through any path.
constexpr const char* kSlotKey = "slot";

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
    if (!json::parse(text, doc)) { r.ok = false; r.error = "malformed JSON"; return r; }
    const JsonNode* root = doc.rootNode();
    if (root->type != JsonType::Object) { r.ok = false; r.error = "a document is a JSON object"; return r; }

    Applier a{scheduler, doc, r};
    // Two passes over the same members: every removal first, then everything else.
    for (int pass = 0; pass < 2 && r.ok; pass++) {
        for (int i = root->firstChild; i >= 0;) {
            const JsonNode* member = doc.node(i);
            if (!member) break;
            i = member->next;
            if (std::strcmp(member->key, kSlotKey) == 0) continue;
            const size_t at = a.push(member->key);
            MoonModule* top = nullptr;
            for (uint8_t m = 0; m < scheduler.moduleCount(); m++) {
                MoonModule* candidate = scheduler.module(m);
                if (candidate && std::strcmp(candidate->name(), member->key) == 0) { top = candidate; break; }
            }
            // The top level is the fixed set main.cpp wires, so a document only reaches into it.
            if (!top) { a.fail("no such top-level module"); break; }
            if (member->type != JsonType::Object) { a.fail("a top-level module takes an object"); break; }
            if (!(pass == 0 ? a.prune(top, member) : a.apply(top, member))) break;
            a.pop(at);
        }
    }
    // A failed document leaves nothing waiting for the rebuild: what came before the failure stays applied, but no deferred write lands after it.
    if (!r.ok) {
        for (uint8_t i = deferredBefore; i < g_deferredCount; i++) platform::free(g_deferred[i].valueJson);
        g_deferredCount = deferredBefore;
        r.deferred = 0;
    }
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
}

}  // namespace mm

/// @}
