/// @module Drivers
/// @also Palette, MoonLivePalette

/// How a DOWNLOADED palette becomes a selectable one: the scan FINDS the file, the picker LISTS it, and the `palette` control ACCEPTS its index, each pinned separately.

#include "doctest.h"
#include "light/drivers/Drivers.h"
#include "light/util/Palette.h"
#include "core/util/JsonSink.h"
#include "core/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"
#include "../core/conditional_controls.h"   // mm::test::controlIndex

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace mm;

namespace {

/// The smallest palette script stand-in: these tests cover DISCOVERY, never what a palette paints.
constexpr const char* kPaletteSrc = "void setup() {}\n";

/// An empty filesystem root per test, so a count assertion ignores whatever palettes the developer's own device directory carries.
struct IsolatedFs {
    char root[256];
    explicit IsolatedFs(const char* tag) {
        std::snprintf(root, sizeof(root), "/tmp/mm_palettes_%s_%u", tag,
                      static_cast<unsigned>(platform::millis()));
        std::filesystem::remove_all(root);
        platform::fsSetRoot(root);
        REQUIRE(platform::fsMount());
    }
    ~IsolatedFs() {
        platform::fsSetRoot("");        // back to the real root for every other test
        std::filesystem::remove_all(root);
    }
};

/// Write one `.mlp` into a script directory, creating the directory first: fsWriteAtomic does not create parents, exactly as POST /api/file does not.
void writePalette(const char* dir, const char* name) {
    platform::fsMkdir(dir);
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    REQUIRE(platform::fsWriteAtomic(path, kPaletteSrc, std::strlen(kPaletteSrc)));
}

/// The highest index the `palette` control ACCEPTS: its `max`, above which a value is refused with "value out of range".
int32_t paletteMax(Drivers& drv) {
    const int i = test::controlIndex(drv, "palette");
    REQUIRE(i >= 0);
    return drv.controls()[static_cast<uint8_t>(i)].max;
}

}  // namespace

TEST_CASE("a palette downloaded to the factory directory is offered by the picker") {
    // The UI downloads a `.mlp` to the FACTORY directory (`/.moonlive`) so a later edit can shadow it, so the scan reads that directory as well as the USER one (`/moonlive`).
    IsolatedFs fs("factory");
    Drivers drv;
    drv.defineControls();
    const int32_t before = paletteMax(drv);

    writePalette(mm::moonlive::kFactoryScriptDir, "unit-factory.mlp");
    drv.rebuildControls();      // CLEARS then re-defines, which is what a schema change does

    CHECK(paletteMax(drv) == before + 1);
}

TEST_CASE("editing a factory palette leaves one entry, not two") {
    // Editing a factory script saves a same-named file to the user directory, which shadows the factory copy; both are scanned, so a dedupe keeps the picker to one row.
    IsolatedFs fs("both");
    Drivers drv;
    drv.defineControls();
    const int32_t before = paletteMax(drv);

    writePalette(mm::moonlive::kFactoryScriptDir, "unit-both.mlp");
    writePalette(mm::moonlive::kScriptDir, "unit-both.mlp");
    drv.rebuildControls();      // CLEARS then re-defines, which is what a schema change does

    CHECK(paletteMax(drv) == before + 1);   // ONE entry, from two files
}

TEST_CASE("a quote in a live palette's name is escaped, so the options stay parseable") {
    // A palette is named by its file, so an unescaped quote would end the JSON string early and the UI would lose the picker.
    static const char* const kNames[] = {"unit-a\"b"};
    static const char* const kTags[] = {"\U0001F3A8"};
    mm::LivePalettes::set(kNames, kTags, 1);

    // The buffer is sized past tens of kilobytes of built-ins, so an overflow means a defect, not a cap.
    std::vector<char> buf(1 << 17, 0);
    JsonSink sink(buf.data(), buf.size());
    mm::paletteOptions(sink);
    mm::LivePalettes::clear();
    REQUIRE(!sink.overflowed());

    // The quote survives ESCAPED, never as a bare one that would close the string.
    const char* q = std::strstr(buf.data(), "unit-a");
    REQUIRE(q != nullptr);
    CHECK(std::strncmp(q + 6, "\\\"", 2) == 0);
}

