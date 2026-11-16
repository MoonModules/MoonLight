#pragma once

/// The repository on disk, for the tests that read a file it ships: its root and whether it is there.

#include <filesystem>

#include "host_android.h"

namespace mm {

/// The repo root: on a device the `repo` folder test_android.py pushes the files tests read into and runs beside, else found from this header's own path, whatever the runner's working folder.
inline std::filesystem::path sourceRoot() {
    if constexpr (kHostIsAndroid) return "repo";
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

/// Whether the repository's shipped files are on this machine: a test binary run on a phone or a headset carries the program but not the tree it was built from unless it was pushed.
inline bool sourceTreePresent() { return std::filesystem::is_directory(sourceRoot() / "moonlive"); }

}  // namespace mm
