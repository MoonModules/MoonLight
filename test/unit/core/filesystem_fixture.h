#pragma once

/// The FilesystemModule tests' fixture: a settings folder of its own per test, `/tmp/mm_<tag>_<millis>`, with the filesystem pointed at it for the test's life and removed after.

#include "platform/platform.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace {
struct TempRoot {
    char path[256];   ///< the folder, `/tmp/mm_<tag>_<millis>`
    /// Make the folder, `/.config` in it when `withConfig`, and point the filesystem at it.
    explicit TempRoot(const char* tag, bool withConfig = false) {
        std::snprintf(path, sizeof(path), "/tmp/mm_%s_%u", tag, static_cast<unsigned>(mm::platform::millis()));
        std::filesystem::remove_all(path);
        if (withConfig) std::filesystem::create_directories(std::string(path) + "/.config");
        mm::platform::fsSetRoot(path);
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
