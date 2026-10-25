/// HttpServerModule's state wire: what each client subscribes to, the full state written a fragment at a time, and the patches of what changed since.

#include "core/system/HttpServerModule.h"

#include "core/module/Scheduler.h"
#include "core/system/DevicesModule.h"   // nameOf, addressOf: the boards a hosts text names
#include "core/util/HostList.h"         // parseHostList: a hosts text read as its service reads it
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"           // a subscribe message parsed
#include "core/util/fnv.h"               // fnv1a: the diff-on-the-wire cache's digest
#include "platform/platform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <new>       // placement new: a send's source built in its heap block

namespace mm {


// Static sink for the schema-changed hook: routes a module's rebuildControls() to the live instance's resync flag, like FilesystemModule::noteDirty.
void HttpServerModule::onSchemaChanged() {
    if (instance_) instance_->requestFullResync();
}

void HttpServerModule::onValuesChanged(MoonModule* mod) {
    if (!instance_ || !mod || !mod->name()) return;
    HttpServerModule& self = *instance_;
    for (uint8_t i = 0; i < self.soonCount_; i++)
        if (std::strcmp(self.soon_[i], mod->name()) == 0) return;
    // A full list leaves the rest to the periodic patch, which carries every change anyway.
    if (self.soonCount_ >= kSoonModules) return;
    mm::formatTo(self.soon_[self.soonCount_++], sizeof(self.soon_[0]), "%s", mod->name());
}

// The state answers a fragment at a time from a send slot, so a large tree waits on no client's ACKs.
void HttpServerModule::serveState(platform::TcpConnection& conn) {
    Send* slot = claimSend(conn);
    if (!slot) return;
    // The reader's side is known now, while the request is routed, and every fragment is written as it would have been.
    if (!startTree(*slot, nullptr, false, SecretsHidden::active(), 0)) {
        sendResponse(conn, 507, "application/json", "{\"error\":\"out of memory\"}");
        return;
    }
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    if (!conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header))) { endSendSlot(*slot); return; }
    slot->conn = std::move(conn);
}

void HttpServerModule::buildStateJson(JsonSink& sink) {
    TreeCursor c;
    writeTreeFragment(sink, c, 0);
}

// The diff-on-the-wire cache keeps an 8-byte FNV-1a {path,value} hash per leaf, not the value; a collision skips one update at worst.

bool HttpServerModule::Subscription::names_(const char* s) const {
    if (!s || !s[0]) return false;
    const uint32_t h = fnv1a(s, std::strlen(s));
    for (uint8_t i = 0; i < count; i++)
        if (names[i] == h) return true;
    return false;
}

bool HttpServerModule::Subscription::covers(const MoonModule* mod) const {
    if (all) return true;
    for (const MoonModule* m = mod; m && count; m = m->parent())
        if (names_(m->name()) || names_(m->typeName())) return true;
    return false;
}

bool HttpServerModule::Subscription::parse(const char* body) {
    json::JsonDoc* doc = nullptr;
    const json::JsonNode* arr = json::parseListArray(body, "subscribe", doc);
    if (!arr) return false;
    *this = Subscription{};
    received = true;
    const int n = json::arraySize(*doc, arr);
    for (int i = 0; i < n; i++) {
        const json::JsonNode* el = json::element(*doc, arr, i);
        if (!el || el->type != json::JsonType::String || !el->str || !el->str[0]) continue;
        if (std::strcmp(el->str, "*") == 0) all = true;
        else if (count < kMaxModules) names[count++] = fnv1a(el->str, std::strlen(el->str));
    }
    return true;
}

bool HttpServerModule::Subscription::operator==(const Subscription& o) const {
    if (received != o.received || all != o.all || count != o.count) return false;
    for (uint8_t i = 0; i < count; i++)
        if (names[i] != o.names[i]) return false;
    return true;
}

