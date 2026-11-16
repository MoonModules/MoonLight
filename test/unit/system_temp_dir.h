#pragma once

/// The system's folder for temporary files, where a test makes its own: `TMPDIR` where it is set (macOS, Android's shell), `/tmp` on other POSIX hosts, the user's temp folder on Windows.

#include <filesystem>
#include <string>

namespace mm {

/// The folder, its trailing separator trimmed, so a test appends `/<its own name>`.
inline std::string systemTempDir() {
    std::string dir = std::filesystem::temp_directory_path().string();
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) dir.pop_back();
    return dir;
}

}  // namespace mm
