#pragma once

/// The repository on disk, for the tests that read a file it ships: its root and whether it is there.

#include <cstdlib>
#include <filesystem>

namespace mm {

/// The repo root: MM_SOURCE_ROOT where set (a device pushed the folders tests read, test_android.py), else found from this header's own path, whatever the runner's working folder.
inline std::filesystem::path sourceRoot() {
    if (const char* env = std::getenv("MM_SOURCE_ROOT"); env && *env) return env;
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

/// Whether the repository's shipped files are on this machine: a test binary run on a phone or a headset carries the program but not the tree it was built from unless it was pushed.
inline bool sourceTreePresent() { return std::filesystem::is_directory(sourceRoot() / "moonlive"); }

}  // namespace mm
