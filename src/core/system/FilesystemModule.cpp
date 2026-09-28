/// @defgroup filesystem_impl Filesystem implementation
/// The persistence engine: writes control values to `/.config/*.json` and restores them on boot.
/// Public surface and class layout live in FilesystemModule.h.
/// @{
#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/FilesystemModule.h"

#include "core/module/Control.h"
#include "core/util/JsonSink.h"   // fixed-buffer mode used by writeValue()
#include "core/util/JsonUtil.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "platform/platform.h"

#include <climits>  // INT16_MIN/MAX in applyValue's Int16 clamp
#include <cstdio>
#include <cstring>

namespace mm {

FilesystemModule::~FilesystemModule() {
    if (instance_ == this) instance_ = nullptr;
}

void FilesystemModule::setScheduler(Scheduler* s) {
    scheduler_ = s;
    instance_ = this;
    if (s) {
        s->setLoadAllHook(&loadAllHookTrampoline_);
        s->setReapplyValuesHook(&reapplyValuesHookTrampoline_);
        // Scheduler::setControl calls this after a mutation so a control set from anywhere (IR, WLED bridge, /api/control) schedules the same debounced save. noteDirty is a static. A plain function pointer suffices, no trampoline needed.
        s->setNoteDirtyHook(&FilesystemModule::noteDirty);
    }
}

void FilesystemModule::setup() {
    // Both failures below name the directory, because the useful question when settings do not persist is always "which location did it try".
    // Reported ONCE here rather than as a write error per save.
    // An unusable root produces one failed save per module per change, and that stream buries the one fact that explains it.
    if (!platform::fsMount()) {
        std::printf("FilesystemModule: cannot use %s, persistence disabled\n",
                    platform::fsRootPath());
        return;
    }
    if (!platform::fsMkdir(CONFIG_DIR)) {
        std::printf("FilesystemModule: cannot create %s%s, persistence disabled\n",
                    platform::fsRootPath(), CONFIG_DIR);
        return;
    }
    mounted_ = true;
    std::printf("FilesystemModule: mounted, %zu / %zu bytes used\n",
                platform::filesystemUsed(), platform::filesystemTotal());
}

// FilesystemModule is a non-UI persistence engine: it holds no controls (hence no defineControls override), so it renders no card in the module tree, a card here would confuse an end user next to the File Manager.
// Its one piece of status, "last saved", is displayed by FileManagerModule, which reads it via FilesystemModule::instance()->lastSavedStr().
// The filesystem-usage gauge likewise lives on FileManagerModule (that's where filesystem state is topical).

void FilesystemModule::tick1s() MM_NONBLOCKING {
    if (!mounted_ || !scheduler_) return;
    updateLastSavedStr();
    if (!dirtyPending_) return;
    const uint32_t now = platform::millis();
    // Two conditions, either of which saves.
    // The DEBOUNCE waits for quiet, which coalesces a burst of edits into one write.
    // The CEILING bounds how long that wait may last, because a continuous writer never goes quiet.
    // Without it a control driven at 50 Hz re-stamped the debounce forever and nothing in that module's file was ever saved, including settings a person had chosen.
    if (now - lastDirtyMs_ < DEBOUNCE_MS && now - firstDirtyMs_ < MAX_DEFER_MS) return;
    flush();
}

// Refresh the "lastSaved" display string, "never" before the first save, otherwise how long ago the last successful write happened.
void FilesystemModule::updateLastSavedStr() {
    if (!everSaved_) {
        mm::formatTo(lastSaveStr_, sizeof(lastSaveStr_), "never");
        return;
    }
    uint32_t agoSec = (platform::millis() - lastSaveMs_) / 1000;
    if (agoSec < 60) {
        mm::formatTo(lastSaveStr_, sizeof(lastSaveStr_), "%us ago",
                      static_cast<unsigned>(agoSec));
    } else if (agoSec < 3600) {
        mm::formatTo(lastSaveStr_, sizeof(lastSaveStr_), "%um ago",
                      static_cast<unsigned>(agoSec / 60));
    } else {
        mm::formatTo(lastSaveStr_, sizeof(lastSaveStr_), "%uh ago",
                      static_cast<unsigned>(agoSec / 3600));
    }
}

void FilesystemModule::flush() {
    if (!mounted_ || !scheduler_) return;
    bool allSaved = true;
    for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
        MoonModule* m = scheduler_->module(i);
        if (!m || m == this) continue;
        if (subtreeDirty(m)) {
            // Only clear the dirty flag when the write actually succeeded, otherwise a failed write would silently drop the pending change.
            if (saveSubtree(m)) {
                clearSubtreeDirty(m);
                lastSaveMs_ = platform::millis();
                everSaved_ = true;
            } else {
                allSaved = false;
            }
        }
    }
    // Keep dirtyPending_ set if anything failed, so tick1s retries.
    dirtyPending_ = !allSaved;
}

