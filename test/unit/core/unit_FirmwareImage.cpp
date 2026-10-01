/// Reading a firmware image's own header, which is what stands between a mistyped URL and a board with no recovery image.
///
/// @moreinfo
///
/// The MoonBase update ERASES the factory partition before it can know whether a stream is any good, so everything that can reject an image is decided from its first chunk.
/// These tests are that decision.
///
/// ## Real binaries, not only synthetic ones
///
/// The offsets are pinned against REAL binaries wherever the build tree has them.
/// The layout comes from the ESP32 toolchain, not from us, and a test that only checks our own synthetic bytes would agree with itself while disagreeing with every actual image.
/// Every image found is checked, and a found image that cannot be parsed FAILS rather than being skipped: skipping the unparseable ones is skipping exactly what this catches.
///
/// ## Empty means not built, not passed
///
/// A checkout that has not built for ESP32 has no images to check.
/// That case has to stay distinguishable from "present but not checked".
/// A loop that skips missing files and asserts nothing reports green either way, which looks exactly like a test that ran.

#include "doctest.h"

#include "core/util/FirmwareImage.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <vector>

using namespace mm::firmware;

namespace {

std::filesystem::path repoRoot() {
    // test/unit/core/<this file> -> repo root
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

/// A synthetic image head: magic, chip id, and a descriptor naming a project.
std::vector<uint8_t> makeImage(uint8_t magic, uint16_t chip, const char* project,
                               const char* version = "4.0.0-dev", bool withDesc = true) {
    std::vector<uint8_t> b(kIdentifyBytes, 0);
    b[0] = magic;
    b[kChipIdOffset] = static_cast<uint8_t>(chip & 0xFF);
    b[kChipIdOffset + 1] = static_cast<uint8_t>(chip >> 8);
    if (withDesc) {
        const uint32_t m = kDescMagic;
        std::memcpy(b.data() + kDescOffset, &m, 4);
        std::memcpy(b.data() + kDescOffset + 16, version, std::strlen(version));
        std::memcpy(b.data() + kDescOffset + 48, project, std::strlen(project));
    }
    return b;
}

std::vector<uint8_t> readHead(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> b(kIdentifyBytes);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(b.size()));
    b.resize(static_cast<size_t>(f.gcount()));
    return b;
}

} // namespace

namespace {

/// The built images present in the tree, as (path, expected project name) pairs: @xref{empty-means-not-built-not-passed}.
std::vector<std::pair<std::filesystem::path, std::string>> builtImages() {
    std::vector<std::pair<std::filesystem::path, std::string>> out;
    const auto build = repoRoot() / "build";
    if (!std::filesystem::exists(build)) return out;
    for (const auto& e : std::filesystem::directory_iterator(build)) {
        if (!e.is_directory()) continue;
        for (const auto& [file, project] :
             {std::pair<const char*, const char*>{"MoonLight-moonbase.bin", "MoonLight-moonbase"},
              std::pair<const char*, const char*>{"MoonLight.bin", "MoonLight"}}) {
            const auto p = e.path() / file;
            if (std::filesystem::exists(p)) out.emplace_back(p, project);
        }
    }
    return out;
}

} // namespace

TEST_CASE("every built image identifies as what it is") {
    // THE PIN THAT MATTERS: our offsets against images the ESP32 toolchain actually produced: @xref{real-binaries-not-only-synthetic-ones}.
    const auto images = builtImages();
    if (images.empty()) {
        MESSAGE("no built ESP32 images in the tree; run build_esp32.py to exercise this");
        return;                     // a fresh checkout, not a failure
    }
    for (const auto& [path, project] : images) {
        CAPTURE(path.string());
        const auto head = readHead(path);
        REQUIRE(head.size() >= kIdentifyBytes);
        const auto info = identify(head.data(), head.size());
        CHECK(info.valid);                                  // begins with the image magic
        CHECK(info.described);                              // carries a readable descriptor
        CHECK(info.chip != ChipId::Invalid);                // names a chip we know
        CHECK(std::string(info.project) == project);
        // And the rule the install path runs: a MoonBase image is accepted for its own chip, an app image is refused whatever the chip. Both directions, against real binaries.
        const char* verdict = moonBaseRejection(info, info.chip);
        if (project == "MoonLight-moonbase") CHECK(verdict == nullptr);
        else CHECK(verdict == std::string("not a MoonBase image"));
    }
    MESSAGE("checked ", images.size(), " built image(s)");
}

TEST_CASE("an HTML error page is refused before anything is erased") {
    // What a wrong URL actually delivers: a 404 body or a login page, with a 200 status.
    const char* html = "<!DOCTYPE html><html><head><title>404 Not Found</title></head>";
    const auto info = identify(reinterpret_cast<const uint8_t*>(html), std::strlen(html));
    CHECK_FALSE(info.valid);
    CHECK(moonBaseRejection(info, ChipId::Esp32S3) == std::string("not a firmware image"));
}