bool HttpServerModule::anyClientCovers(const MoonModule* mod) const {
    // Every slot's subscription, live or not: the uplink poll clears a slot's subscription once its client is gone.
    for (const auto& sub : subs_)
        if (sub.covers(mod)) return true;
    return false;
}

// The diff-on-the-wire core: visit every UI leaf a client subscribes to, in buildStateJson order, as fn(module, pathHash, valueHash, path, valueSink); names are unique tree-wide, so "<module>/<name>" is stable.
template <class Fn>
void HttpServerModule::forEachStateLeaf(Fn&& fn, bool everything) {
    if (!scheduler_) return;
    // A module nobody views can still hold one somebody does, the preview inside Drivers, so every module is visited and only the covered ones yield leaves.
    forEachStateModule([&](MoonModule* mod, uint16_t) {
        uint16_t from = 0;
        // `fn`, not std::forward: forwarding inside a loop moves the callable on the first module, leaving later modules a moved-from object.
        if (everything || anyClientCovers(mod)) visitOwnLeaves(mod, fn, from, [] { return false; });
        return true;
    });
}

template <class Fn>
void HttpServerModule::forEachStateModule(Fn&& fn) {
    if (!scheduler_) return;
    uint16_t k = 0;
    for (uint8_t m = 0; m < scheduler_->moduleCount(); m++)
        if (auto* mod = scheduler_->module(m))
            if (mod->appearsInUi() && !visitTree(mod, k, fn)) return;
}

template <class Fn>
bool HttpServerModule::visitTree(MoonModule* mod, uint16_t& k, Fn& fn) {
    if (!fn(mod, k++)) return false;
    for (uint8_t i = 0; i < mod->childCount(); i++)
        if (auto* ch = mod->child(i))
            if (!visitTree(ch, k, fn)) return false;
    return true;
}

template <class Fn, class Stop>
bool HttpServerModule::visitOwnLeaves(MoonModule* mod, Fn&& fn, uint16_t& next, Stop&& stop) {
    char path[80];
    // The walk runs every second, so a value goes into the module's own small buffer. Only one that outgrows it, a list or a long text, goes into a heap sink. A heap block per leaf fragments a small heap.
    auto leaf = [&](auto&& write) {
        JsonSink vs(leafValue_, sizeof(leafValue_));
        write(vs);
        if (!vs.overflowed()) { fn(mod, fnv1a(path, std::strlen(path)), fnv1a(vs.data(), vs.size()), path, vs); return; }
        JsonSink big;
        write(big);
        fn(mod, fnv1a(path, std::strlen(path)), fnv1a(big.data(), big.size()), path, big);
    };
    static const char* sevStr[] = {"status", "warning", "error"};
    auto& ctrls = mod->controls();
    for (; next < kHeaderLeaves + ctrls.count(); next++) {
        if (stop()) return false;
        switch (next) {
            // Header telemetry the UI shows per card, `@`-prefixed so it cannot collide with a control name; only fields that change per tick (timing, memory) ride the patch.
            case 0:
                std::snprintf(path, sizeof(path), "%s/@tickTimeUs", mod->name());
                leaf([&](JsonSink& s) { s.appendf("%u", static_cast<unsigned>(mod->tickTimeUs())); });
                break;
            case 1:
                std::snprintf(path, sizeof(path), "%s/@dynamicBytes", mod->name());
                leaf([&](JsonSink& s) { s.appendf("%u", static_cast<unsigned>(mod->dynamicBytes())); });
                break;
            // Status and severity ride the patch too, since a driver can fault at any moment and only the diff push runs every second; an unchanged value costs nothing.
            case 2:
                std::snprintf(path, sizeof(path), "%s/@status", mod->name());
                // writeJsonString already emits the quotes and escapes, so add none: manual quotes make invalid JSON that the browser rejects as a whole patch frame.
                leaf([&](JsonSink& s) { s.writeJsonString(mod->status() ? mod->status() : ""); });
                break;
            case 3:
                std::snprintf(path, sizeof(path), "%s/@severity", mod->name());
                leaf([&](JsonSink& s) { s.appendf("\"%s\"", sevStr[static_cast<int>(mod->severity())]); });
                break;
            default: {   // each control's value
                auto& c = ctrls[static_cast<uint8_t>(next - kHeaderLeaves)];
                std::snprintf(path, sizeof(path), "%s/%s", mod->name(), c.name);
                leaf([&](JsonSink& s) {
                    if (c.type == ControlType::Password) writeObfuscatedPassword(s, static_cast<const char*>(c.ptr));   // as the full state shows it
                    else writeControlValue(s, c);
                });
            }
        }
    }
    return true;
}

