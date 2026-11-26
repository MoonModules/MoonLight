/// @module TextEffect

#include "doctest.h"
#include "light/effects/TextEffect.h"
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
// Build a real w×h×1 Layer for the effect to render into (the MetaballsEffect harness).
struct Scene {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    TextEffect text;
    Scene(int w, int h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&text);
    }
    int litPixels() {
        auto& b = layer.buffer();
        int n = 0;
        for (size_t i = 0; i + 2 < b.bytes(); i += 3)
            if (b.data()[i] || b.data()[i+1] || b.data()[i+2]) n++;
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
    FsRoot() : prev(platform::fsRootPath() ? platform::fsRootPath() : ""), dir(uniqueTempDir("textfont")) {
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

/// Puts the palette back when the case ends.
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

int fontIndex(TextEffect& text, const char* name) {
    for (uint8_t i = 0; i < text.controls().count(); i++) {
        const ControlDescriptor& c = text.controls()[i];
        if (std::strcmp(c.name, "font") != 0) continue;
        auto* opts = reinterpret_cast<const char* const*>(c.aux);
        for (int32_t o = 0; o < c.max; o++)
            if (opts[o] && std::strcmp(opts[o], name) == 0) return o;
    }
    return -1;
}

Civil sampleClock() {
    Civil c;
    c.year = 2026; c.month = 11; c.day = 26;
    c.hour = 14; c.minute = 5; c.second = 9;
    c.weekday = 4;
    return c;
}
}  // namespace

// Static text renders glyph pixels top-left. On a grid tall enough for one line of the 4x6 font, a non-empty string lights some pixels; an empty string lights none.
TEST_CASE("TextEffect: static text draws glyph pixels, empty draws nothing") {
    Scene s(48, 8);
    s.text.scroll = false;
    s.text.font = 0;                 // 4x6
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.litPixels() > 0);        // "AB" drew something

    s.text.text_[0] = '\0';          // empty string → blank frame (snprintf with "" is a zero-length
                                     // format, which GCC rejects; clearing the buffer says it plainly)
    s.layer.tick();
    CHECK(s.litPixels() == 0);
}

// The hue default is 128 (mid-palette), not 0: palette index 0 is BLACK in several palettes, so a hue-0 default would render invisible text on those. Pinning the default guards against a silent regression back to 0.
TEST_CASE("TextEffect: default hue is mid-palette (128), not black-at-0") {
    TextEffect text;
    CHECK(text.hue == 128);
}

// A multi-line string wraps: the second line renders on a lower row (font-height down), so a two-line string lights pixels below the first font's height. Uses the 4x6 font (height 6).
TEST_CASE("TextEffect: newline wraps to a second row") {
    Scene s(24, 16);
    s.text.scroll = false;
    s.text.font = 0;                 // 4x6, height 6
    std::snprintf(s.text.text_, sizeof(s.text.text_), "A\nB");
    s.layer.applyState();
    s.layer.tick();

    auto& b = s.layer.buffer();
    const int w = 24;
    auto rowLit = [&](int y) {
        for (int x = 0; x < w; x++) { size_t o = (static_cast<size_t>(y) * w + x) * 3;
            if (b.data()[o] || b.data()[o+1] || b.data()[o+2]) return true; }
        return false;
    };
    bool topLine = false, secondLine = false;
    for (int y = 0; y < 6; y++)  if (rowLit(y)) topLine = true;      // line 1 in the top font cell
    for (int y = 6; y < 12; y++) if (rowLit(y)) secondLine = true;   // line 2 one font-height down
    CHECK(topLine);
    CHECK(secondLine);
}

// Scroll mode advances the text over time and never crashes; on a degenerate grid it's a safe no-op.
TEST_CASE("TextEffect: scroll animates and is safe at any size") {
    Scene s(16, 8);
    s.text.scroll = true;
    std::snprintf(s.text.text_, sizeof(s.text.text_), "MoonModules");
    s.layer.applyState();
    for (int i = 0; i < 5; i++) s.layer.tick();   // several frames — must not crash
    // 1×1 and 0×0 must not crash either.
    Scene tiny(1, 1); tiny.text.scroll = true; std::snprintf(tiny.text.text_, sizeof(tiny.text.text_), "X");
    tiny.layer.applyState(); tiny.layer.tick();
    Scene zero(0, 0); zero.text.scroll = true; std::snprintf(zero.text.text_, sizeof(zero.text.text_), "X");
    zero.layer.applyState(); zero.layer.tick();
    CHECK(true);   // reaching here without a crash is the assertion
}