TEST_CASE("an image for the wrong chip is refused") {
    // There is one MoonBase per chip and they are one paste apart. A checksum does NOT catch this: the image is perfectly valid, it just cannot execute on this silicon.
    const auto classic = makeImage(kImageMagic, 0x0000, "MoonLight-moonbase");
    const auto info = identify(classic.data(), classic.size());
    CHECK(info.valid);
    CHECK(info.chip == ChipId::Esp32);
    CHECK(moonBaseRejection(info, ChipId::Esp32) == nullptr);            // right chip: accepted
    CHECK(moonBaseRejection(info, ChipId::Esp32S3) == std::string("image is for another chip"));
}

TEST_CASE("an image with no description is refused") {
    const auto bare = makeImage(kImageMagic, 0x0009, "", "", /*withDesc=*/false);
    const auto info = identify(bare.data(), bare.size());
    CHECK(info.valid);
    CHECK_FALSE(info.described);
    CHECK(moonBaseRejection(info, ChipId::Esp32S3) == std::string("image carries no description"));
}

TEST_CASE("a truncated stream reports what it can rather than reading past its buffer") {
    // The first chunk can arrive short. Reading a fixed 128 bytes from a 40-byte buffer is how a vetting check becomes the vulnerability it was added to prevent.
    const auto full = makeImage(kImageMagic, 0x0009, "MoonLight-moonbase");
    for (size_t n : {size_t{0}, size_t{1}, size_t{12}, size_t{13}, size_t{31}, size_t{40},
                     kDescOffset + 95}) {
        const auto info = identify(full.data(), n);         // must not read past n
        if (n == 0) { CHECK_FALSE(info.valid); continue; }
        CHECK(info.valid);                                   // the magic is there
        CHECK_FALSE(info.described);                         // but the descriptor is not complete
        CHECK(moonBaseRejection(info, ChipId::Esp32S3) != nullptr);
    }
}

TEST_CASE("a 32-character project name that fills its field stays terminated") {
    // IDF leaves a name that exactly fills its 32 bytes unterminated. Copying all 32 into a 32-byte buffer would leave no NUL, and every later strcmp would run off the end.
    auto b = makeImage(kImageMagic, 0x0009, "MoonLight-moonbase");
    std::memset(b.data() + kDescOffset + 48, 'x', 32);       // no terminator in the field
    const auto info = identify(b.data(), b.size());
    CHECK(std::strlen(info.project) == 31);                  // truncated, and NUL-terminated
    CHECK(moonBaseRejection(info, ChipId::Esp32S3) == std::string("not a MoonBase image"));
}

TEST_CASE("an app image is accepted under the name it ships with and the name v6.0.0 shipped with") {
    // The new name ships, and the old one stays accepted so a device can still go back to the release that bridged them.
    CHECK(isAppImage("MoonLight"));
    CHECK(isAppImage("projectMM"));
    CHECK(std::string(kAppImageNames[0]) == "MoonLight");
}

TEST_CASE("a stranger's image is refused, however close its name") {
    CHECK_FALSE(isAppImage("WLED"));
    CHECK_FALSE(isAppImage(""));
    CHECK_FALSE(isAppImage("MoonLight2"));        // a longer name that merely begins the same
    CHECK_FALSE(isAppImage("MoonLigh"));          // and a shorter one
    CHECK_FALSE(isAppImage("moonlight"));         // the descriptor is case-sensitive
}

TEST_CASE("a recovery image is recognized under both names and never passes as an app") {
    // The guard that keeps a recovery image out of the app slot has to know the next name too: one it does not recognize is written over the app.
    for (const char* name : {"projectMM-moonbase", "MoonLight-moonbase"}) {
        CAPTURE(name);
        CHECK(isMoonBaseImage(name));
        CHECK_FALSE(isAppImage(name));
        const auto image = makeImage(kImageMagic, 0x0009, name);
        CHECK(moonBaseRejection(identify(image.data(), image.size()), ChipId::Esp32S3) == nullptr);
    }
    for (const char* name : {"projectMM", "MoonLight"}) {
        CAPTURE(name);
        CHECK_FALSE(isMoonBaseImage(name));
    }
}

TEST_CASE("a name that fills its descriptor field unterminated is still compared within the field") {
    // ESP-IDF's own descriptor field is 32 bytes and may carry no terminator, so the compare is bounded by the field.
    char field[32];
    std::memset(field, 'x', sizeof(field));
    CHECK_FALSE(isAppImage(field, sizeof(field)));
    std::memcpy(field, "MoonLight", 10);          // terminated inside the field, junk after it
    CHECK(isAppImage(field, sizeof(field)));
}