HttpServerModule::LeafHash* HttpServerModule::findLeaf(uint32_t pathHash, uint16_t& next) {
    // A walk visits the leaves in the baseline's order, so the expected entry is the one after the last match. That is one compare per leaf rather than a scan of the cache.
    if (next < leafHashCount_ && leafHashes_[next].path == pathHash) return &leafHashes_[next++];
    for (uint16_t i = 0; i < leafHashCount_; i++)
        if (leafHashes_[i].path == pathHash) { next = i + 1; return &leafHashes_[i]; }
    return nullptr;
}

void HttpServerModule::baselineLeafHashes(bool everything) {
    // Counted from the schema, each covered module's header leaves and controls, so the one walk that writes and hashes the values is the fill. Resize does not preserve, so it sizes first.
    uint16_t n = 0, modules = 0;
    forEachStateModule([&](MoonModule* mod, uint16_t) {
        if (everything || anyClientCovers(mod)) {
            n += kHeaderLeaves + mod->controls().count();
            modules++;
        }
        return true;
    });
    leafHashes_.resize(n);
    leafHashCount_ = 0;
    forEachStateLeaf([&](const MoonModule*, uint32_t ph, uint32_t vh, const char*, JsonSink&) {
        if (leafHashCount_ < leafHashes_.count()) leafHashes_[leafHashCount_++] = {ph, vh};
    }, everything);
    if (patchSlices_.count() < modules) patchSlices_.resize(modules);
}

uint16_t HttpServerModule::buildStatePatch(JsonSink& sink) {
    PatchBuild b;
    collectPatch(b, [&](auto&& fn) { forEachStateLeaf(fn, true); });
    return writeClientPatch(sink, b, nullptr);
}

void HttpServerModule::buildClientPatchesForTest(JsonSink* outs, int count) {
    PatchBuild b;
    collectPatch(b, [&](auto&& fn) { forEachStateLeaf(fn); });
    for (int i = 0; i < count && i < kMaxWsClients; i++) writeClientPatch(outs[i], b, &subs_[i]);
}

template <class Fn>
void HttpServerModule::forEachSoonLeaf(Fn&& fn, bool everything) {
    auto own = [&](MoonModule* mod, uint16_t) {
        uint16_t from = 0;
        if (everything || anyClientCovers(mod)) visitOwnLeaves(mod, fn, from, [] { return false; });
        return true;
    };
    for (uint8_t i = 0; i < soonCount_; i++)
        // No appearsInUi test: the interface drops a patch for a module it never saw.
        if (MoonModule* mod = scheduler_ ? scheduler_->firstByName(soon_[i]) : nullptr) {
            uint16_t k = 0;
            visitTree(mod, k, own);
        }
    soonCount_ = 0;
}

uint16_t HttpServerModule::buildSoonPatch(JsonSink& sink) {
    PatchBuild b;
    collectPatch(b, [&](auto&& fn) { forEachSoonLeaf(fn, true); });
    return writeClientPatch(sink, b, nullptr);
}

void HttpServerModule::pushSoonPatch() {
    // A small frame written into a full state mid-stream would land inside that message, so it waits.
    if (fullResyncPending_ || anyClientResync() || anyStreamActive()) return;
    const uint32_t now = platform::millis();
    if (now - soonSentMs_ < kSoonMs) return;
    soonSentMs_ = now;
    // What the second's walk gathered goes first, so an older value it holds never lands after this newer one.
    if (walk_.active) sendPatch(walkPatch_);
    // The leaves' hashes update as they go, so the second's patch does not send them again.
    sendPatches([&](auto&& fn) { forEachSoonLeaf(fn); });
}