void FilesystemModule::flushPending() {
    if (instance_) instance_->flush();
}

void FilesystemModule::noteDirty() {
    if (!instance_) return;
    const uint32_t now = platform::millis();
    // The FIRST mark of a pending save starts the ceiling clock; later marks only move the debounce. Stamping both on every mark is what let a continuous writer defer the save forever.
    if (!instance_->dirtyPending_) instance_->firstDirtyMs_ = now;
    instance_->lastDirtyMs_ = now;
    instance_->dirtyPending_ = true;
}

// ---- Scheduler hook trampoline (C-style for typedef compatibility) ----
void FilesystemModule::loadAllHookTrampoline_(Scheduler* s) {
    if (instance_) instance_->loadAll(s);
}

void FilesystemModule::reapplyValuesHookTrampoline_(Scheduler* s) {
    if (instance_) instance_->reapplyValues(s);
}

void FilesystemModule::loadAll(Scheduler* s) {
    if (!mounted_) {
        // setup() hasn't run yet (we're in phase 2, before phase 3 setup). Mount now so we can read; setup() later calls fsMount again (idempotent).
        if (!platform::fsMount()) return;
        if (!platform::fsMkdir(CONFIG_DIR)) return;   // setup() reports it; stay unmounted
        mounted_ = true;
    }
    for (uint8_t i = 0; i < s->moduleCount(); i++) {
        MoonModule* m = s->module(i);
        if (!m || m == this) continue;
        loadSubtree(m);
    }
}

// Re-apply saved VALUES after the tree has been prepared, for a module whose control set is not final until then.
// `applyNode`'s two-pass overlay covers a schema that depends on a control VALUE (ParallelLedDriver's `peripheral` swapping the backend-owned controls), because rebuildControls() alone re-derives it.
// It cannot cover a schema that depends on WORK: a MoonLive script's declared controls exist only once the script has COMPILED, which is prepare()'s job and runs after load.
// So at load time `cols`/`rows` are not in the list, overlayControls skips them, and prepare() then seeds them from the script's own defaults: the saved values are read and dropped.
//
// Values only, and no tree reconciliation: the shape was settled by the first pass, so this pass must not add, remove or re-enable anything.
// Cold path, once per boot, and it re-reads rather than holding every node's JSON until prepare() (memory on every module for a case that is three).
void FilesystemModule::reapplyValues(Scheduler* s) {
    if (!mounted_ || !s) return;
    for (uint8_t i = 0; i < s->moduleCount(); i++) {
        MoonModule* m = s->module(i);
        if (!m || m == this) continue;
        reapplySubtree(m);
    }
}

void FilesystemModule::reapplySubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return;
    const long size = platform::fsSize(path);
    if (size <= 0) return;
    char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!buf) return;                    // out of memory on a cold path: the first pass already ran
    const int n = platform::fsRead(path, buf, static_cast<size_t>(size) + 1);
    if (n > 0) { buf[n] = '\0'; reapplyNode(m, buf, ""); }
    platform::free(buf);
}

