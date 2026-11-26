#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/builtin_fonts.h"   // the WLED faces, and the PixelFonts names a browser can install
#include "light/powerfunctions/clock_text.h"      // #HH:#MM and the other clock tokens
#include "light/powerfunctions/wbf.h"             // a WLED bitmap font, opened and measured
#include "platform/platform_fs.h"                 // the uploaded .wbf font

#include <cstdio>
#include <cstring>

namespace mm {

/// Scrolls one line of text, and reads the clock where the text writes a # token.
/// @card ScrollingTextEffect.gif
/// @attribution WLED, PixelFonts, raster-fonts
///
/// The line enters from the right and leaves at the left. A line that fits the grid stays centered. With `absolute` off, the letters take their color from the active palette at `hue`, scaled by `brightness`. With it on, `hue` is a position on the color wheel and `brightness` is that color's value. With `fallback` on, a black palette color is that wheel color.
class ScrollingTextEffect : public EffectBase {
public:
    /// Catalog tags: written here, the motion taken from WLED.
    const char* tags() const override { return "💫🐙"; }
    /// Draws on the z=0 plane, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The line to show. A # token reads the clock, so #HH:#MM is the time.
    /// @guide `#HH` `#MM` `#SS` are the hour, minute and second, always two digits. `#DD` is the day, `#MO` the month number, `#YY` and `#YYYY` the year. `#MON` and `#MONL` are the short and full month, `#DAY` and `#DDDD` the weekday. `#TIME` is `HH:MM:SS`, `#HHMM` is `HH:MM`, `#DATE` is `DD.MM.YYYY`, `#DDMM` and `#MMDD` the short dates. A `0` after a token is accepted and changes nothing. With `ampm` on, `#HH` runs from 01 to 12 and `#TIME` ends in AM or PM. `#MM` is the minute, not the month.
    char text_[128] = "#HH:#MM";
    /// 12-hour clock, and AM or PM on #TIME.
    bool ampm = false;
    /// Palette index, or a color-wheel position when `absolute` is on.
    uint8_t hue = 128;
    /// How bright the letters are, from black up to the chosen color.
    uint8_t brightness = 255;
    /// Hue is a wheel position, and brightness its value, instead of a palette index.
    bool absolute = false;
    /// When the palette color is black, hue is a wheel position.
    bool fallback = false;
    /// How fast the line travels, in pixels a second.
    uint8_t speed = 40;
    /// Vertical shift. The middle leaves the line centered.
    uint8_t offset = 128;
    /// How long the letters linger. 0 redraws them clean.
    uint8_t trail = 0;
    /// A built-in face, or a .wbf file from /moonlive/fonts.
    uint8_t font = 3;   // Console 6x8: readable on a small matrix, and in every image
    /// Each font pixel covers this many lights on a side.
    uint8_t scale = 1;
    /// On sends the line toward the right.
    bool reverse = false;
    /// Color the letters from the palette along their width.
    bool gradient = false;

    /// Where a scrolling line's left edge sits. A line that fits stays centered.
    static int originX(int cols, int textW, uint32_t elapsedMs, uint8_t speed, bool reverse) {
        if (cols <= 0) return 0;
        if (textW <= cols) return (cols - textW) / 2;
        const int span = textW + cols;
        if (span <= 0 || speed == 0) return cols;
        const uint32_t off = static_cast<uint32_t>(
            (static_cast<uint64_t>(elapsedMs) * speed / 1000u) % static_cast<uint32_t>(span));
        const int step = reverse ? span - 1 - static_cast<int>(off) : static_cast<int>(off);
        return cols - step;
    }

    /// The top row of the line. `offset` 128 is centered, and either end shifts by half the grid.
    static int originY(int rows, int fontH, uint8_t offset) {
        const int shift = (static_cast<int>(offset) - 128) * rows / 128;
        return (rows - fontH) / 2 + shift;
    }

    /// Tests pin the clock the tokens read. The device clock is what a running effect uses.
    void pinClock(Civil clock) { pinned_ = clock; usePin_ = true; }

    /// Publish the line, its color, the motion and the font.
    void defineControls() override {
        controls_.addTextArea("text", text_, sizeof(text_));
        controls_.addControl("ampm", ampm);
        controls_.addControl("hue", hue, 0, 255);
        controls_.addControl("brightness", brightness, 0, 255);
        controls_.addControl("absolute", absolute);
        controls_.addControl("fallback", fallback);
        controls_.setReadOnly(controls_.count() - 1, absolute);   // absolute already paints the wheel, so the fallback is idle
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("offset", offset, 0, 255);
        controls_.addControl("trail", trail, 0, 255);
        // ONE menu for both kinds, as the palette menu is: a .wbf is chosen like a built-in, and the two cannot disagree.
        if (!fontsScanned_) refreshFonts();
        controls_.addSelect("font", font, options_, optionCount_);
        controls_.setPersistLabel(controls_.count() - 1);   // by name, so a file added above the choice does not move it
        controls_.setDefault(controls_.count() - 1, 3);
        controls_.addControl("scale", scale, 1, 16);
        controls_.addControl("reverse", reverse);
        controls_.addControl("gradient", gradient);
    }