template <class Walk>
void HttpServerModule::collectPatch(PatchBuild& b, Walk&& walk) {
    walk([&](const MoonModule* mod, uint32_t ph, uint32_t vh, const char* path, JsonSink& vs) {
        LeafHash* h = findLeaf(ph, b.next);
        if (h && h->value == vh) return;              // unchanged: the common case, emit nothing
        const bool opens = !b.slices || patchSlices_[b.slices - 1].mod != mod;
        // No room for the module's slice leaves the hash as it was, so the change goes out once there is.
        if (opens && b.slices >= patchSlices_.count()) return;
        if (h) h->value = vh;                          // known leaf, value changed → update cache
        // A leaf missing from the baseline cannot happen, since every structural mutation re-baselines first; emit it without growing the cache, because ScratchBuffer::resize is non-preserving and would discard every hash.
        if (opens) patchSlices_[b.slices++] = {mod, static_cast<uint32_t>(b.entries.size()), 0, 0};
        b.entries.append(",{\"path\":\"");
        b.entries.append(path);
        b.entries.append("\",\"value\":");
        b.entries.append(vs.data(), vs.size());        // the already-serialized value
        b.entries.append("}");
        PatchSlice& s = patchSlices_[b.slices - 1];
        s.end = static_cast<uint32_t>(b.entries.size());
        s.count++;
        b.changed++;
    });
}

uint16_t HttpServerModule::writeClientPatch(JsonSink& out, const PatchBuild& b, const Subscription* sub) {
    out.append("{\"patch\":[");
    uint16_t n = 0;
    for (uint16_t i = 0; i < b.slices; i++) {
        const PatchSlice& s = patchSlices_[i];
        if (sub && !sub->covers(s.mod)) continue;
        // Every entry leads with its comma, which the first one of the frame drops.
        const size_t skip = n == 0 ? 1 : 0;
        out.append(b.entries.data() + s.begin + skip, s.end - s.begin - skip);
        n += s.count;
    }
    out.append("]}");
    return n;
}

void HttpServerModule::sendPatch(PatchBuild& b, bool skipResyncing) {
    if (b.changed == 0) return;
    for (int i = 0; i < kMaxWsClients; i++) {
        auto& ws = wsClients_[i];
        if (!ws.valid() || (skipResyncing && resyncClient_[i])) continue;
        // A patch cut short by memory is not valid JSON, and its leaves' hashes already moved on, so the client takes its whole state again instead.
        if (b.entries.overflowed()) { resyncClient_[i] = true; continue; }
        JsonSink out;
        if (writeClientPatch(out, b, &subs_[i]) == 0) continue;
        if (out.overflowed()) { resyncClient_[i] = true; continue; }
        queueFrame(i, 0x81, out.data(), out.size());
    }
    // The slices are shared, so they are free for the next walk once sent; `next` stays, the walk carrying on where it was.
    b.entries.clear();
    b.slices = 0;
    b.changed = 0;
}

template <class Walk>
void HttpServerModule::sendPatches(Walk&& walk, bool skipResyncing) {
    PatchBuild b;
    collectPatch(b, walk);
    sendPatch(b, skipResyncing);
}

void HttpServerModule::advanceValueWalk() {
    if (!walk_.active || anyStreamActive()) return;
    const SecretsHidden hide(anyWsClientOnAccessPoint());
    const uint32_t start = platform::micros();
    if (stepValueWalk([&] { return platform::micros() - start >= kWalkSliceUs; })) {
        sendPatch(walkPatch_);
        walk_.active = false;
    }
}

void HttpServerModule::startValueWalk() {
    walk_ = {true, 0, 0};
    walkPatch_.next = 0;
}