// Walk the same prefix scheme applyNode uses, overlaying values onto whatever controls exist NOW.
void FilesystemModule::reapplyNode(MoonModule* m, const char* json, const char* prefix) {
    if (!m) return;
    overlayControls(m, json, prefix);
    char childPrefix[MAX_PATH];
    for (uint8_t i = 0; i < m->childCount(); i++) {
        MoonModule* c = m->child(i);
        if (!c) continue;
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        reapplyNode(c, json, childPrefix);
    }
}

// ---- Load ----

// Read a WHOLE file into a heap buffer sized to it (caller frees), no fixed ceiling.
// A large saved config (many light presets, a wide fixture) loads in full instead of being truncated to a fixed buffer and failing to parse.
// Mirrors the streaming save (saveSubtree): both sides cap-free.
static char* readWholeFileAlloc(const char* path) {
    const long size = platform::fsSize(path);
    if (size <= 0) return nullptr;
    char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!buf) { std::printf("FilesystemModule: out of memory loading %s (%ld bytes)\n", path, size); return nullptr; }
    const int n = platform::fsRead(path, buf, static_cast<size_t>(size) + 1);
    if (n <= 0) { platform::free(buf); return nullptr; }
    buf[n] = '\0';                       // parsed as a C-string
    return buf;
}

void FilesystemModule::loadSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return;
    if (char* buf = readWholeFileAlloc(path)) {
        applyNode(m, buf, "");
        platform::free(buf);
    }
}

// The shared resolution. "/.config/<Type>.json", one level deep, to the scheduler index of the live top-level module of that type. -1 when the path is not a config file or no module matches.
int FilesystemModule::moduleIndexForConfigPath(const char* path) {
    if (!scheduler_ || !path) return -1;
    constexpr const char* kPrefix = "/.config/";
    constexpr size_t kPrefixLen = 9;
    if (std::strncmp(path, kPrefix, kPrefixLen) != 0) return -1;
    const char* stem = path + kPrefixLen;
    const size_t stemLen = std::strlen(stem);
    constexpr size_t kExtLen = 5;   // ".json"
    if (stemLen <= kExtLen || std::strcmp(stem + stemLen - kExtLen, ".json") != 0) return -1;
    if (std::memchr(stem, '/', stemLen) != nullptr) return -1;   // one level: presets/ etc. skip
    char type[64];
    const size_t typeLen = stemLen - kExtLen;
    if (typeLen >= sizeof(type)) return -1;
    std::memcpy(type, stem, typeLen);
    type[typeLen] = '\0';
    for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
        MoonModule* m = scheduler_->module(i);
        // m != this: the engine itself has no controls and never persists a file, same exclusion the flush loop makes.
        if (m && m != this && std::strcmp(m->typeName(), type) == 0)
            return m->appliesConfigLive() ? i : -1;   // opted out: the file applies at next boot
    }
    return -1;   // no module of this type (a foreign file named like one): leave the tree alone
}

// See the header. The path names the module: the filename stem IS the top-level typeName (the same contract pathFor writes with).
bool FilesystemModule::applyConfigFile(const char* path) {
    const int idx = moduleIndexForConfigPath(path);
    if (idx < 0) return false;
    MoonModule* m = scheduler_->module(static_cast<uint8_t>(idx));
    char* buf = readWholeFileAlloc(path);
    if (!buf) return false;
    const bool applied = applySubtree(m, buf);
    platform::free(buf);
    // Controls that appear only at prepare() (a script's declared controls) could not take their values yet: reapply after the prepare this write requested.
    if (applied) scheduler_->requestValuesReapply();
    return applied;
}

// See the header: queue for the render task; one bit per top-level module coalesces a multi-file upload to one apply each.
bool FilesystemModule::requestConfigApply(const char* path) {
    const int idx = moduleIndexForConfigPath(path);
    if (idx < 0 || idx >= 32) return false;
    pendingApplyMask_.fetch_or(1u << idx, std::memory_order_relaxed);
    return true;
}

