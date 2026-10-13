#pragma once

#include "core/util/ActiveInstance.h" // the one-active-source seat (RAII vacate on destruct)
#include "core/util/color.h"          // RGB: the pattern's band colors
#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "core/util/VideoFrame.h"
#include "platform/platform.h" // fsSize / fsReadAt / millis

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// The device's video input: one decoded RGB frame per tick, published through the static `latestFrame()`.
///
/// @moreinfo
///
/// ## Two sources
///
/// Decoded once here however many effects read it, and effects hold no pointer back to this module; the user adds it under `Services`, so it is not auto-wired.
/// `test pattern` is a DIAGNOSTIC rather than decoration: its colored border bands make a border-mapped effect's orientation self-evident.
/// `file` reads a binary PPM, chosen over JPEG because there is no software JPEG decoder here.
/// Both render into `buf_`, which this module owns, so a published frame lives exactly as long as the service does.
class VideoService : public MoonModule {
public:
    /// A service: it produces for others and draws nothing itself.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    // Appended, never reordered, so a persisted index keeps its meaning.
    /// `source` value for the synthesized diagnostic pattern.
    static constexpr uint8_t kSourcePattern = 0;
    /// `source` value for a binary PPM on the filesystem.
    static constexpr uint8_t kSourceFile = 1;
    /// Dropdown labels for `source`, indexed by the constants above.
    static constexpr const char* kSourceOptions[] = {"test pattern", "file"};
    /// How many of `kSourceOptions` this build offers.
    static constexpr uint8_t kSourceCount = 2;

    /// Which of `kSourceOptions` fills the frame: @xref{two-sources|what each one is for}.
    uint8_t source = kSourcePattern;
    /// Path the `file` source reads.
    char file[64] = "/frame.ppm";
    // Zero parks the block, which makes the pattern a still reference for checking a border light against a known color. Seventeen is the rate it used to be fixed at: a sweep every ~4 s.
    /// Sweep rate of the test pattern's white block, in pixels per second.
    uint8_t patternSpeed = 17;

    // Small on purpose: a border effect averages the frame down to a few dozen values, so pixels beyond that buy nothing but bandwidth and decode time.
    /// Width of the synthesized pattern, 16:9 with kPatternH.
    static constexpr uint16_t kPatternW = 64;
    /// Height of the synthesized pattern.
    static constexpr uint16_t kPatternH = 36;
    /// Thickness of each colored edge band, in pixels.
    static constexpr int kBand = kPatternH / 4;
    // Comfortably past 4K, so a corrupt header is rejected at parse time rather than failing later as "too large for memory". What bounds the allocation is buf_.resize() failing.
    /// Sanity ceiling on either dimension of a loaded file.
    static constexpr uint32_t kMaxDim = 4096;

    /// The live frame. The POINTER is never null - with no source this returns kNoVideoFrame, which has a null `rgb`. So callers test the frame's contents, never the pointer
    static const VideoFrame* latestFrame() MM_NONBLOCKING {
        VideoService* v = ActiveInstance<VideoService>::active();
        return v ? &v->frame_ : &kNoVideoFrame;
    }

    /// Test seam: the curve as built.
    const uint16_t* toneForTest() const { return tone_; }

    /// Claims the one-active-source seat at construction, before any prepare() runs.
    VideoService() { seat_.claim(); }

    /// The source picker, plus the settings that belong to whichever source is selected.
    void defineControls() override {
        controls_.addSelect("source", source, kSourceOptions, kSourceCount);
        // Live (not in affectsPrepare): the rate changes what the NEXT frame draws, and nothing about the buffer, so it must not tear the pipeline down to take effect.
        controls_.addControl("patternSpeed", patternSpeed, 0, 255);
        controls_.setHidden(controls_.count() - 1, source != kSourcePattern);
        controls_.addText("file", file, sizeof(file));
        controls_.setHidden(controls_.count() - 1, source != kSourceFile);
        controls_.addButton("reload");
        controls_.setHidden(controls_.count() - 1, source != kSourceFile);
        MoonModule::defineControls();
    }