    /// A new face is a different bitmap, so the next frame loads it before drawing.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "font") == 0;
    }

    /// Open the face the menu names.
    void prepare() override { loadFace(); }

    /// Hue, brightness or the absolute switch: the next frame replaces the trail instead of fading it.
    void onControlChanged(const char* controlName) override {
        if (!controlName) return;
        if (std::strcmp(controlName, "hue") == 0 || std::strcmp(controlName, "brightness") == 0
            || std::strcmp(controlName, "absolute") == 0 || std::strcmp(controlName, "fallback") == 0) recolor_ = true;
    }

    /// A font added or removed joins the menu. Any other file leaves it.
    void onFileChanged(const char* path) override {
        if (!inFontDir(path)) return;
        fontsScanned_ = false;
        refreshFonts();
        loadFace();
        rebuildControls();
    }

    /// Draw the line, scrolled by elapsed time so the speed holds at any frame rate.
    void tick() MM_NONBLOCKING override {
        framePal_ = *Palettes::active();   // one palette for the whole line, taken after this frame installed it
        if (!face_.ok) openBuiltin(font < wbf::kCount ? font : 3, false);
        const draw::Canvas cv = canvas();
        const int cols = width();
        const int rows = height();
        clearFrame(cv);
        if (!face_.ok || cols <= 0 || rows <= 0) return;

        char shown[256];
        // A line with no token never reads the clock: localtime takes a lock, and a small CPU feels it every frame.
        const bool timed = usePin_ || std::strchr(text_, '#') != nullptr;
        const Civil clock = !timed ? Civil{} : usePin_ ? pinned_ : civilNow();
        expandClock(shown, sizeof(shown), text_, clock, ampm);
        if (!shown[0]) return;

        const int px = scale < 1 ? 1 : static_cast<int>(scale);   // a stored 0 is the packed face
        const int textW = wbf::textWidth(face_, shown) * px;
        int x = originX(cols, textW, elapsed(), speed, reverse);
        const int y = originY(rows, face_.height * px, offset);
        const char* p = shown;
        int glyphs = 0;
        while (*p) {
            uint32_t cp = wbf::nextCode(p);
            if (cp == '\n') cp = ' ';
            const int idx = face_.index(cp);
            if (idx < 0) continue;
            if (glyphs) x += face_.spacing * px;
            const int gw = face_.widthAt(idx) * px;
            if (x < cols && x + gw > 0) paintGlyph(cv, idx, x, y, px);
            x += gw;
            ++glyphs;
        }
    }

private:
    /// Open built-in face `index`. `clear` drops a status the open itself did not set.
    void openBuiltin(uint8_t index, bool clear) {
        const uint8_t i = index < wbf::kCount ? index : 0;
        const wbf::Blob* b = wbf::blob(i);
        if (!b || !wbf::open(face_, b->data, b->bytes)) setStatus("built-in font failed to open");
        else if (clear) clearStatus();
    }

    /// Read one uploaded face into the scratch buffer.
    bool openFile(uint8_t fileIndex) {
        if (fileIndex >= fileCount_) return false;
        char path[96];
        std::snprintf(path, sizeof(path), "%s/%s", kFontDir, fileNames_[fileIndex]);
        const int64_t sz = platform::fsSize(path);
        if (sz < 12 || sz > 32768 || !file_.resize(static_cast<size_t>(sz) + 1)) return false;
        const int n = platform::fsRead(path, reinterpret_cast<char*>(file_.data()), file_.count());
        return n == sz && wbf::open(face_, file_.data(), static_cast<size_t>(n));
    }

    /// Open the menu's face. A file that will not open draws the console face and says so.
    void loadFace() {
        if (!fontsScanned_) refreshFonts();
        face_ = {};
        if (font < wbf::kCount) { openBuiltin(font, true); return; }
        if (openFile(static_cast<uint8_t>(font - wbf::kCount))) { clearStatus(); return; }
        setStatus("custom font is missing or not a .wbf file");
        openBuiltin(3, false);
    }

    /// True when `path` is the font folder or a file inside it.
    static bool inFontDir(const char* path) {
        if (!path) return false;
        constexpr size_t n = sizeof(kFontDir) - 1;
        if (std::strncmp(path, kFontDir, n) != 0) return false;
        return path[n] == '\0' || path[n] == '/';
    }

