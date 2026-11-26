#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/clock_text.h"   // #HH:#MM and the other clock tokens
#include "light/powerfunctions/wbf.h"          // a .wbf face, opened and measured
#include "platform/platform_fs.h"              // the font folder

#include <cstdio>
#include <cstring>

namespace mm {

/// Renders a multi-line string in a bitmap font, static by default or as a wrapping marquee.
/// @card TextEffect.gif
/// @attribution raster-fonts, WLED, PixelFonts
///
/// Static text sits at the top left, and a newline drops one font height. `scroll` marches a line wider than the grid, and a shorter line stays centered. A `#` token reads the clock. The 4x6 face is built in, and every other face is a `.wbf` file in `/moonlive/fonts`. Letters take the active palette at `hue`.
class TextEffect : public EffectBase {
public:
    /// Catalog tags: MoonLight, with the clock tokens and the font file taken from WLED.
    const char* tags() const override { return "💫🐙"; }
    /// Draws on the z=0 plane, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The string to show. A # token reads the clock, so #HH:#MM is the time.
    /// @guide `#HH` `#MM` `#SS` are the hour, minute and second, always two digits. `#DD` is the day, `#MO` the month number, `#YY` and `#YYYY` the year. `#MON` and `#MONL` are the short and full month, `#DAY` and `#DDDD` the weekday. `#TIME` is `HH:MM:SS`, `#HHMM` is `HH:MM`, `#DATE` is `DD.MM.YYYY`, `#DDMM` and `#MMDD` the short dates. A `0` after a token is accepted. With `ampm` on, `#HH` runs from 01 to 12 and `#TIME` ends in AM or PM. `#MM` is the minute, not the month.
    char text_[128] = "MoonLight";
    /// 12-hour clock, and AM or PM on #TIME.
    bool ampm = false;
    /// Off is static, on a horizontal marquee.
    bool scroll = false;
    /// The 4x6 face, or a .wbf file in /moonlive/fonts.
    uint8_t font = 0;
    /// How fast a scrolling line travels, in pixels a second.
    uint8_t speed = 30;
    /// Vertical shift. The middle leaves the line centered.
    uint8_t offset = 0;
    /// Each font pixel covers this many lights on a side.
    uint8_t scale = 1;
    /// Palette index of the letter color. A one-color palette sets it.
    uint8_t hue = 128;
    /// How long the letters linger. 0 redraws them clean.
    uint8_t trail = 0;
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

    /// The top row. Offset 0 is the top of the grid, and 128 centers the line.
    static int originY(int rows, int fontH, uint8_t offset) {
        if (rows <= fontH) return 0;
        return (rows - fontH) * static_cast<int>(offset) / 256;
    }

    /// Tests pin the clock the tokens read. The device clock is what a running effect uses.
    void pinClock(Civil clock) { pinned_ = clock; usePin_ = true; }

    /// Publish the line, the motion, the font and the color.
    void defineControls() override {
        if (!fontsScanned_ || !names_) refreshFonts();
        controls_.addTextArea("text", text_, sizeof(text_));
        controls_.addControl("ampm", ampm);
        controls_.addControl("scroll", scroll);
        controls_.addSelect("font", font, options_, optionCount_);
        controls_.setPersistLabel(controls_.count() - 1);   // by name, so a file added above the choice does not move it
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("offset", offset, 0, 255);
        controls_.addControl("scale", scale, 1, 16);
        controls_.addControl("hue", hue, 0, 255);
        controls_.addControl("trail", trail, 0, 255);
        controls_.addControl("reverse", reverse);
        controls_.addControl("gradient", gradient);
    }

    /// A new face is a different bitmap, so the next frame loads it before drawing.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "font") == 0; }

    /// Open the face the menu names.
    void prepare() override { loadFace(); }

    /// The name table and a loaded file are scratch, freed by the base. The scan runs again on the next prepare.
    void release() override {
        fontsScanned_ = false;
        fileCount_ = 0;
        face_ = {};
        EffectBase::release();
    }

