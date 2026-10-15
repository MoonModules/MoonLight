/// The device's filesystem browser, whose surface is the UI tree panel rather than a control list. Public surface and class layout live in FileManagerModule.h.
#include "core/system/FileManagerModule.h"

#include "core/system/FilesystemModule.h"   // instance()->lastSavedStr() for the "last saved" readout
#include "platform/platform.h"       // fs* primitives

namespace mm {

void FileManagerModule::defineControls() {
    // `show hidden` is the one setting: the File Manager's surface is the tree panel, and the flag stays bound for the API while the generic control list skips it.
    controls_.addControl("show hidden", showHidden_);      // reveal dot-prefixed entries (e.g. .config)
    controls_.setHidden(controls_.count() - 1, true);
    // The `flash` gauge: the total is read once and the usage on tick1s, since defineControls re-runs and a usage scan walks every block.
    if (totalBytes_ == 0) {
        totalBytes_ = static_cast<uint32_t>(platform::filesystemTotal());
        usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
    }
    if (totalBytes_ > 0) {
        controls_.addProgress("flash", usedBytes_, totalBytes_);
        controls_.setHidden(controls_.count() - 1, true);   // renders as the usage bar in the panel, not generically
    }
    // Bound straight to the FilesystemModule engine's buffer, which its tick1s keeps current, as SystemModule binds its strings.
    if (FilesystemModule* fs = FilesystemModule::instance()) {
        /// How long ago the configuration was last saved.
        controls_.addReadOnly("lastSaved", fs->lastSavedStr());
        controls_.setHidden(controls_.count() - 1, true);   // shown in the panel header, not generically
    }
    MoonModule::defineControls();
}

void FileManagerModule::tick1s() MM_NONBLOCKING {
    // Once a minute: `filesystemUsed()` walks every block, about 80 ms on an S3 inline on the render thread, and a minute-old figure serves this one gauge as well.
    if (totalBytes_ == 0) return;
    if (++secondsSinceScan_ < 60) return;
    secondsSinceScan_ = 0;
    usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
}

void FileManagerModule::setup() {
    MoonModule::setup();
    // `show hidden` is a view preference, off on every boot whatever was saved: setup() runs after persistence overlays it.
    showHidden_ = false;
}

// Creating and deleting are HttpServerModule's /api/dir routes, which carry the path in the request, so this module holds no op state; parseFilePath is the one path guard.

} // namespace mm