void FilesystemModule::tick20ms() MM_NONBLOCKING {
    uint32_t mask = pendingApplyMask_.exchange(0, std::memory_order_relaxed);
    if (!mask || !scheduler_) return;
    for (uint8_t i = 0; i < 32 && mask; i++, mask >>= 1) {
        if (!(mask & 1)) continue;
        MoonModule* m = scheduler_->module(i);
        char path[MAX_PATH];
        if (m && pathFor(m, path, sizeof(path))) applyConfigFile(path);
    }
    scheduler_->requestPrepareTree();   // one sweep for the whole batch, next tick
}

// Overlay every persistable control's saved value onto the module's current control list, in list order.
void FilesystemModule::overlayControls(MoonModule* m, const char* json, const char* prefix) {
    char key[MAX_KEY];
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c)) continue;
        std::snprintf(key, sizeof(key), "%s%s", prefix, c.name);
        applyValue(c, json, key);
    }
}

// Restore a code-wired child's saved state when its boot index differs from the index the file recorded it at (a reorder of code-wired siblings).
// Scan the saved child entries ("<prefix><j>.type") for the one whose type matches `wired`, and apply that entry's subtree to it.
// If the file has no entry for this wired child (it predates the child), there is nothing to restore and the child keeps its defaults.
void FilesystemModule::applyWiredChildFromJson(MoonModule* wired, const char* json, const char* prefix) {
    for (uint8_t j = 0; ; j++) {
        char typeKey[MAX_KEY];
        std::snprintf(typeKey, sizeof(typeKey), "%s%u.type", prefix, static_cast<unsigned>(j));
        char typeName[32] = {};
        mm::json::parseString(json, typeKey, typeName, sizeof(typeName));
        if (typeName[0] == 0) return;   // walked past the last saved child — no match, keep defaults
        if (std::strcmp(typeName, wired->typeName()) != 0) continue;
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(j));
        applyNode(wired, json, childPrefix);
        return;
    }
}

// True when a live child of `parent` is a code-wired singleton of `typeName`. The reconcile loop uses it to avoid factory-creating a duplicate of a wired type-singleton whose saved entry sits at an index other than its boot position (already restored in place by applyWiredChildFromJson).
bool FilesystemModule::hasWiredChildOfType(const MoonModule* parent, const char* typeName) {
    for (uint8_t i = 0; i < parent->childCount(); i++) {
        MoonModule* c = parent->child(i);
        if (c && c->isWiredByCode() && std::strcmp(c->typeName(), typeName) == 0) return true;
    }
    return false;
}

// Runtime entry point over applyNode.
// See the header for the contract; the reason it exists is the lifecycle gap.
// At boot, Scheduler phases 3 and 4 call setup() and applyState() across the whole tree after the load, so applyNode only has to call defineControls() on a child it creates.
// A runtime caller gets no such phases.
// A module that never saw setup() comes back with its buffers unbuilt and its hardware unclaimed, a preset that "sometimes does not work".
bool FilesystemModule::applySubtree(MoonModule* m, const char* json, const char* prefix) {
    if (!m || !json) return false;
    // Refuse a body that is not credibly one of ours BEFORE touching the tree. applyNode's trim step reads "no children in the JSON" as "delete every live child", so a truncated file (an interrupted upload, a half-written preset) would not leave the current look alone, it would WIPE it. Every subtree we write emits `<prefix>enabled`, so its absence is the cheap, format-specific test for "this is not a subtree", and it costs one key lookup on a cold path.
    char enabledKey[MAX_KEY];
    std::snprintf(enabledKey, sizeof(enabledKey), "%senabled", prefix);
    if (!mm::json::hasKey(json, enabledKey)) {
        std::printf("FilesystemModule: ignoring malformed subtree for %s\n", m->typeName());
        return false;
    }
    applyNode(m, json, prefix);
    // Same order as the runtime add path (HttpServerModule::applyAddModule): setup() may read what defineControls() bound, and applyState() then builds or releases per effectively-enabled. Both recurse over children on their own (MoonModule::setup / applyState), so one call at the root covers every node applyNode just created.
    m->setup();
    m->applyState();
    // The tree just changed shape and values, so it has to be written back.
    // Without this an applied preset renders correctly and is then LOST on reboot, because the boot loader restores the config file that the apply never updated.
    // Marked here rather than in each caller, so every applySubtree user persists by construction.
    m->markDirty();
    noteDirty();
    // Structural change on a live tree: flip the WS full-resync flag through the existing schema hook.
    // An apply with no HTTP request in flight (Home Assistant picking a preset over MQTT or the WLED shim) still reaches every open browser.
    // Same hook rebuildControls uses; no coupling to HttpServerModule.
    MoonModule::notifySchemaChanged();
    return true;
}