    /// A font added or removed joins the menu. Any other file leaves it.
    void onFileChanged(const char* path) override {
        if (!inFontDir(path)) return;
        fontsScanned_ = false;
        refreshFonts();
        loadFace();
        rebuildControls();
    }

    /// Draw the text. A line with no # token leaves the clock unread.
    void tick() MM_NONBLOCKING override {
        framePal_ = *Palettes::active();
        const draw::Canvas cv = canvas();
        fade(cv);
        if (cv.dims.x <= 0 || cv.dims.y <= 0 || !text_[0]) return;
        char shown[256];
        paint(cv, shownLine(shown, sizeof(shown)));
    }

private:
    /// The face `paint` reads. A file face borrows `face_`. The built-in face is the 4x6.
    struct Face {
        const fonts::Font* cell = nullptr;
        const wbf::Font* file = nullptr;
        int height() const { return file ? file->height : cell->height; }
        int spacing() const { return file ? file->spacing : 0; }
        int indexOf(uint32_t cp) const {
            if (file) return file->index(cp);
            if (cp < 32 || cp > 126) return -1;
            return static_cast<int>(cp) - 32;
        }
        int widthAt(int glyph) const { return file ? file->widthAt(glyph) : cell->width; }
        bool lit(int glyph, int col, int row) const {
            if (file) return file->lit(glyph, col, row);
            const uint8_t* rows = cell->rows + static_cast<size_t>(glyph) * cell->height;
            return (rows[row] >> (7 - col)) & 0x01;
        }
    };

    /// Expand clock tokens when the line has one. Otherwise the text is drawn as it is stored.
    const char* shownLine(char* buf, size_t n) const {
        if (std::strchr(text_, '#') == nullptr) return text_;
        const Civil clock = usePin_ ? pinned_ : civilNow();
        expandClock(buf, n, text_, clock, ampm);
        return buf;
    }

    /// The palette color at `idx`. A black entry is a black letter.
    RGB inkAt(uint8_t idx) const { return colorFromPalette(framePal_, idx); }

    /// Black, or the trail fading toward black. A new palette fades in with the trail.
    void fade(const draw::Canvas& cv) const {
        if (trail != 0 && frameMs() != 0) {
            draw::decay(cv, static_cast<uint32_t>(trail) * 8u, frameMs());
            return;
        }
        draw::fill(cv, {0, 0, 0});
    }

    /// The open face, or the built-in 4x6 when the menu is on that row or the file will not open.
    Face currentFace() const {
        Face face;
        if (font != 0 && face_.ok) face.file = &face_;
        else face.cell = &fonts::kFont4x6;
        return face;
    }

    /// Pixel width of the widest line, in font pixels, spacing included.
    static int widestLine(const Face& face, const char* s) {
        int widest = 0, cur = 0, glyphs = 0;
        const char* p = s ? s : "";
        while (*p) {
            const uint32_t cp = wbf::nextCode(p);
            if (cp == '\n') {
                if (cur > widest) widest = cur;
                cur = 0;
                glyphs = 0;
                continue;
            }
            const int idx = face.indexOf(cp);
            if (idx < 0) continue;
            if (glyphs) cur += face.spacing();
            cur += face.widthAt(idx);
            ++glyphs;
        }
        return cur > widest ? cur : widest;
    }

    /// Draw `s`. Static text starts at the left. A scrolling line that fits stays centered.
    void paint(const draw::Canvas& cv, const char* s) {
        src_ = currentFace();
        const int px = scale < 1 ? 1 : static_cast<int>(scale);
        const int cols = cv.dims.x;
        const int textW = widestLine(src_, s) * px;
        const int x0 = scroll ? originX(cols, textW, elapsed(), speed, reverse) : 0;
        const int lineH = src_.height() * px;
        int y = originY(cv.dims.y, lineH, offset);
        int x = x0;
        int glyphs = 0;
        const char* p = s ? s : "";
        while (*p) {
            const uint32_t cp = wbf::nextCode(p);
            if (cp == '\n') { x = x0; glyphs = 0; y += lineH; continue; }
            const int idx = src_.indexOf(cp);
            if (idx < 0) continue;
            if (glyphs) x += src_.spacing() * px;
            const int gw = src_.widthAt(idx) * px;
            if (x < cols && x + gw > 0) paintGlyph(cv, idx, x, y, px);
            x += gw;
            ++glyphs;
        }
    }

