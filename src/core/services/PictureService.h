#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"   // the one-active-source election
#include "core/util/color.h"            // RGB
#include "core/util/PictureFrame.h"
#include "core/util/ScratchBuffer.h"
#include "core/util/TryLock.h"      // the swap a reader holds off for its gather
#include "platform/platform.h"      // maxAllocBlock, freeHeap: whether the second picture fits

#include <cstdint>
#include <cstring>

namespace mm {

/// The picture the Picture effect samples onto the lights, from one source at a time.
/// @card PictureService.png
///
/// One picture service is active at a time, as with Audio, and any number of effects read its picture.
class PictureService : public MoonModule {
public:
    /// A service, the source half of the picture pipeline. It swaps a front and a back picture, or locks the one picture when there is no room for two, so frames skip but never tear.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// The sources `source` picks from, in option order.
    enum Source : uint8_t { kTestPattern, kSourceCount };
    /// Where the picture comes from.
    /// @guide The test pattern is a test card in the layout of Philips' PM5544, drawn at any size.
    uint8_t source = kTestPattern;
    /// The picture's width in pixels.
    uint16_t width = 64;
    /// The picture's height in pixels.
    uint16_t height = 48;

    /// Publish the source and the picture's size.
    void defineControls() override {
        static constexpr const char* kSources[] = {"test pattern"};
        controls_.addSelect("source", source, kSources, kSourceCount);
        controls_.addControl("width", width, 1, kMaxSide);
        controls_.addControl("height", height, 1, kMaxSide);
    }

    /// Every control reshapes or redraws the picture.
    bool affectsPrepare(const char*) const override { return true; }

    /// Size the pixels for the source and draw a still one into them; a failed allocation publishes no picture.
    void prepare() override {
        seat_.claim();
        if (!prepareBuffers(kRunsInTask[source])) return;
        drawTestPattern(front_, width, height);   // the frame prepareBuffers published, filled before any reader ticks
    }

    /// Size one picture, or two for a source writing from its own task, degrading to one when the second does not fit; false when even one does not.
    bool prepareBuffers(bool writerTask) {
        // On the render thread with the source's task stopped, so no reader or writer holds the pictures.
        const size_t bytes = static_cast<size_t>(width) * height * 3;
        if (!a_.resize(bytes)) {
            front_ = back_ = nullptr;
            frame_ = PictureFrame{nullptr, 0, 0, frame_.generation + 1};
            setStatus("not enough memory for the picture", Severity::Error);
            return false;
        }
        front_ = a_.data();
        back_ = writerTask && secondFits(bytes) && b_.resize(bytes) ? b_.data() : nullptr;
        if (!back_) b_.resize(0);
        if (writerTask && !back_) setStatus("one picture buffer: not enough memory, a capture may skip frames", Severity::Warning);
        else clearStatus();
        frame_ = PictureFrame{front_, width, height, frame_.generation + 1};
        return true;
    }

    /// Whether a second picture is held, so a writer fills one while readers read the other.
    bool twoBuffers() const { return back_ != nullptr; }

    /// The picture a source fills: the back one, or the only one, locked until endWrite.
    uint8_t* beginWrite() {
        if (back_) return back_;
        while (!lock_.tryAcquire()) platform::yield();   // readers hold it for one gather at most
        return front_;
    }
    /// Publish what beginWrite handed out: swap the two pictures, or unlock the one, and bump the generation.
    void endWrite() {
        if (back_) {
            while (!lock_.tryAcquire()) platform::yield();
            uint8_t* filled = back_;
            back_ = front_;
            front_ = filled;
        }
        frame_ = PictureFrame{front_, width, height, frame_.generation + 1};
        lock_.release();
    }

    /// Re-claim an empty seat, so removing the active service lets a survivor take over.
    void tick() MM_NONBLOCKING override { seat_.claim(); }

    /// Vacate the seat for any survivor.
    void release() override {
        seat_.vacate();
        MoonModule::release();
    }

    /// What a consumer reads with no source: a class constant, since a function-local static needs a guard on first use.
    static constexpr PictureFrame kNone{};