void FilesystemModule::applyNode(MoonModule* m, const char* json, const char* prefix) {
    char key[MAX_KEY];
    // Overlay the saved values.
    // A module whose CONTROL SET depends on one of its own control VALUES (the canonical case: ParallelLedDriver's `peripheral` Select, which swaps the bus backend and with it the backend-owned controls, clockPin, dcPin, the ring cluster) needs a second pass: the first overlay writes `peripheral`, but the backend-owned controls are still bound to the DEFAULT backend's members, so their saved values land on a backend about to be discarded.
    // So overlay, then rebuildControls() (which re-runs defineControls → swaps the live backend to match the just-applied `peripheral`, re-binding the control list to the RIGHT backend's members), then overlay again onto the now-correct controls. rebuildControls' schema-hash gate no-ops the refire when nothing changed (the common case: a module with no value-dependent schema), and the second overlay is idempotent value writes, so this is safe and cheap for every module.
    // Without it, any reload that rebuilds the control set (a reboot, an INT_WDT restart) silently reverts every backend-owned control (clockPin, the ring geometry) to its default.
    overlayControls(m, json, prefix);
    m->rebuildControls();
    overlayControls(m, json, prefix);

    std::snprintf(key, sizeof(key), "%senabled", prefix);
    // Note: we can't distinguish "key absent" from "key=false" with the flat parser.
    // The convention: every saved file includes "enabled", so if the file exists and applyNode is reached we assume the key is present.
    // Production callers always emit enabled (see writeNode).
    // If the user hand-edited the file and dropped it, they get enabled=false (matches the default-after-bad-edit behavior).
    m->setEnabled(mm::json::parseBool(json, key));

    // Reconcile children with the JSON's tree shape.
    // Walk each saved child entry ("<prefix><idx>.type") and place the corresponding live child.
    // The JSON index `i` and the live position `pos` are DECOUPLED: `i` always advances; `pos` advances only when a child is actually placed there.
    // This decoupling is what makes a single bad entry non-fatal, a JSON entry that produces no live child (an unknown/renamed type, or a stale slot over a code-wired child) is skipped WITHOUT dropping the user modules the file records after it.
    // User modules are created fresh here in file order via addChild, so their user-chosen order (a UI reorder) round-trips.
    // Code-wired children pre-exist and are skipped in place (see the isWiredByCode() branch below).
    //
    // The decoupling is what keeps a single bad entry from taking the rest of the tree down with it, the failure mode a naive "break on any mismatch/unknown" reconcile has, where one unresolvable entry drops every module the file records after it.
    // The two entries that must skip-not-break:
    //   - a stale slot over a code-wired child (the file predates the wired child, or names a different
    //     type where it now sits): keep the wired instance, advance past it;
    //   - a renamed/removed module type (a documented break rather than a migration, e.g. a pre-consolidation
    //     MoonI80Peripheral/I80Peripheral entry): that entry drops, the rest stay.
    uint8_t pos = 0;
    for (uint8_t i = 0; ; i++) {
        char typeKey[MAX_KEY];
        std::snprintf(typeKey, sizeof(typeKey), "%s%u.type", prefix, static_cast<unsigned>(i));
        char typeName[32] = {};
        mm::json::parseString(json, typeKey, typeName, sizeof(typeName));
        if (typeName[0] == 0) break;

        MoonModule* live = m->child(pos);
        if (!live || std::strcmp(live->typeName(), typeName) != 0) {
            // A code-wired child that mismatches the saved type here is a STALE SLOT (the file predates this code-wired child, or names a different type where it now sits, e.g. boot wired the code-wired siblings in a different order than the file recorded them).
            // Never replace/destroy the wired instance: keep it and advance past it.
            // But first restore ITS saved values, find the JSON entry that names THIS wired child's type and overlay that entry's controls.
            // A code-wired child's persisted state survives even when its saved index differs from its boot index (a reorder).
            // Without this the wired child keeps its defaults on every reboot.
            if (live && live->isWiredByCode()) {
                applyWiredChildFromJson(live, json, prefix);
                pos++;
                continue;
            }
            // A code-wired child is a type-singleton (one per type per container).
            // If a live wired child already has this entry's type, this entry IS that singleton's saved slot, it was restored by the applyWiredChildFromJson type-search above when the wired child sat at an earlier position (its saved index differs from its boot index).
            // Creating here would spawn a DUPLICATE, so drop the entry without advancing `pos`: the singleton already stands in the live tree.
            if (hasWiredChildOfType(m, typeName)) continue;
            MoonModule* created = ModuleFactory::create(typeName);
            if (!created) {
                // Unknown/renamed type: the module drops. Skip this JSON entry and keep reconciling the rest, do NOT advance `pos`, so the file's later user modules still map to the correct live position.
                continue;
            }
            created->defineControls();
            if (live) {
                MoonModule* old = m->replaceChildAt(pos, created);
                if (old) { old->release(); Scheduler::deleteTree(old); }
            } else {
                m->addChild(created);
            }
            // A freshly created module carries the factory's display name, so restoring one config while another tree already holds that name leaves TWO modules answering to it. The boot path gets this from deduplicateNamesInTree, but a config applied after boot (a card saved, a preset recalled) reached the live tree without it: a MoonLiveLayout and a MoonLiveEffect were then both "MoonLive", and every lookup that resolves a module by name (parent_id on an add, the UI's card selector) found whichever came first, so the effect's controls rendered on the layout's card.
            if (auto* sched = Scheduler::instance()) sched->ensureUniqueName(created);
        }

        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        applyNode(m->child(pos), json, childPrefix);
        pos++;
    }
    uint8_t jsonChildCount = pos;   // live children reconciled; the trim loop keeps these, prunes the rest
    // Trim live children beyond what the JSON describes, EXCEPT children that were wired by code at boot (main.cpp annotates those via markWiredByCode).
    // A code-wired child is preserved across persistence loads even when the on-disk file predates its addition, the upgrade-day case where a new release adds a code-created child (e.g.
    // ImprovProvisioningModule under NetworkModule) whose existence the device's saved file doesn't yet know about.
    // Without this exemption the child would get trimmed on every boot.
    //
    // Walks back-to-front so removeChild's left-shift of later siblings doesn't skip an entry.
    // Any code-wired child at index >= jsonChildCount stays; its position relative to the JSON-described children may not match what the file expects.
    // On the first dirty event the next save writes the current (post-merge) tree shape and from then on the file matches.
    uint8_t i = m->childCount();
    while (i > jsonChildCount) {
        i--;
        MoonModule* extra = m->child(i);
        if (!extra) continue;
        if (extra->isWiredByCode()) continue;
        extra->release();
        m->removeChild(extra);
        Scheduler::deleteTree(extra);
    }
}