    /// Read one uploaded face into the scratch buffer. `fsRead` reserves its last byte for a NUL, so the buffer is one byte longer than the file.
    bool openFile(uint8_t fileIndex) {
        if (fileIndex >= fileCount_) return false;
        char path[96];
        std::snprintf(path, sizeof(path), "%s/%s", kFontDir, fileName(fileIndex));
        const int64_t sz = platform::fsSize(path);
        if (sz < 12 || sz > 32768 || !file_.resize(static_cast<size_t>(sz) + 1)) return false;
        const int n = platform::fsRead(path, reinterpret_cast<char*>(file_.data()), file_.count());
        return n == sz && wbf::open(face_, file_.data(), static_cast<size_t>(n));
    }

    /// Open the menu's face. A file that will not open draws the 4x6 face and says so.
    void loadFace() {
        if (!fontsScanned_ || !names_) refreshFonts();
        face_ = {};
        if (font == 0) { clearStatus(); return; }
        if (openFile(static_cast<uint8_t>(font - 1))) { clearStatus(); return; }
        setStatus("custom font is missing or is not a .wbf file");
    }

    /// True when `path` is the font folder or a file inside it.
    static bool inFontDir(const char* path) {
        if (!path) return false;
        constexpr size_t n = sizeof(kFontDir) - 1;
        if (std::strncmp(path, kFontDir, n) != 0) return false;
        return path[n] == '\0' || path[n] == '/';
    }

    /// Keep a `.wbf` name that fits the menu.
    void takeFile(const char* name) {
        if (!name || fileCount_ >= kMaxFiles || !names_) return;
        const size_t n = std::strlen(name);
        if (n < 5 || n >= kNameCap) return;
        if (std::strcmp(name + n - 4, ".wbf") != 0) return;
        for (uint8_t i = 0; i < fileCount_; ++i)
            if (std::strcmp(fileName(i), name) == 0) return;
        std::memcpy(fileName(fileCount_), name, n);
        fileName(fileCount_)[n] = '\0';
        ++fileCount_;
    }

    /// Alphabetical, so the menu does not jump when the directory is listed in another order.
    void sortFiles() {
        for (uint8_t i = 1; i < fileCount_; ++i)
            for (uint8_t j = i; j > 0 && std::strcmp(fileName(j), fileName(j - 1)) < 0; --j) {
                char tmp[kNameCap];
                std::memcpy(tmp, fileName(j), kNameCap);
                std::memcpy(fileName(j), fileName(j - 1), kNameCap);
                std::memcpy(fileName(j - 1), tmp, kNameCap);
            }
    }

    /// 4x6, then the files. The pointers stay valid until the next scan.
    void publishOptions() {
        options_[0] = "4x6";
        for (uint8_t i = 0; i < fileCount_; ++i) options_[1 + i] = fileName(i);
        optionCount_ = static_cast<uint8_t>(1 + fileCount_);
    }

    /// The name the menu shows, copied out before a scan rewrites the table.
    void remember(char* keep, size_t cap) const {
        const char* name = font < optionCount_ ? options_[font] : nullptr;
        if (name) std::snprintf(keep, cap, "%s", name);
        else if (cap) keep[0] = '\0';
    }

    /// Put `font` back on `keep` after a scan. A name that left the folder returns to 4x6.
    void bindChoice(const char* keep) {
        if (keep && keep[0]) {
            for (uint8_t i = 0; i < optionCount_; ++i)
                if (options_[i] && std::strcmp(options_[i], keep) == 0) { font = i; return; }
            font = 0;
            return;
        }
        if (font >= optionCount_) font = 0;
    }