void HttpServerModule::endValueWalk() {
    walk_.active = false;
    walkPatch_.entries.clear();
    walkPatch_.slices = 0;
    walkPatch_.changed = 0;
}

template <class Stop>
bool HttpServerModule::stepValueWalk(Stop&& stop) {
    // The first leaf goes whatever the budget, so every call moves the walk on.
    bool first = true;
    auto halt = [&] {
        if (first) { first = false; return false; }
        return stop();
    };
    bool done = true;
    collectPatch(walkPatch_, [&](auto&& fn) {
        forEachStateModule([&](MoonModule* mod, uint16_t k) {
            if (k < walk_.module) return true;    // walked in an earlier tick
            walk_.module = k;
            if (anyClientCovers(mod) && !visitOwnLeaves(mod, fn, walk_.leaf, halt)) { done = false; return false; }
            walk_.leaf = 0;
            return true;
        });
    });
    return done;
}

bool HttpServerModule::stepValueWalkForTest(int leaves) {
    return stepValueWalk([&] { return --leaves <= 0; });
}

void HttpServerModule::writeModuleHeader(JsonSink& sink, MoonModule* mod) {
    // Per-module header: name, role, enabled, tickTimeUs (fps/ms display), classSize (static C++ object bytes) + dynamicBytes (heap)
    const char* roleStr = roleName(mod->role());
    const char* type = mod->typeName();
    if (!type) type = "";
    // `enabled` is published for every module, and the ones that keep running regardless say so here, so the UI can leave out a switch that would do nothing.
    sink.appendf(
        "{\"name\":\"%s\",\"type\":\"%s\",\"role\":\"%s\",\"enabled\":%s,"
        "\"respectsEnabled\":%s,"
        "\"tickTimeUs\":%u,\"classSize\":%u,\"dynamicBytes\":%u",
        mod->name() ? mod->name() : "",
        type,
        roleStr,
        mod->enabledSetting() ? "true" : "false",
        mod->respectsEnabled() ? "true" : "false",
        static_cast<unsigned>(mod->tickTimeUs()),
        static_cast<unsigned>(mod->classSize()),
        static_cast<unsigned>(mod->dynamicBytes()));
    // The flag as set, and whether safe mode holds the module anyway, so the UI shows why its switch does nothing.
    if (mod->held()) sink.append(",\"held\":true");
    // The instance's own tags (a scripted module's come from its script), since /api/types answers per type; emitted only when non-empty.
    if (const char* tg = mod->tags(); tg && tg[0]) {
        sink.append(",\"tags\":");
        sink.writeJsonString(tg);
    }
    writeStatus(sink, mod);
    // userEditable is omitted when true (the UI treats absent as editable); modules that opt out, like PreviewDriver, hide their delete/replace affordance.
    if (!mod->userEditable()) sink.append(",\"userEditable\":false");
}

void HttpServerModule::writeModuleJson(JsonSink& sink, MoonModule* mod) {
    TreeCursor c;
    c.single = mod;
    writeTreeFragment(sink, c, 0);
}

MoonModule* HttpServerModule::moduleAt(const TreeCursor& c) const {
    MoonModule* mod = c.single ? (c.idx[0] == 0 ? c.single : nullptr)
                               : (scheduler_ ? scheduler_->module(c.idx[0]) : nullptr);
    for (uint8_t l = 1; mod && l < c.depth; l++)
        mod = c.idx[l] < mod->childCount() ? mod->child(c.idx[l]) : nullptr;
    return mod;
}

bool HttpServerModule::writeTreeFragment(JsonSink& sink, TreeCursor& c, size_t target) {
    if (c.done) return true;
    c.pieces = target != 0;
    if (c.depth == 0) {
        if (!c.single) sink.append("{\"modules\":[");
        c.depth = 1;
        c.idx[0] = 0;
        c.phase = TreeCursor::kHeader;
    }
    // The path is resolved again at every step rather than held as pointers, so a tree changed between fragments still ends in valid JSON.
    while (target == 0 || sink.size() < target) {
        if (MoonModule* mod = moduleAt(c)) stepInto(sink, c, mod);
        else if (stepPast(sink, c)) return true;
    }
    return false;
}

