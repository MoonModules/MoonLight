#pragma once

/// The repo's shipped scripts as files, for the tests that sweep them: their folder and their text.

#include "unit/source_tree.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

/// The repo's script folder, found relative to this header so a test does not depend on the working directory a runner uses.
inline std::filesystem::path scriptRoot() {
    return mm::sourceRoot() / "moonlive";
}

/// Whether the repository is on this machine: a test binary run elsewhere, on a phone or a headset, carries the program but not the tree it was built from, so a sweep over the scripts skips there.
inline bool scriptsPresent() { return mm::sourceTreePresent(); }

/// A file's whole text, empty when it cannot be read.
inline std::string readText(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