    /// List `/moonlive/fonts` and keep the current choice on the same name.
    void refreshFonts() {
        char keep[kNameCap] = {};
        remember(keep, sizeof(keep));
        fileCount_ = 0;
        if (!names_.resize(static_cast<size_t>(kMaxFiles) * kNameCap)) {
            setStatus("out of memory for the font list");
            publishOptions();
            fontsScanned_ = true;
            return;
        }
        std::memset(names_.data(), 0, names_.bytes());
        platform::fsList(kFontDir, [](const char* name, bool isDir, uint32_t, void* user) {
            if (!isDir) static_cast<TextEffect*>(user)->takeFile(name);
        }, this);
        sortFiles();
        publishOptions();
        bindChoice(keep);
        fontsScanned_ = true;
    }

    /// True when the rectangle misses the grid.
    static bool offGrid(int x, int y, int w, int h, int cols, int rows) {
        return w <= 0 || h <= 0 || x >= cols || y >= rows;
    }

    /// The part of a rectangle that lies on the grid. `ok` is false when the rectangle misses the grid.
    struct Clip { int x, y, w, h; bool ok; };
    static Clip clip(int x, int y, int w, int h, int cols, int rows) {
        if (w <= 0 || h <= 0) return {};
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (offGrid(x, y, w, h, cols, rows)) return {};
        if (x + w > cols) w = cols - x;
        if (y + h > rows) h = rows - y;
        return {x, y, w, h, true};
    }

    /// Write `w` lights of `c` along one row.
    static void writeRow(uint8_t* p, int w, uint8_t cpl, RGB c) {
        for (int i = 0; i < w; ++i, p += cpl) {
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
        }
    }

    /// Copy a painted row onto the rows under it.
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

    /// One source row. A solid face stamps each run of ink. A gradient stamps each source pixel.
    void paintRow(const draw::Canvas& cv, int glyph, int row, int x, int dy, int px, Span cols) const {
        const RGB solid = gradient ? RGB{} : inkAt(hue);
        int col = cols.a;
        while (col < cols.b) {
            if (!src_.lit(glyph, col, row)) { ++col; continue; }
            if (gradient) {
                stamp(cv, x + col * px, dy, px, px, inkAt(static_cast<uint8_t>(hue + col * 16 + row * 8)));
                ++col;
                continue;
            }
            const int start = col;
            while (++col < cols.b && src_.lit(glyph, col, row)) {}
            stamp(cv, x + start * px, dy, (col - start) * px, px, solid);
        }
    }

    /// One glyph. Columns off the grid are not visited, and a run of ink is one rectangle.
    void paintGlyph(const draw::Canvas& cv, int glyph, int x, int y, int px) const {
        const Span cols = colsOnScreen(x, src_.widthAt(glyph), px, cv.dims.x);
        if (cols.a >= cols.b) return;
        const int rows = cv.dims.y;
        if (y >= rows || y + src_.height() * px <= 0) return;
        for (int row = 0; row < src_.height(); ++row) {
            const int dy = y + row * px;
            if (dy + px <= 0) continue;
            if (dy >= rows) break;
            paintRow(cv, glyph, row, x, dy, px, cols);
        }
    }

    /// Uploaded faces live here. The file manager creates folders under /moonlive.
    static constexpr char kFontDir[] = "/moonlive/fonts";
    /// How many file rows the menu holds.
    static constexpr uint8_t kMaxFiles = 32;
    /// Bytes kept for one file name, including the terminator.
    static constexpr uint8_t kNameCap = 64;

    /// One stored file name. Valid while `names_` holds the scan.
    char* fileName(uint8_t i) { return names_.data() + static_cast<size_t>(i) * kNameCap; }
    /// One stored file name, for a const menu read.
    const char* fileName(uint8_t i) const { return names_.data() + static_cast<size_t>(i) * kNameCap; }

    Civil pinned_{};
    bool usePin_ = false;
    bool fontsScanned_ = false;
    uint8_t fileCount_ = 0;
    uint8_t optionCount_ = 0;
    const char* options_[1 + kMaxFiles] = {};
    Face src_{};
    Palette framePal_{};
    wbf::Font face_{};
    ScratchBuffer<char> names_{*this};
    ScratchBuffer<uint8_t> file_{*this};
};

static_assert(sizeof(TextEffect) <= 2048, "TextEffect is over the inline budget");

}  // namespace mm