bool HttpServerModule::stepPast(JsonSink& sink, TreeCursor& c) {
    const uint8_t level = c.depth - 1;
    if (c.phase != TreeCursor::kHeader) {
        // The module this level was writing went mid-object: close what it opened, as if it ended there, and go on with its next sibling.
        if (c.ctrlName) closeOpenControl(sink, c);
        sink.append(c.phase == TreeCursor::kControls ? "]}" : "}");
        c.idx[level]++;
        c.phase = TreeCursor::kHeader;
        return false;
    }
    if (level > 0) {
        sink.append("]}");   // the parent's children and the parent itself
        c.depth--;
        c.idx[c.depth - 1]++;
        return false;
    }
    if (!c.single) {
        sink.append("]");
        // The id of the request the streamed state answers, so the page knows its view arrived rather than an earlier one.
        if (c.answers) sink.appendf(",\"resync\":%lu", static_cast<unsigned long>(c.answers));
        sink.append("}");
    }
    c.done = true;
    return true;
}

void HttpServerModule::stepInto(JsonSink& sink, TreeCursor& c, MoonModule* mod) {
    const uint8_t level = c.depth - 1;
    if (c.phase == TreeCursor::kHeader) {
        // HttpServerModule and FilesystemModule are no cards: appearsInUi is the one mechanism for that.
        if (level == 0 && !c.single && !mod->appearsInUi()) { c.idx[0]++; return; }
        if (level == 0 ? c.wroteRoot : c.idx[level] > 0) sink.append(",");
        if (level == 0) c.wroteRoot = true;
        writeModuleHeader(sink, mod);
        // An outline leaves the controls out: the module's name and shape are what a client viewing elsewhere reads.
        const bool full = !c.sub || c.sub->covers(mod);
        if (full) sink.append(",\"controls\":[");
        c.ctrl = 0;
        c.phase = full ? TreeCursor::kControls : TreeCursor::kChildren;
    } else if (c.phase == TreeCursor::kControls) {
        const bool here = c.ctrl < mod->controls().count();
        if (c.ctrlName && (!here || mod->controls()[c.ctrl].name != c.ctrlName)) {
            closeOpenControl(sink, c);   // it changed between fragments: ended as it stood, valid JSON
            return;
        }
        if (here) {
            const ControlDescriptor& d = mod->controls()[c.ctrl];
            // A fragment-sized document cuts a large list or option set at a row or an option, so no fragment needs a large block.
            if (c.pieces && writtenInPieces(d)) { writeControlPieceOf(sink, c, d); return; }
            if (c.ctrl > 0) sink.append(",");
            writeControl(sink, d);
            c.ctrl++;
            return;
        }
        sink.append("]");
        c.phase = TreeCursor::kChildren;
    } else if (mod->childCount() > 0 && c.depth < TreeCursor::kMaxDepth) {
        sink.append(",\"children\":[");
        c.idx[c.depth++] = 0;
        c.phase = TreeCursor::kHeader;
    } else {
        sink.append("}");
        c.idx[level]++;
        c.phase = TreeCursor::kHeader;
    }
}

void HttpServerModule::writeStatus(JsonSink& sink, MoonModule* mod) {
    // Only emit when the module has a status; severity strings are stable wire format ("status", "warning", "error"), documented in HttpServerModule.md.
    const char* s = mod->status();
    if (!s) return;
    static const char* sevStr[] = {"status", "warning", "error"};
    // writeJsonString emits its own quotes and escapes a `"` or `\` that a raw %s would turn into invalid JSON; severity is a fixed vocabulary and stays a plain %s.
    sink.append(",\"status\":");
    sink.writeJsonString(s);
    sink.appendf(",\"severity\":\"%s\"", sevStr[static_cast<int>(mod->severity())]);
}

