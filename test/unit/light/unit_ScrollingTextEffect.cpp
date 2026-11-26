/// @module ScrollingTextEffect

#include "doctest.h"
#include "light/effects/ScrollingTextEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/powerfunctions/clock_text.h"
#include "platform/platform_fs.h"
#include "unit/system_temp_dir.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

using namespace mm;

namespace {

struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    ScrollingTextEffect text;
    explicit Scene(int w, int h) { grid.width = w; grid.height = h; grid.depth = 1; layouts.addChild(&grid); layer.setChannelsPerLight(3); layer.setLayouts(&layouts); layer.addChild(&text); }
    int litPixels() {
        const uint8_t* p = layer.buffer().data();
        int n = 0;
        for (size_t i = 2; i < layer.buffer().bytes(); i += 3) n += (p[i - 2] | p[i - 1] | p[i]) != 0;
        return n;
    }
    bool litAt(int x, int y) {
        auto& b = layer.buffer();
        const size_t o = (static_cast<size_t>(y) * static_cast<size_t>(grid.width) + static_cast<size_t>(x)) * 3;
        return o + 2 < b.bytes() && (b.data()[o] || b.data()[o + 1] || b.data()[o + 2]);
    }
};

/// A filesystem root of this test's own, put back when the case ends.
struct FsRoot {
    std::string prev;
    std::string dir;
    FsRoot() : prev(platform::fsRootPath() ? platform::fsRootPath() : ""), dir(uniqueTempDir("scrollfont")) {
        std::filesystem::create_directories(dir);
        platform::fsSetRoot(dir.c_str());
        platform::fsMount();
    }
    ~FsRoot() {
        platform::fsSetRoot(prev.empty() ? nullptr : prev.c_str());
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

/// Puts the palette back when the case ends, so a later case still sees the rainbow.
struct HoldPalette {
    Palette saved = *Palettes::active();
    ~HoldPalette() { Palettes::setActiveDirect(saved); }
};

/// True when every lit pixel is `c`, and at least one pixel is lit.
bool paintIs(Scene& s, RGB c) {
    const auto& b = s.layer.buffer();
    bool saw = false;
    for (size_t i = 0; i + 2 < b.bytes(); i += 3) {
        if (!b.data()[i] && !b.data()[i + 1] && !b.data()[i + 2]) continue;
        if (b.data()[i] != c.r || b.data()[i + 1] != c.g || b.data()[i + 2] != c.b) return false;
        saw = true;
    }
    return saw;
}

/// The 4x6 bang the packing note describes: column 2 lit on rows 0, 1, 2 and 4.
const uint8_t kBang[] = {
    0x57, 0x06, 0x04, 0x00, 0x00, '!', '!', 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x20, 0x20,
};

/// A 16 by 8 grid, letters AB, on a black palette, after one frame.
void drawAB(Scene& s) {
    Palettes::setActiveDirect(Palette{});
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.text.font = 3;
    s.text.hue = 0;
    s.text.brightness = 255;
    s.layer.applyState();
    s.layer.tick();
}

/// Install bang.wbf and draw "!" with it.
void drawBang(Scene& s) {
    CHECK(platform::fsWriteAtomic("/moonlive/fonts/bang.wbf", reinterpret_cast<const char*>(kBang), sizeof(kBang)));
    std::snprintf(s.text.text_, sizeof(s.text.text_), "!");
    s.text.font = wbf::kCount;
    s.layer.applyState();
    s.layer.tick();
}

Civil sampleClock() {
    Civil c;
    c.year = 2026; c.month = 11; c.day = 26;
    c.hour = 14; c.minute = 5; c.second = 9;
    c.weekday = 4;   // Thursday
    return c;
}

}  // namespace

// #HH:#MM is the hour and the minute, zero-padded. #MM is the minute, #MO the month.
TEST_CASE("ScrollingTextEffect: #HH:#MM expands to the zero-padded clock") {
    char out[64];
    const Civil c = sampleClock();
    expandClock(out, sizeof(out), "#HH:#MM", c, false);
    CHECK(std::string(out) == "14:05");
    expandClock(out, sizeof(out), "#HH0:#MM", c, false);
    CHECK(std::string(out) == "14:05");   // the WLED 0 mark is accepted, and the field was already padded

    Civil morning = c;
    morning.hour = 9;
    expandClock(out, sizeof(out), "#HH:#MM", morning, false);
    CHECK(std::string(out) == "09:05");

    expandClock(out, sizeof(out), "#TIME #DATE #MON #DAY", c, false);
    CHECK(std::string(out) == "14:05:09 26.11.2026 Nov Thu");
    expandClock(out, sizeof(out), "#TIME", c, true);
    CHECK(std::string(out) == "02:05:09 PM");
    expandClock(out, sizeof(out), "#MO/#DD/#YYYY", c, false);
    CHECK(std::string(out) == "11/26/2026");
    expandClock(out, sizeof(out), "no #ZZ tokens", c, false);
    CHECK(std::string(out) == "no #ZZ tokens");
}

// A line that fits stays centered. A wider line starts off the right and walks left with elapsed time.
TEST_CASE("ScrollingTextEffect: scroll position follows elapsed time") {
    CHECK(ScrollingTextEffect::originX(16, 8, 0, 40, false) == 4);          // centered
    CHECK(ScrollingTextEffect::originX(16, 40, 0, 40, false) == 16);         // off the right edge
    CHECK(ScrollingTextEffect::originX(16, 40, 1000, 10, false) == 6);       // 10 px in one second
    CHECK(ScrollingTextEffect::originX(16, 40, 0, 40, true) == 1 - 40);      // entering from the left
    CHECK(ScrollingTextEffect::originY(16, 8, 128) == 4);
    CHECK(ScrollingTextEffect::originY(16, 8, 0) == 4 - 16);
}

// Every shipped face opens, and a digit has ink, so a clock is not a blank line.
TEST_CASE("ScrollingTextEffect: built-in fonts open") {
    CHECK(wbf::kWledCount == 5);
    CHECK(wbf::kCount == wbf::kWledCount);
    CHECK(wbf::kFactoryCount == 18);
    for (uint8_t i = 0; i < wbf::kCount; i++) {
        const wbf::Blob* b = wbf::blob(i);
        REQUIRE(b);
        wbf::Font f;
        CHECK(wbf::open(f, b->data, b->bytes));
        CHECK(wbf::textWidth(f, "8") > 0);
    }
}

// The 4x6 bang from the WBF packing note: column 2 lit on rows 0, 1, 2 and 4.
TEST_CASE("ScrollingTextEffect: a .wbf glyph lights the packed pixels") {
    wbf::Font f;
    REQUIRE(wbf::open(f, kBang, sizeof(kBang)));
    CHECK(f.lit(0, 2, 0));
    CHECK(f.lit(0, 2, 1));
    CHECK(f.lit(0, 2, 2));
    CHECK_FALSE(f.lit(0, 2, 3));
    CHECK(f.lit(0, 2, 4));
    CHECK_FALSE(f.lit(0, 1, 0));
}

// A short line draws. An empty line leaves the grid dark. A pinned clock makes #HH:#MM draw digits.
TEST_CASE("ScrollingTextEffect: text draws, a clock token draws, empty leaves the grid dark") {
    Scene s(48, 16);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.text.font = 3;
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.litPixels() > 0);