TEST_CASE("a palette added while running becomes selectable without a reboot") {
    // The `palette` ceiling is baked when the control is defined while prepare() discovers files, so prepare() rebuilds the controls when the count moves.
    IsolatedFs fs("late");
    Drivers drv;
    drv.defineControls();
    const int32_t before = paletteMax(drv);

    // The file arrives after the control was defined, as a download onto a running device.
    writePalette(mm::moonlive::kFactoryScriptDir, "unit-late.mlp");
    drv.prepare();

    CHECK(paletteMax(drv) == before + 1);
}

namespace {
/// The palette control's value as the device saves and reports it.
std::string savedPalette(Drivers& drv) {
    const int i = test::controlIndex(drv, "palette");
    REQUIRE(i >= 0);
    JsonSink out;
    writeControlValue(out, drv.controls()[static_cast<uint8_t>(i)]);
    return std::string(out.data(), out.size());
}

/// Apply a saved palette value the way the boot load does.
void applyPalette(Drivers& drv, const std::string& value) {
    const int i = test::controlIndex(drv, "palette");
    REQUIRE(i >= 0);
    const std::string json = "{\"palette\":" + value + "}";
    CHECK(applyControlValue(drv.controls()[static_cast<uint8_t>(i)], json.c_str(), "palette",
                            ApplyPolicy::Clamp) == ApplyResult::Ok);
}
}  // namespace

TEST_CASE("the palette saves as its name, built in or scripted") {
    IsolatedFs fs("byname");
    writePalette(mm::moonlive::kFactoryScriptDir, "unit-named.mlp");
    Drivers drv;
    drv.defineControls();
    drv.prepare();

    CHECK(savedPalette(drv) == "\"Rainbow\"");   // the default, built in
    applyPalette(drv, "\"unit-named.mlp\"");
    CHECK(drv.palette == mm::palettes::kCount + mm::moonlive::kPaletteCatalogCount);   // the first of the user's own, after the catalog
    CHECK(savedPalette(drv) == "\"unit-named.mlp\"");
}

// The reason the name is saved: a script added later that sorts before the chosen one moves its index.
TEST_CASE("a scripted palette chosen by name survives a script added before it") {
    IsolatedFs fs("shift");
    writePalette(mm::moonlive::kFactoryScriptDir, "m-chosen.mlp");
    Drivers drv;
    drv.defineControls();
    drv.prepare();
    applyPalette(drv, "\"m-chosen.mlp\"");
    const std::string saved = savedPalette(drv);
    const uint8_t oldIndex = drv.palette;

    writePalette(mm::moonlive::kFactoryScriptDir, "a-first.mlp");   // sorts ahead of m-chosen
    drv.prepare();
    applyPalette(drv, saved);

    CHECK(savedPalette(drv) == "\"m-chosen.mlp\"");
    CHECK(drv.palette == oldIndex + 1);   // the old index now names a-first
}

// A slot, a desk or the autopilot picks a palette by number, so a factory palette keeps its number, and a missing one is listed as absent for download.
TEST_CASE("a factory palette keeps its number whether the device holds it or not") {
    IsolatedFs fs("fixed");
    Drivers drv;
    drv.defineControls();
    drv.prepare();
    REQUIRE(mm::moonlive::kPaletteCatalogCount >= 2);
    CHECK(std::string(mm::LivePalettes::nameAt(1)) == mm::moonlive::kPaletteCatalog[1]);
    CHECK_FALSE(mm::LivePalettes::presentAt(1));

    std::vector<char> buf(1 << 17, 0);
    JsonSink sink(buf.data(), buf.size());
    mm::paletteOptions(sink);
    REQUIRE(!sink.overflowed());
    CHECK(std::strstr(buf.data(), "\"absent\":true") != nullptr);

    writePalette(mm::moonlive::kFactoryScriptDir, mm::moonlive::kPaletteCatalog[1]);
    drv.prepare();
    CHECK(std::string(mm::LivePalettes::nameAt(1)) == mm::moonlive::kPaletteCatalog[1]);   // the same number, now held
    CHECK(mm::LivePalettes::presentAt(1));
}

// The StadBeest autopilot picks its fire palette by number, so a palette added before it in the list must move this test and that script together.
TEST_CASE("the StadBeest fire palette is number 64, the one its autopilot picks") {
    int index = -1;
    for (size_t c = 0; c < mm::moonlive::kPaletteCatalogCount; c++)
        if (std::strcmp(mm::moonlive::kPaletteCatalog[c], "stadbeest-fire.mlp") == 0) index = static_cast<int>(c);
    REQUIRE(index >= 0);
    CHECK(mm::palettes::kCount + index == 64);   // moonlive/services/stadbeest-autopilot.mls: palette = 64
}
