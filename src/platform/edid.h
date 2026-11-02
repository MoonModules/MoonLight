#pragma once

/// The hardware-free half of HDMI's feedback: the monitor's EDID (VESA E-EDID 1.4 with its CEA-861 extension), read over DDC and reduced to what the output needs: the monitor's name, its preferred size, and whether it lists each size the output can run.
///
/// HDMI carries video one way; the EDID is the one thing a monitor says back, and it says it in advance. A mode the monitor lists is one it accepts; nothing tells the source whether the picture arrived.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm::edid {

constexpr size_t kBlockBytes = 128;   // one EDID block; the base block and its first extension are read

/// One size the output can run, matched against the monitor's list by its active pixels alone: a monitor that lists a size takes it at the common rates.
struct Size {
    uint16_t width;    ///< active pixels across
    uint16_t height;   ///< active lines
};

/// What the output needs from the monitor.
struct Monitor {
    char     name[14] = {};          ///< the monitor name descriptor, 13 characters at most
    uint16_t preferredW = 0;         ///< the first detailed timing's width, which E-EDID 1.4 makes the preferred mode
    uint16_t preferredH = 0;         ///< its height
    uint32_t lists = 0;              ///< bit i set when the monitor lists sizes[i]
};

namespace detail {

inline bool checksumOk(const uint8_t* block) {
    uint8_t sum = 0;
    for (size_t i = 0; i < kBlockBytes; i++) sum = static_cast<uint8_t>(sum + block[i]);
    return sum == 0;
}

/// The sizes the output runs, which a parse marks against.
struct Sizes {
    const Size* sizes;   ///< the output's sizes, in its mode order
    size_t n;            ///< how many, of which the first 32 are marked
};

inline void mark(Monitor& m, const Sizes& s, uint16_t w, uint16_t h) {
    for (size_t i = 0; i < s.n && i < 32; i++) if (s.sizes[i].width == w && s.sizes[i].height == h) m.lists |= 1u << i;
}

/// An 18-byte detailed timing descriptor: pixel clock, then the active pixels in the split bytes the standard uses.
inline void detailed(const uint8_t* d, uint16_t& w, uint16_t& h) {
    w = static_cast<uint16_t>(d[2] | ((d[4] & 0xf0) << 4));
    h = static_cast<uint16_t>(d[5] | ((d[7] & 0xf0) << 4));
}

/// A 2-byte standard timing: width in units of 8 from 248, height from the aspect ratio in the top two bits.
inline bool standard(const uint8_t* s, uint16_t& w, uint16_t& h) {
    if (s[0] == 0x01 && s[1] == 0x01) return false;   // unused slot
    w = static_cast<uint16_t>((s[0] + 31) * 8);
    static constexpr uint8_t kAspect[4][2] = {{10, 16}, {3, 4}, {4, 5}, {9, 16}};   // height over width, by the top two bits
    const uint8_t* a = kAspect[s[1] >> 6];
    h = static_cast<uint16_t>(w * a[0] / a[1]);
    return true;
}

/// A CEA-861 short video descriptor's code and the size it names, for the sizes the output carries.
struct Vic {
    uint8_t  code;     ///< the video identification code
    uint16_t width;    ///< its active pixels across
    uint16_t height;   ///< its active lines
};
inline constexpr Vic kVics[] = {
    {1, 640, 480},    {2, 720, 480},    {3, 720, 480},    {4, 1280, 720},   {19, 1280, 720},  {5, 1920, 1080},
    {16, 1920, 1080}, {20, 1920, 1080}, {31, 1920, 1080}, {32, 1920, 1080}, {33, 1920, 1080}, {34, 1920, 1080},
    {93, 3840, 2160}, {94, 3840, 2160}, {95, 3840, 2160}, {96, 3840, 2160}, {97, 3840, 2160},
};

inline void vic(Monitor& m, const Sizes& s, uint8_t code) {
    for (const Vic& v : kVics) if (v.code == code) mark(m, s, v.width, v.height);
}

/// The name descriptor's text, padded with 0x0a and spaces.
inline void name(const uint8_t* d, Monitor& out) {
    size_t len = 0;
    while (len < 13 && d[5 + len] != 0x0a) len++;
    while (len && d[4 + len] == ' ') len--;
    std::memcpy(out.name, d + 5, len);
    out.name[len] = '\0';
}

/// One of the base block's four 18-byte descriptors: a timing, the first being the preferred mode, or text, one of which may be the name.
inline void descriptor(const uint8_t* d, bool first, const Sizes& s, Monitor& out) {
    if (!d[0] && !d[1]) {
        if (d[3] == 0xfc) name(d, out);
        return;
    }
    uint16_t w, h;
    detailed(d, w, h);
    mark(out, s, w, h);
    if (first) { out.preferredW = w; out.preferredH = h; }
}

/// The base block's timings: the two sizes the output runs that VESA names by bit, the eight standard timings, and the four descriptors.
inline void base(const uint8_t* b, const Sizes& s, Monitor& out) {
    if (b[35] & 0x01) mark(out, s, 800, 600);
    if (b[36] & 0x08) mark(out, s, 1024, 768);
    for (int i = 0; i < 8; i++) {
        uint16_t w, h;
        if (standard(b + 38 + 2 * i, w, h)) mark(out, s, w, h);
    }
    for (int i = 0; i < 4; i++) descriptor(b + 54 + 18 * i, i == 0, s, out);
}

/// The CEA extension: data blocks from byte 4 up to the detailed timings at `dtd`, each a tag in the top 3 bits and a length in the low 5, then the timings themselves.
inline void extension(const uint8_t* ext, const Sizes& s, Monitor& out) {
    const uint8_t dtd = ext[2];
    for (size_t p = 4; p < dtd && p < kBlockBytes; p += 1u + (ext[p] & 0x1f)) {
        if (ext[p] >> 5 != 2) continue;   // tag 2: the short video descriptors
        const uint8_t len = ext[p] & 0x1f;
        for (uint8_t v = 1; v <= len && p + v < kBlockBytes; v++) vic(out, s, ext[p + v] & 0x7f);
    }
    for (size_t p = dtd; p + 18 <= kBlockBytes && dtd >= 4 && (ext[p] || ext[p + 1]); p += 18) {
        uint16_t w, h;
        detailed(ext + p, w, h);
        mark(out, s, w, h);
    }
}

}  // namespace detail

/// Reduce an EDID to what the output needs; `ext` is the first extension block or null. False for a base block without the EDID header or with a wrong checksum, which is no monitor's answer.
inline bool parse(const uint8_t* base, const uint8_t* ext, const Size* sizes, size_t n, Monitor& out) {
    static const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    out = Monitor{};
    if (!base || std::memcmp(base, header, sizeof(header)) != 0 || !detail::checksumOk(base)) return false;
    const detail::Sizes s{sizes, n};
    detail::base(base, s, out);
    if (ext && ext[0] == 0x02 && detail::checksumOk(ext)) detail::extension(ext, s, out);
    return true;
}

}  // namespace mm::edid