    /// Keep a `.wbf` name that fits the menu. A longer name is not a choice the saved label can match.
    void takeFile(const char* name) {
        if (!name || fileCount_ >= kMaxFiles) return;
        const size_t n = std::strlen(name);
        if (n < 5 || n >= sizeof(fileNames_[0])) return;
        if (std::strcmp(name + n - 4, ".wbf") != 0) return;
        for (uint8_t i = 0; i < fileCount_; ++i)
            if (std::strcmp(fileNames_[i], name) == 0) return;
        std::memcpy(fileNames_[fileCount_], name, n);
        fileNames_[fileCount_][n] = '\0';
        ++fileCount_;
    }

    /// Alphabetical, so the menu does not jump when the directory is listed in another order.
    void sortFiles() {
        for (uint8_t i = 1; i < fileCount_; ++i)
            for (uint8_t j = i; j > 0 && std::strcmp(fileNames_[j], fileNames_[j - 1]) < 0; --j) {
                char tmp[sizeof(fileNames_[0])];
                std::memcpy(tmp, fileNames_[j], sizeof(tmp));
                std::memcpy(fileNames_[j], fileNames_[j - 1], sizeof(tmp));
                std::memcpy(fileNames_[j - 1], tmp, sizeof(tmp));
            }
    }

    /// Built-in names, then the files. The pointers stay valid until the next scan.
    void publishOptions() {
        for (uint8_t i = 0; i < wbf::kCount; ++i) options_[i] = wbf::kNames[i];
        for (uint8_t i = 0; i < fileCount_; ++i) options_[wbf::kCount + i] = fileNames_[i];
        optionCount_ = static_cast<uint8_t>(wbf::kCount + fileCount_);
    }

    /// The name the menu currently shows, copied out before a scan rewrites the table.
    void remember(char* keep, size_t cap) const {
        const char* name = nullptr;
        if (font < optionCount_) name = options_[font];
        else if (font < wbf::kCount) name = wbf::kNames[font];
        if (name) std::snprintf(keep, cap, "%s", name);
    }

    /// Put `font` back on `keep` after a scan. A name that left the folder returns to the console face.
    void bindChoice(const char* keep) {
        if (keep && keep[0]) {
            for (uint8_t i = 0; i < optionCount_; ++i)
                if (options_[i] && std::strcmp(options_[i], keep) == 0) { font = i; return; }
            font = 3;
            return;
        }
        if (font >= optionCount_) font = 3;
    }

    /// List the installable faces, then whatever else is already in /moonlive/fonts, and keep the current choice on the same name.
    void refreshFonts() {
        char keep[sizeof(fileNames_[0])] = {};
        remember(keep, sizeof(keep));
        fileCount_ = 0;
        for (uint8_t i = 0; i < wbf::kFactoryCount; ++i) takeFile(wbf::kFactoryFonts[i]);
        platform::fsList(kFontDir, [](const char* name, bool isDir, uint32_t, void* user) {
            if (!isDir) static_cast<ScrollingTextEffect*>(user)->takeFile(name);
        }, this);
        sortFiles();
        publishOptions();
        bindChoice(keep);
        fontsScanned_ = true;
    }

    /// Black, or the trail's decay. A new palette drops the trail, so the old ink does not follow the new color.
    void clearFrame(const draw::Canvas& cv) {
        const uint32_t gen = Palettes::generation();
        const bool replace = gen != paletteGen_ || recolor_;
        paletteGen_ = gen;
        recolor_ = false;
        if (!replace && trail != 0 && frameMs() != 0) {
            draw::decay(cv, static_cast<uint32_t>(trail) * 8u, frameMs());
            return;
        }
        if (cv.data && cv.cpl == 3) std::memset(cv.data, 0, cv.bytes);
        else draw::fill(cv, {0, 0, 0});
    }

    /// True when the palette sample at `idx` is black before brightness scales it.
    bool paletteBlack(uint8_t idx) const {
        const RGB c = colorFromPalette(framePal_, idx, 255);
        return c.r == 0 && c.g == 0 && c.b == 0;
    }

    /// The color of palette index `idx`, the wheel when `absolute` is on, or the wheel when `fallback` meets black.
    RGB ink(uint8_t idx) const {
        if (absolute || (fallback && paletteBlack(idx))) return hsvToRgb(idx, 255, brightness);
        return colorFromPalette(framePal_, idx, brightness);
    }

    /// The part of a rectangle that lies on the grid. `ok` is false when nothing remains.
    struct Clip { int x, y, w, h; bool ok; };
    static Clip clip(int x, int y, int w, int h, int cols, int rows) {
        if (w <= 0 || h <= 0) return {};
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (w <= 0 || h <= 0 || x >= cols || y >= rows) return {};
        if (x + w > cols) w = cols - x;
        if (y + h > rows) h = rows - y;
        return {x, y, w, h, true};
    }