    /// The active service's picture held still for one read, or an empty frame when no service publishes one; false while a writer swaps or fills the only picture.
    class Reader {
    public:
        /// Take the active service's lock for one read, which fails while a writer swaps.
        Reader() : svc_(ActiveInstance<PictureService>::active()), held_(!svc_ || svc_->lock_.tryAcquire()) {}
        /// Let a writer swap again.
        ~Reader() { if (svc_ && held_) svc_->lock_.release(); }
        /// Not copyable: one reader holds the lock once.
        Reader(const Reader&) = delete;
        /// Not assignable, for the same reason.
        Reader& operator=(const Reader&) = delete;
        /// Whether the frame may be read now.
        explicit operator bool() const { return held_; }
        /// The frame, valid while this reader lives.
        const PictureFrame& frame() const { return svc_ ? svc_->frame_ : kNone; }
    private:
        PictureService* svc_;
        bool held_;
    };

    /// The test card in the layout of Philips' PM5544, drawn from coordinates at any size; its round circle shows a stretch, its gratings where sampling aliases.
    static void drawTestPattern(uint8_t* rgb, uint16_t w, uint16_t h) {
        const float cx = w / 2.0f, cy = h / 2.0f;
        const float r = 0.44f * (w < h * 4 / 3 ? w * 0.75f : h);   // the circle's radius: 88% of a 4:3 picture's height
        const float sq = 2.0f * r / 12.0f;                         // a grid square: twelve across the circle
        const float line = sq / 12.0f > 1.0f ? sq / 12.0f : 1.0f;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float dx = px - cx, dy = py - cy;
                uint8_t* out = rgb + (y * w + x) * 3;
                const RGB c = dx * dx + dy * dy <= r * r ? cardCircle((dx + r) / (2 * r), (dy + r) / (2 * r))
                                                         : cardBackground(dx / sq, dy / sq, line / sq);
                out[0] = c.r; out[1] = c.g; out[2] = c.b;
            }
    }

private:
    /// Outside the circle, in grid squares from the center: the gray grid with its white lines, and the four colored blocks at each side.
    static RGB cardBackground(float gx, float gy, float lineFrac) {
        RGB block;
        if (sideBlock(gx, gy, block)) return block;
        return onGridLine(gx, lineFrac) || onGridLine(gy, lineFrac) ? RGB{230, 230, 230} : RGB{128, 128, 128};
    }

    /// The colored block at (gx, gy), true when there is one: two a side, a tall one outside and a short one inside, each split at the middle row.
    static bool sideBlock(float gx, float gy, RGB& out) {
        static constexpr RGB kOuter[2][2] = {{{80, 160, 140}, {160, 160, 90}}, {{160, 80, 110}, {100, 80, 160}}};   // [bottom][right]
        static constexpr RGB kInner[2] = {{70, 130, 160}, {160, 110, 80}};                                          // [bottom]
        const float ax = gx < 0 ? -gx : gx;
        if (ax < 5.0f || ax >= 7.0f || gy <= -5.0f || gy >= 5.0f) return false;
        const int bottom = gy >= 0, right = gx >= 0;
        if (ax >= 6.0f) { out = kOuter[bottom][right]; return true; }
        if (gy > -3.0f && gy < 3.0f) return false;
        out = kInner[bottom];
        return true;
    }

    /// Whether a coordinate in grid squares falls on a grid line `lineFrac` of a square wide.
    static bool onGridLine(float g, float lineFrac) {
        return g - static_cast<float>(static_cast<int>(g < 0 ? g - 1 : g)) < lineFrac;
    }

