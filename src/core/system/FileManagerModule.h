#pragma once

#include "core/module/MoonModule.h"

#include <cstddef>
#include <cstdint>

namespace mm {

/// Browse and manage the device filesystem: a folder tree with an inline text editor.
/// @card FileManagerModule.png
///
/// Each folder loads its children on first expand. It is distinct from @ref FilesystemModule, the persistence engine that writes the config files this panel shows.
///
/// @guide
///
/// ## Working with files
///
/// Click a folder to select it and open or close it, and click a selected file to open the editor. The toolbar acts on the selection: new folder, new file, delete (press twice) and refresh.
///
/// Drag files from the desktop onto a folder to upload them.
///
/// ## The editor
///
/// The editor pretty-prints JSON on open and saves atomically. A binary file loads read-only: download it instead.
///
/// ## Backup and restore
///
/// Backup downloads the device's files as one `.json` bundle, and Restore uploads one back. Keep the bundle private: it contains the WiFi password. [The how-to](https://moonmodules.org/MoonLight/how-to/backup-and-restore.html) walks through both.
///
/// @technical
///
/// **Browsing lives in the UI.** The tree is a client-side lazy tree, each folder loading its own children on first expand. Expansion is UI state, so the module exposes only the operations and keeps recursion and paging out of core state.
///
/// A leading dot hides an entry unless the toggle is on, the standard convention. The toggle is a per-session view preference, forced off at every boot.
///
/// **Operations are endpoints, not controls.** Creating a folder, deleting an entry and reading or writing a file are HTTP calls. The path rides the request and all share one path guard, so an operation stores nothing and fails cleanly.
///
/// Last-modified dates need a time source and filesystem support, both backlogged. Bodies live in the `.cpp`, so an edit recompiles only that file.
class FileManagerModule : public MoonModule {
public:
    /// Declare the view toggle and the usage gauges.
    void defineControls() override;
    /// Read the partition total, and force the hidden toggle off.
    void setup() override;
    /// Refresh the usage gauge, which walks the partition on a slow cadence.
    void tick1s() MM_NONBLOCKING override;

private:
    /// Reveal dot-prefixed files and folders, such as `.config`.
    bool showHidden_ = false;
    /// Flash used and total, as the usage bar.
    uint32_t usedBytes_ = 0;
    uint32_t totalBytes_ = 0;         ///< the partition total, read once
    uint8_t  secondsSinceScan_ = 0;   ///< paces the usage scan
};

} // namespace mm