void HttpServerModule::writeControl(JsonSink& sink, const ControlDescriptor& c) {
    // Common wrapper for every control; per-type VALUE + EXTRAS live in Control.cpp, but Password is handled here since its API form is XOR-obfuscated + base64 and writeControlValue stays sink-neutral.
    sink.appendf("{\"name\":\"%s\",\"type\":\"%s\",\"value\":",
                 c.name, controlTypeName(c.type));
    if (c.type == ControlType::Password) {
        // Not readable at a glance in `curl /api/state`, and the UI's hold-to-peek reverses it.
        writeObfuscatedPassword(sink, static_cast<char*>(c.ptr));
    } else {
        writeControlValue(sink, c);
    }
    writeControlMetadata(sink, c);
    writeControlTail(sink, c);
}

void HttpServerModule::writeControlPieceOf(JsonSink& sink, TreeCursor& c, const ControlDescriptor& d) {
    if (!c.ctrlName) {
        if (c.ctrl > 0) sink.append(",");
        sink.appendf("{\"name\":\"%s\",\"type\":\"%s\",\"value\":", d.name, controlTypeName(d.type));
        c.ctrlName = d.name;
        c.at = {};
        return;
    }
    if (writeControlPiece(sink, d, c.at)) return;
    writeControlTail(sink, d);
    c.ctrlName = nullptr;
    c.ctrl++;
}

// Each address the device list knows with its board's name, and each board's name with its address, read as the service reads its list; nothing for a list it refuses.
void HttpServerModule::writeKnownHosts(JsonSink& sink, const char* text) {
    const DevicesModule* devices = DevicesModule::active();
    if (!devices || !text) return;
    Host hosts[32];
    uint8_t n = 0;
    if (parseHostList(text, hosts, 32, n)) return;
    bool any = false;
    for (uint8_t i = 0; i < n; i++) {
        char name[kMaxHostName + 1], dotted[16];
        const char* entry = dotted;
        const char* counterpart = name;
        if (hosts[i].isName()) {
            hostName(text, hosts[i], name);
            uint8_t ip[4];
            if (!devices->addressOf(name, ip)) continue;
            formatDottedQuad(dotted, ip);
            entry = name;
            counterpart = dotted;
        } else {
            counterpart = devices->nameOf(hosts[i].ip);
            if (!counterpart) continue;
            formatDottedQuad(dotted, hosts[i].ip);
        }
        sink.append(any ? "," : ",\"known\":{");
        sink.writeJsonString(entry);
        sink.append(":");
        sink.writeJsonString(counterpart);
        any = true;
    }
    if (any) sink.append("}");
}

// Before its first piece only the head is out, waiting for a value; after it, one array is open inside the control.
void HttpServerModule::closeOpenControl(JsonSink& sink, TreeCursor& c) {
    sink.append(c.at.part == 0 ? "null}" : "]}");
    c.ctrlName = nullptr;
    c.ctrl++;
}