    /// A source switch changes what the buffer must hold, so it re-runs the whole build. The reload button re-reads the same file in place: cheap, and it must NOT tear down the pipeline.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "source") == 0 || std::strcmp(name, "file") == 0;
    }

    /// Routes the one live edit, the reload button; every other control re-runs prepare().
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "reload") == 0) loadFile();
        MoonModule::onControlChanged(name);
    }

    /// Cold path: size the frame buffer for the selected source and fill it once, so a frame exists before the first tick rather than one tick later.
    void prepare() override {
        seat_.claim();  // re-take after a disable/enable cycle: release() vacated it
        if (source >= kSourceCount) source = kSourcePattern; // a config naming a source this build lacks
        rebuildTone(); // both sources are display-encoded, so the curve is built once and never moves
        if (source == kSourceFile) {
            loadFile();
        } else if (allocate(kPatternW, kPatternH)) {
            renderPattern();
        }
    }

    /// Only the synthesized pattern regenerates per frame, since it animates; a still file keeps the buffer it already holds. File I/O is blocking and belongs nowhere near this function.
    void tick() MM_NONBLOCKING override {
        // Take an EMPTY seat, so deleting the elected source while a second one runs hands over rather than going permanently dark. claim() only fills an empty seat, never yanks one.
        seat_.claim();
        if (source == kSourcePattern && buf_.data())
            renderPattern();
        else if (source == kSourceFile && buf_.data())
            publish();
        MoonModule::tick();
    }



    /// Drops the published frame and the seat, so a disabled module holds neither.
    void release() override {
        frame_ = VideoFrame{};
        seat_.vacate();
        MoonModule::release();
    }

private:
    // Claimed at CONSTRUCTION, since an effect resolves latestFrame() during its own build, then re-claimed in prepare() after a disable/enable and in tick() so a survivor inherits it.
    ActiveInstance<VideoService> seat_{*this};


    static constexpr int kMaxHeaderBytes = 256; // room for a comment, and its own error if not
    ScratchBuffer<uint8_t> buf_{*this}; // width*height*3, accounted in dynamicBytes()
    VideoFrame frame_;
    uint32_t seq_ = 0;
    uint16_t tone_[256] = {}; // the published curve: source encoding -> linear light
    // Sweep position in 1/1000 px and the millis() it was last advanced at. Milli-pixels because a per-second rate sampled per tick rounds to zero motion in whole pixels at 1 px/s.
    uint32_t sweepMilliPx_ = 0;
    uint32_t sweepAtMs_ = 0;
    char status_[40] = {};

    /// Drop the published frame and say why. Returns false so every failing path reads as one line, `return fail("...")`, and none can forget to un-publish the stale frame.
    bool fail(const char* why) {
        frame_ = VideoFrame{};
        setStatus(why, Severity::Error);
        return false;
    }

    /// Size the buffer and point the published frame at it. False on any failure, so a too-large image degrades to "no video" rather than to a crash.
    bool allocate(uint16_t w, uint16_t h) {
        if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim) return fail("frame size out of range");
        if (!buf_.resize(static_cast<uint32_t>(w) * h * 3u)) return fail("frame too large for memory");
        frame_.rgb = buf_.data();
        frame_.width = w;
        frame_.height = h;
        // Published here, not from a tick: dimensions only change on a resize, so this keeps the snprintf off the render path.
        std::snprintf(status_, sizeof(status_), "%ux%u", w, h);
        setStatus(status_, Severity::Status);
        return true;
    }

    /// Publish the buffer as a NEW frame: the sequence bump is what tells a consumer the pixels changed, so every producer path ends here (see VideoFrame::seq).
    void publish() {
        frame_.tone = tone_; // per frame, so a curve change lands on the next one with nothing to remember
        frame_.seq = ++seq_;
    }

    /// Fill `tone_`: the source's transfer curve undone to linear light, where a consumer averages (the mean of encoded bytes is not the mean of the picture). Cold path: 256 float evaluations per edit or prepare, never per frame.
    void rebuildTone() {
        for (int i = 0; i < 256; i++) {
            const float e = static_cast<float>(i) / 255.0f;
            // sRGB EOTF (IEC 61966-2-1), which is what both a PPM and the pattern carry.
            float lin = e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f);
            if (lin < 0.0f) lin = 0.0f;
            if (lin > 1.0f) lin = 1.0f;
            tone_[i] = static_cast<uint16_t>(lin * VideoFrame::kLinearMax + 0.5f);
        }
    }

    // Four colored border bands and a sweeping white block. Integer-only and allocation-free: it runs on the render tick.
    void renderPattern() {
        uint8_t* p = buf_.data();
        if (!p) return;
        // An accumulator fed by elapsed time, not a position derived from millis(): a derived one jumps the block the instant the rate changes and cannot express "stopped" at all.
        const uint32_t now = platform::millis();
        if (sweepAtMs_ == 0) sweepAtMs_ = now; // first frame: no elapsed time to charge for
        const uint32_t elapsed = now - sweepAtMs_;
        sweepAtMs_ = now;
        if (patternSpeed) sweepMilliPx_ = (sweepMilliPx_ + elapsed * patternSpeed) % (kPatternW * 1000u);
        const int sweepX = static_cast<int>(sweepMilliPx_ / 1000u);
        for (int y = 0; y < kPatternH; y++) {
            for (int x = 0; x < kPatternW; x++) {
                // A white block riding the top edge: shows liveness, and which way "forward" runs.
                const bool onSweep = y < kBand && x >= sweepX && x < sweepX + 4;
                const RGB c = onSweep ? RGB{255, 255, 255} : bandColor(x, y);
                uint8_t* px = p + (static_cast<size_t>(y) * kPatternW + x) * 3;
                px[0] = c.r;
                px[1] = c.g;
                px[2] = c.b;
            }
        }
        publish();
    }

    /// Color of the pattern at (x, y): one hue per edge, black interior.
    static RGB bandColor(int x, int y) {
        if (y < kBand) return {255, 0, 0};              // top    → red
        if (y >= kPatternH - kBand) return {0, 0, 255}; // bottom → blue
        if (x < kBand) return {255, 255, 0};            // left   → yellow
        if (x >= kPatternW - kBand) return {0, 255, 0}; // right  → green
        return {0, 0, 0};                               // interior stays dark
    }

    // --- PPM (P6) file source -----------------------------------------------------------------
    /// Read the header, size the buffer, then read the pixel block straight into it. Cold path only (prepare / the reload button): this blocks on the filesystem.
    bool loadFile() {
        const long size = platform::fsSize(file);
        if (size <= 0) return fail("file not found");

        // Netpbm allows comments and any run of whitespace between tokens, so a header is not a fixed length, and the ceiling has to clear the comment GIMP writes.
        char header[kMaxHeaderBytes] = {};
        const int headerLen = platform::fsReadAt(file, 0, header, sizeof(header) - 1);
        uint16_t w = 0, h = 0;
        const int pixOff = parsePpmHeader(header, headerLen, w, h);
        if (pixOff < 0)
            return fail(headerLen >= static_cast<int>(sizeof(header)) - 1
                            ? "PPM header too long"
                            : "not a binary PPM (P6, maxval 255)");
        if (!allocate(w, h)) return false; // allocate() already reported why

        const uint32_t need = static_cast<uint32_t>(w) * h * 3u;
        if (static_cast<uint32_t>(size - pixOff) < need) return fail("PPM truncated");

        const int read = platform::fsReadAt(file, pixOff, reinterpret_cast<char*>(buf_.data()), need);
        if (read < 0 || static_cast<uint32_t>(read) != need) return fail("PPM read failed");

        setStatus(status_, Severity::Status); // allocate() formatted the size; restore it over an error
        publish();
        return true;
    }