    s.text.text_[0] = '\0';
    s.layer.tick();
    CHECK(s.litPixels() == 0);

    std::snprintf(s.text.text_, sizeof(s.text.text_), "#HH:#MM");
    s.text.pinClock(sampleClock());
    s.layer.tick();
    CHECK(s.litPixels() > 0);
}

// Brightness scales the palette color, so 0 is a black letter on the black ground.
TEST_CASE("ScrollingTextEffect: brightness 0 leaves the grid dark and 255 draws the letters") {
    Scene s(16, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.text.font = 3;
    s.text.brightness = 0;
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.litPixels() == 0);

    s.text.brightness = 255;
    s.layer.tick();
    CHECK(s.litPixels() > 0);
}

// A black palette paints black letters. Absolute hue paints the wheel color instead.
TEST_CASE("ScrollingTextEffect: absolute hue ignores a black palette") {
    HoldPalette restore;
    Scene s(16, 8);
    drawAB(s);
    CHECK(s.litPixels() == 0);

    s.text.absolute = true;
    s.layer.tick();
    CHECK(paintIs(s, hsvToRgb(0, 255, 255)));
    (void)restore;
}

// A black palette sample uses the wheel when fallback is on. A colored sample stays the palette.
TEST_CASE("ScrollingTextEffect: fallback uses the wheel when the palette is black") {
    HoldPalette restore;
    Scene s(16, 8);
    s.text.fallback = true;
    drawAB(s);
    CHECK(paintIs(s, hsvToRgb(0, 255, 255)));

    Palettes::setActive(0);
    s.text.hue = 8;   // between two rainbow entries, so the blend is not the wheel color at 8
    s.layer.tick();
    const RGB wheel = hsvToRgb(8, 255, 255);
    const RGB pal = colorFromPalette(*Palettes::active(), 8, 255);
    REQUIRE((wheel.r != pal.r || wheel.g != pal.g || wheel.b != pal.b));
    CHECK(paintIs(s, pal));
    (void)restore;
}

