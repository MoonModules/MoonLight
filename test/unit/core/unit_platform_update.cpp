/// @module platform

/// Pins a desktop installing a new version of itself as a board does, each test over a scratch copy standing in for the running program.
///
/// An image written through `otaBegin`, `otaWrite` and `otaEnd` replaces a program of this system and processor, the old one kept as `.prev`, the new one `.pending`.
///
/// An image that is no program, misses its digest or is abandoned leaves the program as it was; a program in a folder it cannot write is updated by hand.

#include "doctest.h"
#include "platform/platform.h"
#include "host_update.h"
#include "core/util/sha256.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>   // geteuid
#endif

using namespace mm;
namespace fs = std::filesystem;

namespace {

// Read through FILE: a stream's object takes more stack than the frame limit allows.
std::vector<uint8_t> readBytes(const fs::path& path) {
    std::vector<uint8_t> bytes;
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (!file) return bytes;
    uint8_t chunk[256];
    for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(file);
    return bytes;
}

// Written through a stream, whose file takes the process's umask, as the program's own files do.
void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeText(const fs::path& path, const std::string& text) { writeBytes(path, std::vector<uint8_t>(text.begin(), text.end())); }

std::string readText(const fs::path& path) {
    const std::vector<uint8_t> bytes = readBytes(path);
    return {bytes.begin(), bytes.end()};
}

/// The bytes' SHA-256 in capitals: GitHub writes lowercase, and either case is the same digest.
std::string upperDigestOf(const std::vector<uint8_t>& bytes) {
    char hex[kSha256DigestSize * 2 + 1];
    sha256Hex(bytes.data(), bytes.size(), hex, kSha256DigestSize);
    std::string upper(hex);
    for (char& c : upper) c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
    return upper;
}

/// A folder holding a stand-in program, "old program", which every install in it replaces; the real program path comes back after the test.
struct Scratch {
    fs::path dir;
    fs::path program;
    std::vector<uint8_t> realProgram;   ///< this test binary's bytes: a program for this system and processor, the image a release would carry
    explicit Scratch(const char* name) {
        realProgram = readBytes(platform::programPath());
        dir = fs::temp_directory_path() / (std::string("mm_update_") + name);
        fs::remove_all(dir);
        fs::create_directories(dir);
        program = dir / "MoonLight";
        writeText(program, "old program");
        platform::setTestProgramPath(program);
    }
    ~Scratch() {
        platform::setTestProgramPath({});
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, ec);
        fs::remove_all(dir, ec);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    /// Write `image` through the image writer in chunks, as the upload route does, and commit it or not.
    bool install(const std::vector<uint8_t>& image, bool commit = true) const {
        char status[64] = {};
        uint32_t written = 0;
        platform::OtaWrite w;
        if (!platform::otaBegin(w, platform::OtaTarget::App, image.size(), status, sizeof(status), &written)) return false;
        for (size_t at = 0; at < image.size(); at += 4096) {
            const size_t n = image.size() - at < 4096 ? image.size() - at : 4096;
            if (!platform::otaWrite(w, image.data() + at, n)) return false;
        }
        CHECK(written == image.size());
        return platform::otaEnd(w, commit);
    }
};

/// Whether this test runs as root, whose program is updated by hand.
bool runningAsRoot() {
#ifdef _WIN32
    return false;
#else
    return ::geteuid() == 0;
#endif
}

/// A container's program is its host's to replace and root's is updated by hand, so the self-update cases run as a native user only.
const auto kSelfUpdating = doctest::skip(platform::inContainer() || runningAsRoot());

}  // namespace

TEST_CASE("a native program in a folder it can write updates itself, one in a folder it cannot write is updated by hand" * kSelfUpdating) {
    Scratch s("mode");
    CHECK(platform::updateMode() == platform::UpdateMode::Self);
#ifndef _WIN32
    fs::permissions(s.dir, fs::perms::owner_read | fs::perms::owner_exec);
    CHECK(platform::updateMode() == platform::UpdateMode::Manual);
#endif
    CHECK(std::string(platform::updateModeName(platform::UpdateMode::Self)) == "self");
    CHECK(std::string(platform::updateModeName(platform::UpdateMode::Host)) == "host");
    CHECK(std::string(platform::updateModeName(platform::UpdateMode::Manual)) == "manual");
}

TEST_CASE("an image written through the image writer replaces the program and keeps the old one as .prev, pending until it has run" * kSelfUpdating) {
    Scratch s("install");
    REQUIRE(s.install(s.realProgram));
    CHECK(readBytes(s.program) == s.realProgram);
    CHECK(readText(fs::path(s.program.string() + ".prev")) == "old program");
    CHECK(fs::exists(s.program.string() + ".pending"));
    CHECK_FALSE(fs::exists(s.dir / "MoonLight.next"));
}

TEST_CASE("an image that is no program for this system leaves the program as it was" * kSelfUpdating) {
    Scratch s("refused");
    const std::string page = "<html>404 Not Found</html>";
    CHECK_FALSE(s.install(std::vector<uint8_t>(page.begin(), page.end())));
    CHECK(readText(s.program) == "old program");
    CHECK_FALSE(fs::exists(s.program.string() + ".prev"));
    CHECK_FALSE(fs::exists(s.dir / "MoonLight.next"));
}

TEST_CASE("an abandoned image leaves the program as it was" * kSelfUpdating) {
    Scratch s("abandoned");
    CHECK_FALSE(s.install(s.realProgram, false));
    CHECK(readText(s.program) == "old program");
    CHECK_FALSE(fs::exists(s.dir / "MoonLight.next"));
}

TEST_CASE("an image missing the release's digest leaves the program as it was, and one matching it in either case is installed" * kSelfUpdating) {
    Scratch s("digest");
    const fs::path next = s.dir / "MoonLight.next";
    writeBytes(next, s.realProgram);
    char status[64] = {};
    const std::string wrong(kSha256DigestSize * 2, 'a');
    CHECK_FALSE(platform::putProgramInPlace(next, s.program, wrong.c_str(), status, sizeof(status)));
    CHECK(std::string(status).find("digest") != std::string::npos);
    CHECK(readText(s.program) == "old program");

    writeBytes(next, s.realProgram);
    CHECK(platform::putProgramInPlace(next, s.program, upperDigestOf(s.realProgram).c_str(), status, sizeof(status)));
    CHECK(readBytes(s.program) == s.realProgram);
}

TEST_CASE("a desktop has one program: MoonBase and the radio chip are refused") {
    Scratch s("target");
    char status[64] = {};
    uint32_t written = 0;
    platform::OtaWrite w;
    CHECK_FALSE(platform::otaBegin(w, platform::OtaTarget::MoonBase, 0, status, sizeof(status), &written));
    CHECK_FALSE(platform::otaBegin(w, platform::OtaTarget::Coprocessor, 0, status, sizeof(status), &written));
    CHECK(std::string(status).rfind("error:", 0) == 0);
}

TEST_CASE("an image larger than a program could be is refused before a byte is written" * kSelfUpdating) {
    Scratch s("too_large");
    char status[64] = {};
    uint32_t written = 0;
    platform::OtaWrite w;
    CHECK_FALSE(platform::otaBegin(w, platform::OtaTarget::App, size_t{1} << 30, status, sizeof(status), &written));
    CHECK(std::string(status).find("too large") != std::string::npos);
    CHECK_FALSE(fs::exists(s.dir / "MoonLight.next"));
}
