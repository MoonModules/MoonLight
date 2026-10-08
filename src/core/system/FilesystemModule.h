#pragma once


#include "core/module/MoonModule.h"

#include <atomic>

#include <cstddef>
#include <cstdint>

namespace mm {

class Scheduler;

/// Control-list-driven JSON persistence: it writes control values to flash, so settings survive a reboot.
///
/// It is always loaded and runs first, so its load hook fires before any other module sets up.
/// Storage is one file per top-level module, named after the module's type, holding that module's state document.
/// Its bodies live in the `.cpp`, keeping the recompile cost off the tree.
///
/// @moreinfo
///
/// ## One state document per module, applied at load
///
/// A file is the same document a preset or `PATCH /api/state` carries, with the module's secrets in it, since it never leaves the device.
/// Boot applies it as a stored file: unknown types and keys are skipped and logged, values clamp to their bounds, and a wired module stays when the file predates it.
/// The boot phases after the load set up and build what it created, and a script's own controls take their values once it has compiled.
/// Until the release after next, a file in the flat format older builds wrote is read as the document it describes and saved as one (see MIGRATING).
///
/// ## Saving is debounced, and bounded
///
/// Every successful mutation marks its module dirty, tree-shape changes included.
/// A save waits for quiet, then writes each dirty subtree to a temporary file and renames it.
/// A flag clears only once its write succeeds, so a failed write is retried.
///
/// ## Conditional controls, and the first boot
///
/// A module binds its whole control set and hides what does not apply, so a value is always found.
/// On the first boot no file exists, so each module keeps its defaults until something changes.
/// @card FilesystemModule.png
class FilesystemModule : public MoonModule {
public:
    static constexpr const char* CONFIG_DIR = "/.config";   ///< where every config file lives
    static constexpr size_t MAX_PATH = 64;                  ///< the longest path this module builds
    static constexpr uint32_t DEBOUNCE_MS = 2000;           ///< how long a save waits for quiet
    /// The ceiling on a deferred save, so a continuous writer cannot starve one forever.
    static constexpr uint32_t MAX_DEFER_MS = 10000;

    /// Age the ceiling clock, so a test need not sleep through it.
    void ageDirtyForTest(uint32_t ms) { firstDirtyMs_ -= ms; }

    /// The constructor leaves the singleton alone, which `setScheduler` registers.
    FilesystemModule() = default;
    /// Clear the singleton when this is the instance that registered it.
    ~FilesystemModule() override;

    /// Keep flushing whatever the enabled toggle says, so disabling this module cannot lose changes.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Stay out of the UI: this is a pure engine, and FileManagerModule shows its status.
    bool appearsInUi() const override { return false; }

    /// Adopt the scheduler, register the singleton and wire the persistence hooks.
    void setScheduler(Scheduler* s);
    /// Mount the filesystem and create the config directory.
    void setup() override;
    /// Once the debounce or the ceiling expires, write every dirty subtree.
    void tick1s() MM_NONBLOCKING override;

    /// The live "last saved" buffer, which FileManagerModule binds its own control straight at.
    char* lastSavedStr() { return lastSaveStr_; }

    /// The live singleton, or null before the boot wiring registers it.
    static FilesystemModule* instance() { return instance_; }

    /// Save every dirty subtree now, which is the same work the debounce eventually does.
    void flush();

    /// Force the pending saves through from a static context, as the reboot handler does.
    static void flushPending();

    /// Record that something changed, which starts the debounce.
    static void noteDirty();

    /// A whole file in a heap buffer sized to it, null-terminated, which the caller frees; null when absent, empty or out of memory.
    static char* readWholeFile(const char* path);

    /// Build the config path for one top-level module.
    static bool pathFor(MoonModule* m, char* out, size_t n);

    /// Apply a written config file onto the running tree, so a restored backup needs no reboot.
    bool applyConfigFile(const char* path);

    /// Queue a written config file, which the next tick applies on the render task.
    bool requestConfigApply(const char* path);

    /// Apply any config file the upload path queued.
    void tick20ms() MM_NONBLOCKING override;

private:
    static inline FilesystemModule* instance_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    bool mounted_ = false;
    bool dirtyPending_ = false;
    /// Which top-level module a config path names, or a negative number for none.
    int moduleIndexForConfigPath(const char* path);
    std::atomic<uint32_t> pendingApplyMask_{0};   ///< one bit per module; the web task sets, the render tick consumes
    bool everSaved_ = false;       ///< false until the first successful save
    uint32_t lastDirtyMs_ = 0;     ///< when the most recent mark arrived, which the debounce measures from
    /// When the pending save first became dirty, stamped by the first mark after a flush.
    uint32_t firstDirtyMs_ = 0;
    uint32_t lastSaveMs_ = 0;      ///< when the last save landed, which the status string ages from
    char lastSaveStr_[24] = "never";  ///< the status string FileManagerModule reads

    // The module holds no standing buffer: each operation allocates and frees its own.

    /// Refresh the status string from the age of the last save.
    void updateLastSavedStr();
    /// The C hook the scheduler calls, which forwards to the load below.
    static void loadAllHookTrampoline_(Scheduler* s);
    /// Read every module's file and apply it as boot's load.
    void loadAll(Scheduler* s);
    /// Read one top-level module's file and apply it to that subtree.
    void loadSubtree(Scheduler& s, MoonModule* m);
    /// Serialize one subtree to its own file, atomically.
    bool saveSubtree(MoonModule* m);
    /// Whether anything in this subtree is marked dirty.
    static bool subtreeDirty(MoonModule* m);
    /// Clear the dirty mark across a subtree, once its write has succeeded.
    static void clearSubtreeDirty(MoonModule* m);
};

} // namespace mm
