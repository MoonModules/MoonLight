#pragma once

/// The FilesystemModule tests' fixture: a settings folder of its own per test, `<temp>/mm_<tag>_<millis>`, with the filesystem pointed at it for the test's life and removed after.

#include "platform/platform_clock.h"   // millis
#include "platform/platform_fs.h"   // fsSetRoot
#include "unit/system_temp_dir.h"   // mm::systemTempDir: the system temp folder, which Android and Windows place elsewhere than /tmp

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
struct TempRoot {
    std::string path;   ///< the folder, `<temp>/mm_<tag>_<millis>`
    /// Make the folder, `/.config` in it when `withConfig`, and point the filesystem at it.
    explicit TempRoot(const char* tag, bool withConfig = false) {
        path = mm::systemTempDir() + "/mm_" + tag + "_" + std::to_string(mm::platform::millis());
        std::filesystem::remove_all(path);
        if (withConfig) std::filesystem::create_directories(path + "/.config");
        mm::platform::fsSetRoot(path.c_str());
    }
    /// Point the filesystem back at its default and remove the folder.
    ~TempRoot() {
        mm::platform::fsSetRoot("");
        std::filesystem::remove_all(path);
    }
    /// Non-copyable: one test owns one folder.
    TempRoot(const TempRoot&) = delete;
    /// Non-assignable, for the same reason.
    TempRoot& operator=(const TempRoot&) = delete;
};
}  // namespace