void HttpServerModule::writeControlTail(JsonSink& sink, const ControlDescriptor& c) {
    // Emit optional flags only when set (common case is false; omit to save bytes).
    if (c.readonly) sink.append(",\"readonly\":true");
    if (c.minMode) sink.appendf(",\"minMode\":%u", static_cast<unsigned>(c.minMode));   // the mode the UI needs before it shows this
    if (c.numberField) sink.append(",\"numberField\":true");   // render a plain number input, not a slider
    if (c.hex) sink.append(",\"hex\":true");   // that number input reads and writes hexadecimal
    if (c.hosts) writeKnownHosts(sink, static_cast<const char*>(c.ptr));
    if (c.switchRow) sink.append(",\"switchRow\":true");
    if (c.displayStrip) sink.append(",\"displayStrip\":true");
    // The target rides with all three surface kinds, since a switch drives something too (switch1 is the global on/off) and its popup should say so.
    if (c.fader || c.encoder || c.switchRow) {
        if (c.fader)        sink.append(",\"fader\":true");
        else if (c.encoder) sink.append(",\"encoder\":true");
        if (c.surfaceTarget) { sink.append(",\"target\":"); sink.writeJsonString(c.surfaceTarget); }
    }
    // An editable List shows add/delete/reorder and inline row editors; its rows carry a stable id and field descriptors.
    if (c.type == ControlType::List) {
        const auto* ls = static_cast<const ListSource*>(c.ptr);
        if (ls && ls->isEditableList()) sink.append(",\"editable\":true");
        if (ls && ls->listRowsFixed()) sink.append(",\"fixedRows\":true");
        if (ls && ls->listAsPads()) {
            sink.append(",\"pads\":true");
            const uint8_t gc = ls->listGridCols(), gr = ls->listGridRows();
            if (gc && gr) sink.appendf(",\"gridCols\":%u,\"gridRows\":%u",
                                       static_cast<unsigned>(gc), static_cast<unsigned>(gr));
        }
    }
    sink.append(c.hidden ? ",\"hidden\":true}" : "}");
}

void HttpServerModule::pushStateToWebSockets() {
    bool hasClients = false;
    for (auto& ws : wsClients_) {
        if (ws.valid()) { hasClients = true; break; }
    }
    if (!hasClients) { endValueWalk(); return; }

    // Nothing may land inside a streamed document, so every push waits while one is open; a resync requested meanwhile starts once they end.
    if (anyStreamActive()) return;
    if (fullResyncPending_ || anyClientResync()) {
        // A resync of some clients first patches the rest up to now, since the baseline taken next forgets what they have not been sent. The second's walk finishes in this frame, a resync being rare.
        if (!fullResyncPending_) {
            if (!walk_.active) startValueWalk();
            stepValueWalk([] { return false; });
            sendPatch(walkPatch_, /*skipResyncing=*/true);
        }
        endValueWalk();
        baselineLeafHashes();       // the full state IS the new baseline: next tick patches from here
        startStreams();
        return;   // the WLED frame waits for the streams too
    }
    // PATCH, the steady state: the walk starts here and moves on a slice per transport tick. Each client gets its share of the changed leaves once it ends, and a walk still under way carries on.
    if (!walk_.active) startValueWalk();

    // Push a WLED-shaped {state, info} frame for the WLED app on this /ws; each consumer ignores the other's frame (the UI keys on `modules`, the app on `state`/`info`).
    pushWledStateToWebSockets();
}

// Each client due a document gets a cursor that streams it on tick20ms, a fragment at a time, so no board needs the whole tree in one block.
void HttpServerModule::startStreams() {
    for (int i = 0; i < kMaxWsClients; i++) {
        const bool wanted = fullResyncPending_ || resyncClient_[i];
        resyncClient_[i] = false;
        if (!wanted || !wsClients_[i].valid() || !subs_[i].received) continue;   // the WLED app never subscribes, and reads its own frame
        // Frames still leaving go first: the document starts at the next frame boundary.
        if (!startTree(controlSends_[i], &subs_[i], true, wsOnAccessPoint_[i], resyncId_[i])) {
            refuseFragment();
            wsClients_[i].close();
        }
        resyncId_[i] = 0;
    }
    fullResyncPending_ = false;
}

void HttpServerModule::applySubscription(int client, const char* body) {
    if (client < 0 || client >= kMaxWsClients) return;
    Subscription next;
    if (!next.parse(body) || next == subs_[client]) return;
    subs_[client] = next;
    resyncClient_[client] = true;   // this client alone: the push brings the others up to date before it re-baselines
}

bool HttpServerModule::anyClientResync() const MM_NONBLOCKING {
    for (bool r : resyncClient_)
        if (r) return true;
    return false;
}

bool HttpServerModule::anyStreamActive() const {
    for (const auto& cs : controlSends_)
        if (cs.source == Send::Source::Tree) return true;
    return false;
}

} // namespace mm