// #HH:#MM is the hour and the minute, zero-padded. #MM is the minute, #MO the month.
TEST_CASE("TextEffect: #HH:#MM expands to the zero-padded clock") {
    char out[64];
    const Civil c = sampleClock();
    expandClock(out, sizeof(out), "#HH:#MM", c, false);
    CHECK(std::string(out) == "14:05");
    expandClock(out, sizeof(out), "#HH0:#MM", c, false);
    CHECK(std::string(out) == "14:05");

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
TEST_CASE("TextEffect: scroll position follows elapsed time") {
    CHECK(TextEffect::originX(16, 8, 0, 40, false) == 4);
    CHECK(TextEffect::originX(16, 40, 0, 40, false) == 16);
    CHECK(TextEffect::originX(16, 40, 1000, 10, false) == 6);
    CHECK(TextEffect::originX(16, 40, 0, 40, true) == 1 - 40);
    CHECK(TextEffect::originY(16, 8, 128) == 4);
    CHECK(TextEffect::originY(16, 8, 0) == 0);
}

// The 4x6 bang from the WBF packing note: column 2 lit on rows 0, 1, 2 and 4.
TEST_CASE("TextEffect: a .wbf glyph lights the packed pixels") {
    wbf::Font f;
    REQUIRE(wbf::open(f, kBang, sizeof(kBang)));
    CHECK(f.lit(0, 2, 0));
    CHECK(f.lit(0, 2, 1));
    CHECK(f.lit(0, 2, 2));
    CHECK_FALSE(f.lit(0, 2, 3));
    CHECK(f.lit(0, 2, 4));
    CHECK_FALSE(f.lit(0, 1, 0));
}

// A pinned clock makes #HH:#MM draw digits on the built-in face.
TEST_CASE("TextEffect: a clock token draws") {
    Scene s(48, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "#HH:#MM");
    s.text.font = 0;
    s.text.pinClock(sampleClock());
    s.layer.applyState();
    s.layer.tick();
    CHECK(s.litPixels() > 0);
}

// A one-color palette is the letter color, including when hue is 0.
TEST_CASE("TextEffect: a one-color palette is the letter color") {
    HoldPalette restore;
    Palette red;
    for (auto& e : red.entry) e = RGB{255, 0, 0};
    Palettes::setActiveDirect(red);
    Scene s(24, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.text.font = 0;
    s.text.hue = 0;
    s.layer.applyState();
    s.layer.tick();
    CHECK(paintIs(s, RGB{255, 0, 0}));
    (void)restore;
}

// Scale 2 covers four lights for each lit font pixel.
TEST_CASE("TextEffect: scale draws each font pixel as a square") {
    Scene s(48, 32);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "A");
    s.text.font = 0;
    s.text.scale = 1;
    s.layer.applyState();
    s.layer.tick();
    const int one = s.litPixels();
    s.text.scale = 2;
    s.layer.tick();
    CHECK(one > 0);
    CHECK(s.litPixels() == one * 4);
}

// The menu lists the files in the font folder, and a name that is not there is not a row.
TEST_CASE("TextEffect: the font menu lists /moonlive/fonts") {
    FsRoot root;
    Scene s(16, 8);
    s.text.defineControls();
    CHECK(fontIndex(s.text, "4x6") == 0);
    CHECK(fontIndex(s.text, "pixelfont_5x5_thin.wbf") < 0);
    CHECK(platform::fsWriteAtomic("/moonlive/fonts/bang.wbf", reinterpret_cast<const char*>(kBang), sizeof(kBang)));
    s.text.onFileChanged("/moonlive/fonts/bang.wbf");
    const int bang = fontIndex(s.text, "bang.wbf");
    REQUIRE(bang > 0);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "!");
    s.text.font = static_cast<uint8_t>(bang);
    s.layer.applyState();
    s.layer.tick();
    // The 4x6 bang lights this same pixel, so a clear status is what shows the file opened.
    CHECK(s.text.status() == nullptr);
    CHECK(s.litAt(2, 0));
    (void)root;
}

// The trail keeps the previous letters while they fade, and a palette change leaves that fade in place.
TEST_CASE("TextEffect: a trail fades across a palette change") {
    HoldPalette restore;
    Scene s(24, 8);
    std::snprintf(s.text.text_, sizeof(s.text.text_), "AB");
    s.text.font = 0;
    s.text.trail = 255;
    s.layer.applyState();
    s.layer.tick();
    REQUIRE(s.litPixels() > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    s.text.text_[0] = '\0';
    s.layer.tick();
    REQUIRE(s.litPixels() > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    Palettes::setActive(1);
    s.layer.tick();
    CHECK(s.litPixels() > 0);
    (void)restore;
}