void FilesystemModule::applyValue(const ControlDescriptor& c, const char* json, const char* key) {
    // Per-type parse + validate + apply lives in Control.cpp.
    // Use Clamp: a stale on-disk value from a schema change should snap to the new bounds (Uint8 200 → max 100), not silently drop to 0.
    // The HTTP API uses Strict instead so a bogus client value surfaces as a 400.
    (void)applyControlValue(c, json, key, ApplyPolicy::Clamp);
}

// ---- Save ----
// Serialize a subtree into a caller's sink.
// The write half of saveSubtree, split out so a caller storing the bytes elsewhere (a named preset file) produces the SAME format the loader reads, rather than a second serializer that could drift from this one.
// See the header.
bool FilesystemModule::saveSubtreeTo(MoonModule* m, JsonSink& sink, const char* prefix) {
    if (!m) return false;
    const bool bare = (prefix == nullptr || prefix[0] == 0);
    if (bare) sink.append("{");   // a namespaced subtree is a fragment of the caller's object
    // firstField=true in BOTH cases: this writes only its own fields, and the caller assembling a larger object owns the separator before each subtree. Emitting a leading comma here as well produced ",," in every preset carrying more than one capture, invalid JSON that our own first-match key reader happened to tolerate.
    writeNode(m, sink, bare ? "" : prefix, /*firstField=*/true);
    if (bare) sink.append("}");
    return !sink.overflowed();           // only trips on an allocation failure, not a size cap
}

