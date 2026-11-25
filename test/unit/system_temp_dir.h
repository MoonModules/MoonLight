#pragma once

/// The system's folder for temporary files, where a test makes its own: `TMPDIR` where it is set (macOS, Android's shell), `/tmp` on other POSIX hosts, the user's temp folder on Windows.

#include <filesystem>
#include <random>
#include <string>

namespace mm {

/// The folder, its trailing separator trimmed, so a test appends `/<its own name>`.
inline std::string systemTempDir() {
    std::string dir = std::filesystem::temp_directory_path().string();
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir;
}

/// A fixture folder of this test run's own, `<temp>/<prefix>_<run>_<n>`: the run's random tag keeps two runs at once (another shell, a CI shard) from removing each other's folder, and `n` counts the fixtures within the run.
inline std::string uniqueTempDir(const char* prefix) {
    static const unsigned run = std::random_device{}();
    static unsigned n = 0;
    return systemTempDir() + "/" + prefix + "_" + std::to_string(run) + "_" + std::to_string(++n);
}

}  // namespace mm