public:
    /// Parse a binary-PPM header (Netpbm). Returns the byte offset where pixel data begins, or -1 if `buf` is not one. Pure: `len` bounds the read, so a truncated file is rejected rather than parsed into whatever follows it.
    static int parsePpmHeader(const char* buf, int len, uint16_t& w, uint16_t& h) {
        // P6 <width> <height> <maxval> <ONE whitespace byte> <pixels>
        HeaderCursor cur{buf, len};
        cur.skipBlanks();
        if (cur.pos + 1 >= len || buf[cur.pos] != 'P' || buf[cur.pos + 1] != '6') return -1;
        cur.pos += 2;

        const long ww = cur.readInt();
        const long hh = cur.readInt();
        const long maxval = cur.readInt();
        if (ww <= 0 || ww > static_cast<long>(kMaxDim)) return -1;
        if (hh <= 0 || hh > static_cast<long>(kMaxDim)) return -1;
        if (maxval != 255) return -1; // 16-bit samples are two big-endian bytes: another format
        // Netpbm requires ONE whitespace byte here. Accepting whatever is present would eat a pixel: "P6\n2 2\n255X" would read as valid with the X swallowed.
        if (cur.pos >= len || !HeaderCursor::isBlank(buf[cur.pos])) return -1;

        w = static_cast<uint16_t>(ww);
        h = static_cast<uint16_t>(hh);
        return cur.pos + 1; // the pixels begin straight after that one byte
    }

private:
    /// Position within an ASCII header. Netpbm separates tokens with any run of whitespace and `#` comments to end-of-line, so both readers skip those first.
    struct HeaderCursor {
        const char* buf;
        int len;
        int pos = 0;

        /// Spelled out rather than isspace(), which is locale-dependent and undefined for a signed char above 127.
        static bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

        void skipBlanks() {
            while (pos < len) {
                if (buf[pos] == '#') {
                    while (pos < len && buf[pos] != '\n') pos++;
                } else if (isBlank(buf[pos])) {
                    pos++;
                } else {
                    break;
                }
            }
        }

        /// Next decimal token, or -1 when the next token is not one. Capped well above any real dimension purely so a long digit run cannot overflow; the true bounds are the caller's.
        long readInt() {
            skipBlanks();
            if (pos >= len || buf[pos] < '0' || buf[pos] > '9') return -1;
            long v = 0;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                v = v * 10 + (buf[pos] - '0');
                if (v > 100000) return -1;
                pos++;
            }
            return v;
        }
    };
};

} // namespace mm