    /// Inside the circle at (s, t), each 0 to 1 across its diameter: the center marker over the middle, else the band `t` falls in, from top to bottom.
    static RGB cardCircle(float s, float t) {
        if (s > 0.47f && s < 0.53f && t > 0.38f && t < 0.72f) return RGB{0, 0, 0};
        struct Band { float until; RGB (*draw)(float s, float t); };
        static constexpr Band kBands[] = {{0.125f, topBox}, {0.25f, shoulders}, {0.33f, steps}, {0.50f, colorBars},
                                          {0.60f, crossLines}, {0.75f, gratings}, {0.82f, grayRamp}, {0.91f, lowerBox}};
        for (const Band& b : kBands) if (t < b.until) return b.draw(s, t);
        return (s > 0.45f && s < 0.55f) ? RGB{180, 40, 30} : RGB{190, 190, 50};   // the yellow base with its red mark
    }

    // The bands, each one call: a box, the black shoulders, the gray and black steps, the bars, the lines, the gratings, the gray ramp, the lower box.
    static RGB bw(bool black) { return black ? RGB{0, 0, 0} : RGB{255, 255, 255}; }
    static RGB topBox(float s, float t) { return bw(s > 0.30f && s < 0.70f && t > 0.04f); }
    static RGB shoulders(float s, float) { return bw(s < 0.30f || s > 0.75f); }
    static RGB steps(float s, float) { return static_cast<int>(s * 10.0f) % 2 ? RGB{0, 0, 0} : RGB{190, 190, 190}; }
    static RGB colorBars(float s, float) {
        // Yellow, cyan, green, magenta, red, blue: the bars at three quarters of full saturation.
        static constexpr RGB kBars[6] = {{190, 190, 50}, {80, 190, 190}, {80, 190, 60}, {180, 60, 190}, {180, 40, 30}, {0, 30, 190}};
        return kBars[sixth(s)];
    }
    static RGB crossLines(float s, float t) {
        const float fs = s * 12.0f;
        return bw(!(fs - static_cast<float>(static_cast<int>(fs)) < 0.06f || (t > 0.545f && t < 0.555f)));
    }
    static RGB gratings(float s, float) {
        // Five gratings of rising frequency, the resolution wedges.
        if (s < 0.1f || s >= 0.9f) return RGB{0, 0, 0};
        const int g = static_cast<int>((s - 0.1f) / 0.16f);
        const float phase = (s - 0.1f - g * 0.16f) / 0.16f * static_cast<float>(3 + 2 * g);
        return bw(phase - static_cast<float>(static_cast<int>(phase)) >= 0.5f);
    }
    static RGB grayRamp(float s, float) {
        static constexpr uint8_t kSteps[6] = {0, 50, 100, 150, 200, 255};
        const uint8_t v = kSteps[sixth(s)];
        return RGB{v, v, v};
    }
    static RGB lowerBox(float s, float) { return bw(s > 0.25f && s < 0.75f); }
    /// Which sixth of the width `s` falls in, 0 to 5.
    static int sixth(float s) { const int i = static_cast<int>(s * 6.0f); return i < 0 ? 0 : (i > 5 ? 5 : i); }

    static constexpr uint16_t kMaxSide = 512;   ///< a side above this exceeds what a board holds for one picture
    /// Which sources write from a task of their own, in `source` order; the test pattern is drawn at prepare.
    static constexpr bool kRunsInTask[kSourceCount] = {false};

    /// Whether an optional picture of `bytes` fits one free block with the heap's reserve intact, the rule the mapping uses; 0 from the platform means no limit.
    static bool secondFits(size_t bytes) {
        const size_t block = platform::maxAllocBlock();
        if (block != 0 && block < bytes) return false;
        const size_t free = platform::freeHeap();
        return free == 0 || (free > platform::HEAP_RESERVE && free - platform::HEAP_RESERVE >= bytes);
    }

    ScratchBuffer<uint8_t> a_{*this}, b_{*this};   ///< the pictures, three bytes a pixel; b_ only for a writer task
    uint8_t* front_ = nullptr;   ///< what readers see
    uint8_t* back_ = nullptr;    ///< what a writer fills, or null with one picture
    PictureFrame frame_;         ///< the front picture, as a Reader hands it out
    TryLock lock_;               ///< held by a reader for its gather, by the writer to swap or to fill the only picture
    ActiveInstance<PictureService> seat_{*this};
};

}  // namespace mm