// Absolute already paints the wheel, so the fallback switch is read-only and the page greys it.
TEST_CASE("ScrollingTextEffect: fallback is idle while absolute is on") {
    Scene s(4, 8);
    s.text.absolute = true;
    s.text.rebuildControls();
    const ControlDescriptor* fb = nullptr;
    for (uint8_t i = 0; i < s.text.controls().count(); i++)
        if (std::strcmp(s.text.controls()[i].name, "fallback") == 0) fb = &s.text.controls()[i];
    REQUIRE(fb != nullptr);
    CHECK(fb->readonly);

    s.text.absolute = false;
    s.text.rebuildControls();
    fb = nullptr;
    for (uint8_t i = 0; i < s.text.controls().count(); i++)
        if (std::strcmp(s.text.controls()[i].name, "fallback") == 0) fb = &s.text.controls()[i];
    REQUIRE(fb != nullptr);
    CHECK_FALSE(fb->readonly);
}

// An uploaded .wbf replaces the built-in face. The bang's column lands where the packing says.
TEST_CASE("ScrollingTextEffect: an uploaded .wbf font is the face that draws") {
    FsRoot root;
    Scene s(4, 8);
    drawBang(s);
    CHECK(s.litAt(2, 1));
    CHECK(s.litAt(2, 2));
    CHECK(s.litAt(2, 3));
    CHECK_FALSE(s.litAt(2, 4));
    CHECK(s.litAt(2, 5));
    CHECK(s.litPixels() == 4);
    CHECK(s.text.status() == nullptr);
}

// Scale 2 draws the packed column as a 2 by 2 block, and leaves the dark source row dark.
TEST_CASE("ScrollingTextEffect: scale draws each font pixel as a square") {
    FsRoot root;
    Scene s(8, 12);
    s.text.scale = 2;
    drawBang(s);
    CHECK(s.litAt(4, 0));
    CHECK(s.litAt(5, 1));
    CHECK_FALSE(s.litAt(4, 6));
    CHECK_FALSE(s.litAt(5, 7));
    CHECK(s.litAt(4, 8));
    CHECK(s.litAt(5, 9));
    CHECK(s.litPixels() == 16);
}

// The menu lists the file after the built-in faces, and choosing one of those faces draws that face.
TEST_CASE("ScrollingTextEffect: the font menu returns to a built-in face") {
    FsRoot root;
    CHECK(platform::fsWriteAtomic("/moonlive/fonts/bang.wbf", reinterpret_cast<const char*>(kBang), sizeof(kBang)));
    Scene s(4, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "!");
    s.text.defineControls();
    bool listed = false;
    bool byName = false;
    for (uint8_t i = 0; i < s.text.controls().count(); i++) {
        const ControlDescriptor& c = s.text.controls()[i];
        if (std::strcmp(c.name, "font") != 0) continue;
        byName = c.persistLabel;
        auto* opts = reinterpret_cast<const char* const*>(c.aux);
        for (int32_t o = 0; o < c.max; o++)
            if (opts[o] && std::strcmp(opts[o], "bang.wbf") == 0) listed = true;
    }
    CHECK(listed);
    CHECK(byName);

    s.text.font = wbf::kCount;
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.litPixels() == 4);

    s.text.font = 3;
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.litPixels() != 4);
    CHECK(s.text.status() == nullptr);
}

// A PixelFonts face is a name in the menu before its file is on the device. Choosing it does not draw that file.
TEST_CASE("ScrollingTextEffect: a PixelFonts face is offered before it is installed") {
    FsRoot root;
    Scene s(4, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "!");
    s.text.defineControls();
    int idx = -1;
    for (uint8_t i = 0; i < s.text.controls().count(); i++) {
        const ControlDescriptor& c = s.text.controls()[i];
        if (std::strcmp(c.name, "font") != 0) continue;
        auto* opts = reinterpret_cast<const char* const*>(c.aux);
        for (int32_t o = 0; o < c.max; o++)
            if (opts[o] && std::strcmp(opts[o], "pixelfont_5x5_thin.wbf") == 0) idx = o;
    }
    REQUIRE(idx > wbf::kCount);
    s.text.font = static_cast<uint8_t>(idx);
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.text.status() != nullptr);
}

// The trail is the previous color. A new palette drops it in that frame. A script rewriting entries does not.
TEST_CASE("ScrollingTextEffect: a palette change drops the trail and a script rewrite keeps it") {
    HoldPalette restore;

    Palettes::setActive(0);
    Scene s(16, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.text.font = 3;
    s.text.trail = 255;
    s.text.brightness = 255;
    s.layer.applyState();
    s.layer.tick();
    REQUIRE(s.litPixels() > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    s.text.text_[0] = '\0';
    s.layer.tick();
    REQUIRE(s.litPixels() > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Palettes::setActiveDirect(*Palettes::active());
    s.layer.tick();
    CHECK(s.litPixels() > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Palettes::setActive(2);
    s.layer.tick();
    CHECK(s.litPixels() == 0);
    (void)restore;
}