    /// Write `w` lights of `c` along one row. A channel past RGB stays as the clear left it.
    static void writeRow(uint8_t* p, int w, uint8_t cpl, RGB c) {
        for (int i = 0; i < w; ++i, p += cpl) {
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
        }
    }

    /// Copy a painted row onto the rows under it. Three channels pack tight, so the copy is the row itself.
    static void copyRows(uint8_t* first, int h, size_t stride, size_t span, const uint8_t* end) {
        for (int r = 1; r < h; ++r) {
            uint8_t* dst = first + static_cast<size_t>(r) * stride;
            if (dst + span > end) return;
            std::memcpy(dst, first, span);
        }
    }

    /// A solid rectangle. The first row is written, and the rows under it are that row copied.
    void stamp(const draw::Canvas& cv, int x, int y, int w, int h, RGB c) const {
        const uint8_t cpl = cv.cpl;
        if (!cv.data || cpl < 3) return;
        const Clip r = clip(x, y, w, h, cv.dims.x, cv.dims.y);
        if (!r.ok) return;
        const size_t stride = static_cast<size_t>(cv.dims.x) * cpl;
        uint8_t* first = cv.data + (static_cast<size_t>(r.y) * static_cast<size_t>(cv.dims.x) + static_cast<size_t>(r.x)) * cpl;
        const size_t span = static_cast<size_t>(r.w) * cpl;
        const uint8_t* end = cv.data + cv.bytes;
        if (first + span > end) return;
        writeRow(first, r.w, cpl, c);
        if (cpl == 3) copyRows(first, r.h, stride, span, end);
        else for (int row = 1; row < r.h; ++row) writeRow(first + static_cast<size_t>(row) * stride, r.w, cpl, c);
    }

    /// Source columns whose blocks meet the grid. `b` is one past the last.
    struct Span { int a = 0; int b = 0; };

    /// Columns of a glyph whose scaled blocks meet `[0, cols)`.
    static Span colsOnScreen(int x, int gw, int px, int cols) {
        if (px <= 0 || gw <= 0 || x >= cols || x + gw * px <= 0) return {};
        const int a = x < 0 ? (-x) / px : 0;
        if (a >= gw) return {};
        if (x + gw * px <= cols) return {a, gw};
        int b = (cols - x + px - 1) / px;
        if (b > gw) b = gw;
        if (b < a) return {};
        return {a, b};
    }

    /// One source row. A solid face stamps each run of ink; a gradient stamps each source pixel.
    void paintRow(const draw::Canvas& cv, int glyph, int row, int x, int dy, int px, Span cols) const {
        const RGB solid = gradient ? RGB{} : ink(hue);
        int col = cols.a;
        while (col < cols.b) {
            if (!face_.lit(glyph, col, row)) { ++col; continue; }
            if (gradient) {
                const uint8_t idx = static_cast<uint8_t>(hue + col * 16 + row * 8);
                stamp(cv, x + col * px, dy, px, px, ink(idx));
                ++col;
                continue;
            }
            const int start = col;
            while (++col < cols.b && face_.lit(glyph, col, row)) {}
            stamp(cv, x + start * px, dy, (col - start) * px, px, solid);
        }
    }

    /// One glyph. Columns off the grid are not visited, and a run of ink is one rectangle.
    void paintGlyph(const draw::Canvas& cv, int glyph, int x, int y, int px) const {
        const Span cols = colsOnScreen(x, face_.widthAt(glyph), px, cv.dims.x);
        if (cols.a >= cols.b) return;
        const int rows = cv.dims.y;
        if (y >= rows || y + face_.height * px <= 0) return;
        for (int row = 0; row < face_.height; ++row) {
            const int dy = y + row * px;
            if (dy + px <= 0) continue;
            if (dy >= rows) break;
            paintRow(cv, glyph, row, x, dy, px, cols);
        }
    }

    /// Uploaded faces live here: the file manager creates folders under /moonlive.
    static constexpr char kFontDir[] = "/moonlive/fonts";
    /// How many file rows the menu holds: the installable faces, then files a user added.
    static constexpr uint8_t kMaxFiles = 32;

    Civil pinned_{};
    bool usePin_ = false;
    bool recolor_ = false;          ///< the ink controls changed, so the trail is stale
    uint32_t paletteGen_ = 0;       ///< the palette generation the trail was painted with
    Palette framePal_{};            ///< the palette this frame paints, copied once
    bool fontsScanned_ = false;
    uint8_t fileCount_ = 0;
    uint8_t optionCount_ = 0;
    char fileNames_[kMaxFiles][64] = {};
    const char* options_[wbf::kCount + kMaxFiles] = {};
    wbf::Font face_{};
    ScratchBuffer<uint8_t> file_{*this};
};

}  // namespace mm
