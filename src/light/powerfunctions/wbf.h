#pragma once

#include <cstddef>
#include <cstdint>

/// A WLED bitmap font: the 12-byte header, an optional width table, then bit-packed glyphs.
///
/// Row-major, the high bit on the left, each glyph padded to a whole byte. The same layout WLED stores in flash and the layout an uploaded `.wbf` file uses, so one reader covers both.
namespace mm::wbf {

/// One opened face. The pointers borrow `data` for as long as that buffer lives.
struct Font {
    /// Borrowed font bytes.
    const uint8_t* data = nullptr;
    /// Byte length of `data`.
    size_t size = 0;
    /// Glyph height in pixels.
    uint8_t height = 0;
    /// Widest glyph, in pixels.
    uint8_t maxWidth = 0;
    /// Gap between glyphs, in pixels.
    uint8_t spacing = 0;
    /// Header flags. Bit 0 marks a variable-width face.
    uint8_t flags = 0;
    /// First code point the table holds.
    uint8_t first = 0;
    /// Last code point the table holds.
    uint8_t last = 0;
    /// How many glyphs the table holds.
    uint16_t count = 0;
    /// Unicode base for code points past ASCII. Zero means the face is ASCII only.
    uint32_t unicode = 0;
    const uint8_t* widths = nullptr;   ///< present when the face is variable-width
    /// True once `open` accepted the buffer.
    bool ok = false;
    mutable int cachedGlyph_ = -1;     ///< the glyph `cachedAt_` was computed for
    mutable uint32_t cachedAt_ = 0;    ///< byte offset of `cachedGlyph_`

    /// Glyph index of a code point, or -1 when the face has no glyph for it.
    int index(uint32_t cp) const {
        if (!ok) return -1;
        if (cp <= 127) {
            if (cp < first || cp > last) return -1;
            return static_cast<int>(cp - first);
        }
        // Past ASCII, the header's unicode base maps the code point onto the same table.
        if (unicode == 0 || cp < unicode) return -1;
        const uint32_t adj = cp - unicode + 128;
        if (adj < first || adj > last) return -1;
        return static_cast<int>(adj - first);
    }

    /// Pixel width of one glyph. A missing index is zero wide.
    uint8_t widthAt(int glyph) const {
        if (glyph < 0 || glyph >= count) return 0;
        if ((flags & 0x01) == 0) return maxWidth;
        return widths[static_cast<size_t>(glyph)];
    }

    /// Byte length of one glyph's packed rows.
    uint32_t bytesAt(int glyph) const {
        const unsigned bits = static_cast<unsigned>(widthAt(glyph)) * height;
        return (bits + 7u) / 8u;
    }

    /// Byte offset of glyph `glyph`. The last answer is kept, so a row of pixels does not walk the face again.
    uint32_t glyphAt(int glyph) const {
        if (glyph == cachedGlyph_) return cachedAt_;
        uint32_t pos = 12;
        if (flags & 0x01) pos += count;
        for (int i = 0; i < glyph; ++i) pos += bytesAt(i);
        cachedGlyph_ = glyph;
        cachedAt_ = pos;
        return pos;
    }

    /// True when `glyph`, `col` and `row` name a pixel this face holds.
    bool pixelIn(int glyph, int col, int row) const {
        if (!ok || glyph < 0 || glyph >= count) return false;
        if (row < 0 || row >= height) return false;
        const int w = widthAt(glyph);
        return col >= 0 && col < w;
    }

    /// Whether the glyph's pixel is ink. Out of range is empty.
    bool lit(int glyph, int col, int row) const {
        if (!pixelIn(glyph, col, row)) return false;
        const uint32_t bit = static_cast<uint32_t>(row) * static_cast<uint32_t>(widthAt(glyph)) + static_cast<uint32_t>(col);
        const uint32_t base = glyphAt(glyph);
        const uint8_t* p = data + base + (bit >> 3);
        if (p >= data + base + bytesAt(glyph) || p >= data + size) return false;
        return ((*p) >> (7 - (bit & 7))) & 0x01;
    }
};

/// Height and width inside the reader's limit, one to 64 pixels.
inline bool dimsOk(uint8_t height, uint8_t width) {
    return height >= 1 && height <= 64 && width >= 1 && width <= 64;
}

/// Read the 12-byte header into `f`. False on a short buffer, a bad magic or a count the reader refuses.
inline bool readHeader(Font& f, const uint8_t* data, size_t n) {
    if (!data || n < 12 || data[0] != 'W') return false;
    f.data = data;
    f.size = n;
    f.height = data[1];
    f.maxWidth = data[2];
    f.spacing = data[3];
    f.flags = data[4];
    f.first = data[5];
    f.last = data[6];
    f.unicode = static_cast<uint32_t>(data[8])
              | (static_cast<uint32_t>(data[9]) << 8)
              | (static_cast<uint32_t>(data[10]) << 16)
              | (static_cast<uint32_t>(data[11]) << 24);
    if (!dimsOk(f.height, f.maxWidth) || f.last < f.first) return false;
    const int count = static_cast<int>(f.last) - static_cast<int>(f.first) + 1;
    if (count <= 0 || count > 256) return false;
    f.count = static_cast<uint16_t>(count);
    return true;
}

/// Check each glyph fits. False when one runs off the end or a width exceeds the header.
inline bool placeGlyphs(Font& f, size_t n) {
    size_t pos = 12;
    if (f.flags & 0x01) {
        if (pos + f.count > n) return false;
        f.widths = f.data + pos;
        for (uint16_t i = 0; i < f.count; i++)
            if (f.widths[i] > f.maxWidth) return false;
        pos += f.count;
    }
    for (uint16_t i = 0; i < f.count; i++) {
        pos += f.bytesAt(i);
        if (pos > n) return false;
    }
    return true;
}

/// Open a face over `data`. False leaves `f` closed: a short buffer, a bad header, or a glyph that runs off the end.
inline bool open(Font& f, const uint8_t* data, size_t n) {
    f = {};
    if (!readHeader(f, data, n) || !placeGlyphs(f, n)) { f = {}; return false; }
    f.ok = true;
    return true;
}

/// The next UTF-8 code point, advancing `p`. A broken sequence yields its first byte.
inline uint32_t nextCode(const char*& p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    if (u[0] < 0x80) { ++p; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && u[1] >= 0x80 && u[1] < 0xC0) {
        p += 2;
        return (static_cast<uint32_t>(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    }
    if ((u[0] & 0xF0) == 0xE0 && u[1] >= 0x80 && u[1] < 0xC0 && u[2] >= 0x80 && u[2] < 0xC0) {
        p += 3;
        return (static_cast<uint32_t>(u[0] & 0x0F) << 12)
             | (static_cast<uint32_t>(u[1] & 0x3F) << 6)
             | (u[2] & 0x3F);
    }
    ++p;
    return u[0];
}

/// Pixel width of `s`, including the face's spacing between glyphs. A newline takes the width of a space.
inline int textWidth(const Font& f, const char* s) {
    if (!f.ok || !s) return 0;
    int w = 0;
    int glyphs = 0;
    const char* p = s;
    while (*p) {
        uint32_t cp = nextCode(p);
        if (cp == '\n') cp = ' ';
        const int idx = f.index(cp);
        if (idx < 0) continue;
        if (glyphs) w += f.spacing;
        w += f.widthAt(idx);
        ++glyphs;
    }
    return w;
}

}  // namespace mm::wbf
