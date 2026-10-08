/// @defgroup filesystem_impl Filesystem implementation
/// The persistence engine: writes control values to `/.config/*.json` and restores them on boot.
/// Public surface and class layout live in FilesystemModule.h.
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

// The file is the module's state document, applied as boot's load: what this build cannot place is skipped and logged, and the boot phases set up and build what it created.
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
// Filename = "/.config/<TypeName>.json".
// Single instance assumed; multi-instance gets a .N suffix when that becomes a requirement (item 12, module switching).
bool FilesystemModule::pathFor(MoonModule* m, char* out, size_t n) {
    if (!m || m->typeName()[0] == 0) return false;
    int w = mm::formatTo(out, n, "%s/%s.json", CONFIG_DIR, m->typeName());
    return w > 0 && static_cast<size_t>(w) < n;
}

} // namespace mm

/// @}