// Returns true only when the file was written. On failure (path/overflow/write error) the caller must keep the subtree dirty so the change isn't lost.
bool FilesystemModule::saveSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return false;
    // Serialize the whole subtree into a buffer-mode JsonSink, a growable heap buffer with NO fixed ceiling (the same primitive /api/state streams through), so a large config (many light presets, a wide fixture wiring) persists in full instead of silently truncating. Written atomically once complete.
    JsonSink sink;                       // heap/buffer mode: grows as needed, no cap
    if (!saveSubtreeTo(m, sink)) {
        std::printf("FilesystemModule: out of memory serializing %s\n", path);
        return false;
    }
    if (platform::fsWriteAtomic(path, sink.data(), sink.size())) {
        std::printf("FilesystemModule: saved %s (%zu bytes)\n", path, sink.size());
        return true;
    }
    std::printf("FilesystemModule: write failed for %s\n", path);
    return false;
}

// Append this module's persistable controls, its enabled flag, and (recursively) its children to `sink`.
// `firstField` is true when this is the first field-emitter inside its containing `{`, the top-level call passes true.
// A recursive child call passes false because the parent already emitted its `"N.type"` field, so the child must prefix a comma before its first control.
// No size limit: the sink grows; the old overflow-returns-bool plumbing is gone (an allocation failure surfaces via sink.overflowed() at the top level).
void FilesystemModule::writeNode(MoonModule* m, JsonSink& sink, const char* prefix, bool firstField) {
    bool first = firstField;
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c)) continue;
        sink.appendf("%s\"%s%s\":", first ? "" : ",", prefix, c.name);
        writeControlValue(sink, c);      // the shared value serializer (same as /api/state)
        first = false;
    }
    sink.appendf("%s\"%senabled\":%s", first ? "" : ",", prefix, m->enabled() ? "true" : "false");
    for (uint8_t i = 0; i < m->childCount(); i++) {
        MoonModule* child = m->child(i);
        if (!child) continue;  // addChild rejects nullptr today; defend against future invariants
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        // Emit "0.type":"NoiseEffect" so the reader can detect tree-shape mismatches.
        sink.appendf(",\"%stype\":\"%s\"", childPrefix, child->typeName());
        writeNode(child, sink, childPrefix, /*firstField=*/false);
    }
}

// ---- Dirty walking ----
bool FilesystemModule::subtreeDirty(MoonModule* m) {
    if (!m) return false;
    if (m->dirty()) return true;
    for (uint8_t i = 0; i < m->childCount(); i++) {
        if (subtreeDirty(m->child(i))) return true;
    }
    return false;
}

void FilesystemModule::clearSubtreeDirty(MoonModule* m) {
    if (!m) return;
    m->clearDirty();
    for (uint8_t i = 0; i < m->childCount(); i++) clearSubtreeDirty(m->child(i));
}

// ---- Paths ----
// Filename = "/.config/<TypeName>.json".
// Single instance assumed; multi-instance gets a .N suffix when that becomes a requirement (item 12, module switching).
bool FilesystemModule::pathFor(MoonModule* m, char* out, size_t n) {
    if (!m || m->typeName()[0] == 0) return false;
    int w = mm::formatTo(out, n, "%s/%s.json", CONFIG_DIR, m->typeName());
    return w > 0 && static_cast<size_t>(w) < n;
}

} // namespace mm

/// @}
