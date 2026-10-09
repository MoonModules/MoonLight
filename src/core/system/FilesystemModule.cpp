/// @defgroup filesystem_impl Filesystem implementation
/// The persistence engine: writes control values to `/.config/*.json` and restores them on boot. Public surface and class layout live in FilesystemModule.h.
/// @{
#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/FilesystemModule.h"

#include "core/module/Control.h"
#include "core/util/JsonSink.h"   // the growable sink the save and the boot conversion write into
#include "core/util/JsonUtil.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "platform/platform.h"

#include <cstdio>
#include <new>      // placement new: removeTree's heap DirLevel
#include <cstring>

namespace mm {

namespace {

/// One directory level, collected. fsList hands entries to a C callback while the directory is open.
/// Removing a file from inside that callback mutates what is being walked, which LittleFS does not promise to survive. So a level is read out first, then acted on.
struct DirLevel {
    static constexpr uint8_t kMax = 64;   ///< entries per level; a deeper listing is deleted in passes
    char names[kMax][40];
    bool isDir[kMax];
    uint8_t count = 0;
    bool truncated = false;
};

void collectEntry(const char* name, bool isDir, uint32_t, void* user) {
    auto* lvl = static_cast<DirLevel*>(user);
    if (lvl->count >= DirLevel::kMax) { lvl->truncated = true; return; }
    if (!name || std::strlen(name) >= sizeof(lvl->names[0])) return;
    std::snprintf(lvl->names[lvl->count], sizeof(lvl->names[0]), "%s", name);
    lvl->isDir[lvl->count] = isDir;
    lvl->count++;
}

}  // namespace

// Depth-first, because a directory goes only once empty; `depth` bounds the recursion since a user shapes this tree on the render task.
bool FilesystemModule::removeTree(const char* path, uint8_t depth) {
    if (depth > 8) return false;
    if (platform::fsRemove(path)) return true;   // a file, or an already-empty directory

    // The listing lives on the heap: a DirLevel is ~2.6 KB, users nest folders freely, and the main task has 12 KB of stack (CONFIG_ESP_MAIN_TASK_STACK_SIZE).
    auto* raw = platform::alloc(sizeof(DirLevel));
    if (!raw) return false;                      // no room to list: report failure, delete nothing
    // Placement new rather than assigning the two fields by hand: DirLevel already declares its defaults, and a copy here silently skips whatever member is added to it next.
    DirLevel* lvlp = new (raw) DirLevel;
    DirLevel& lvl = *lvlp;
    struct Freer { DirLevel* p; ~Freer() { p->~DirLevel(); platform::free(p); } } freer{lvlp};
    platform::fsList(path, &collectEntry, &lvl);
    if (lvl.count == 0) return false;            // not a directory, or unreadable: the failure stands

    bool ok = true;
    for (uint8_t i = 0; i < lvl.count; i++) {
        char child[192];
        // A truncated child path names a different file than the one listed, so a length at or past the buffer skips it.
        const int n = std::snprintf(child, sizeof(child), "%s/%s", path, lvl.names[i]);
        if (n < 0 || static_cast<size_t>(n) >= sizeof(child)) { ok = false; continue; }
        if (!removeTree(child, static_cast<uint8_t>(depth + 1))) ok = false;
    }
    // A level wider than kMax leaves entries behind, so the directory is still not empty. Report the failure rather than a false success: the caller can delete again to take the next batch.
    if (!ok || lvl.truncated) return false;
    return platform::fsRemove(path);
}

FilesystemModule::~FilesystemModule() {
    if (instance_ == this) instance_ = nullptr;
}

void FilesystemModule::setScheduler(Scheduler* s) {
    scheduler_ = s;
    instance_ = this;
    if (s) {
        s->setLoadAllHook(&loadAllHookTrampoline_);
        // Scheduler::setControl calls this after a mutation so a control set from anywhere (IR, WLED bridge, /api/control) schedules the same debounced save. noteDirty is a static. A plain function pointer suffices, no trampoline needed.
        s->setNoteDirtyHook(&FilesystemModule::noteDirty);
    }
}

void FilesystemModule::setup() {
    // Both failures name the directory and report once here, because a failed save per module per change would bury the one fact that explains it.
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

// No controls and no card: FileManagerModule shows the "last saved" status via lastSavedStr() and the filesystem-usage gauge.

void FilesystemModule::tick1s() MM_NONBLOCKING {
    if (!mounted_ || !scheduler_) return;
    updateLastSavedStr();
    if (!dirtyPending_) return;
    const uint32_t now = platform::millis();
    // Either saves: the debounce coalesces a burst of edits, and the ceiling bounds the wait because a control driven at 50 Hz never goes quiet.
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
        loadSubtree(*s, m);
    }
}

// ---- Load ----

// No fixed ceiling, so a large saved config loads in full, mirroring the streaming save (saveSubtree).
char* FilesystemModule::readWholeFile(const char* path) {
    const long size = platform::fsSize(path);
    if (size <= 0) return nullptr;
    char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!buf) { std::printf("FilesystemModule: out of memory loading %s (%ld bytes)\n", path, size); return nullptr; }
    const int n = platform::fsRead(path, buf, static_cast<size_t>(size) + 1);
    if (n <= 0) { platform::free(buf); return nullptr; }
    buf[n] = '\0';                       // parsed as a C-string
    return buf;
}

// The file is the module's state document, applied as boot's load: what this build cannot place is skipped and logged.
void FilesystemModule::loadSubtree(Scheduler& s, MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return;
    char* buf = readWholeFile(path);
    if (!buf) return;
    // Temporary, see MIGRATING: a file in the flat format of builds before 2026-10-08 is read as the document it describes, and saved as one.
    JsonSink converted;
    const bool flat = flatToStateDocument(buf, *m, converted);
    const StateDocumentResult r = applyStateDocument(s, flat ? converted.data() : buf, StateSource::Boot);
    if (!r.ok) std::printf("FilesystemModule: %s not loaded: %s\n", path, r.error);
    if (flat && r.ok) {
        std::printf("FilesystemModule: %s converted to a state document\n", path);
        m->markDirty();
        noteDirty();
    }
    platform::free(buf);
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
    char* buf = readWholeFile(path);
    if (!buf) return false;
    // Applied as a stored file: lenient where this build differs, with the lifecycle, the prepare and the save a running tree needs.
    const StateDocumentResult r = applyStateDocument(*scheduler_, buf, StateSource::Stored);
    platform::free(buf);
    if (!r.ok) std::printf("FilesystemModule: %s not applied: %s\n", path, r.error);
    return r.ok;
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

// ---- Save ----
// Returns true only when the file was written. On failure (path/overflow/write error) the caller must keep the subtree dirty so the change isn't lost.
bool FilesystemModule::saveSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return false;
    // The module's state document, secrets included since the file is the device's own; a growable sink with no ceiling, written atomically once complete.
    JsonSink sink;
    sink.append("{");
    writeStateMember(sink, *m, /*withSecrets=*/true);
    sink.append("}");
    if (sink.overflowed()) {
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
// Filename = "/.config/<TypeName>.json". Single instance assumed; multi-instance gets a .N suffix when that becomes a requirement (item 12, module switching).
bool FilesystemModule::pathFor(MoonModule* m, char* out, size_t n) {
    if (!m || m->typeName()[0] == 0) return false;
    int w = mm::formatTo(out, n, "%s/%s.json", CONFIG_DIR, m->typeName());
    return w > 0 && static_cast<size_t>(w) < n;
}

} // namespace mm

/// @}
