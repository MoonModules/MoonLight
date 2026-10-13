#pragma once

/// The repo's shipped scripts as files, for the tests that sweep them: their folder and their text.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

/// The repo's script folder, found relative to this header so a test does not depend on the working directory a runner uses.
inline std::filesystem::path scriptRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "moonlive";
}

/// A file's whole text, empty when it cannot be read.
inline std::string readText(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
