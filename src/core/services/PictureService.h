#pragma once

#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // requestPrepare: a grabber arriving or leaving re-opens the capture
#include "core/util/ActiveInstance.h"   // the one-active-source election
#include "core/util/color.h"            // RGB
#include "core/util/format.h"           // formatTo: the status line built off snprintf
#include "core/util/PictureFrame.h"
#include "core/util/ScratchBuffer.h"
#include "core/util/TryLock.h"      // the swap a reader holds off for its gather
#include "platform/platform.h"      // maxAllocBlock, freeHeap: whether the second picture fits

#include <atomic>
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

    /// Stop any decode first: a task holding this service must never outlive it, even unreleased.
    ~PictureService() override { stopCapture(); }

    /// The sources `source` picks from, in option order.
    enum Source : uint8_t { kTestPattern, kUsbCapture, kSourceCount };
    /// Where the picture comes from.
    /// @guide The test pattern is a test card in the layout of Philips' PM5544, drawn at any size. With **usb capture**, MJPEG from an HDMI grabber fills the picture at its set size.
    uint8_t source = kTestPattern;
    /// Frames a second asked of a capture device, which streams the nearest rate it offers.
    uint8_t fps = 30;
    /// Source bytes at or below this read as black.
    /// @guide A grabber's black is rarely a 0: a dark frame decodes to a 1 or 2, which no screen shows and every LED does. Raise **noiseFloor** to the level its noise reaches.
    uint8_t noiseFloor = 1;
    /// Gap tolerated before the picture is dropped, in milliseconds.
    /// @guide A source that stops leaves its last picture on the lights, which reads as the room lit from a frozen frame. Past **staleMs** the picture goes, and the lights with it.
    uint16_t staleMs = 2000;
    /// The picture's width in pixels, and the most a capture scales its frames to.
    uint16_t width = 64;
    /// The picture's height in pixels, under the same ceiling as the width.
    uint16_t height = 48;

    /// Publish the source and the picture's size.
    void defineControls() override {
        static constexpr const char* kSources[] = {"test pattern", "usb capture"};
        // Only where the transport links, so a board without it shows the one source it has, as the capture's own controls do.
        controls_.addSelect("source", source, kSources,
                            static_cast<uint8_t>(platform::hasUsbVideo ? kSourceCount : 1));
        if (platform::hasUsbVideo) {
            controls_.addControl("fps", fps, 1, 120);
            controls_.addControl("noiseFloor", noiseFloor, 0, 64);
            controls_.addControl("staleMs", staleMs, 100, 10000);
        }
        controls_.addControl("width", width, 1, kMaxSide);
        controls_.addControl("height", height, 1, kMaxSide);
    }

    /// Every control reshapes or redraws the picture.
    bool affectsPrepare(const char*) const override { return true; }

    /// Size the pixels for the source and draw a still one into them; a failed allocation publishes no picture.
    void prepare() override {
        seat_.claim();
        stopCapture();   // before the pictures move under a decode still writing one
        if (source != kUsbCapture) platform::videoCaptureShutdown();   // what sees devices goes with the source
        if (!prepareBuffers(kRunsInTask[source])) return;
        if (source == kUsbCapture) { startCapture(); return; }
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
        bufW_ = width;
        bufH_ = height;
        floor_ = noiseFloor;
        back_ = writerTask && secondFits(bytes) && b_.resize(bytes) ? b_.data() : nullptr;
        if (!back_) b_.resize(0);
        if (writerTask && !back_) setStatus("one picture buffer: not enough memory, a capture may skip frames", Severity::Warning);
        else clearStatus();
        // A writer's picture is whatever it last decoded, so only its own publish lights it: a rebuild here would re-light a frame the drop just put out.
        frame_ = PictureFrame{writerTask ? nullptr : front_, width, height, frame_.generation + 1};
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
    /// Publish what beginWrite handed out at the size written, for a source that cannot hit the one it asked for; a size past the picture's is clamped to it.
    void endWrite(uint16_t w, uint16_t h) {
        if (w > bufW_) w = bufW_;
        if (h > bufH_) h = bufH_;
        if (back_) {
            while (!lock_.tryAcquire()) platform::yield();
            uint8_t* filled = back_;
            back_ = front_;
            front_ = filled;
        }
        frame_ = PictureFrame{front_, w, h, frame_.generation + 1};
        lock_.release();
    }
    /// Publish what beginWrite handed out: swap the two pictures, or unlock the one, and bump the generation.
    void endWrite() { endWrite(bufW_, bufH_); }
    /// Give back what beginWrite handed out without publishing it; with one picture, what a failed decode already wrote stays in it.
    void abortWrite() { if (!back_) lock_.release(); }

    /// Re-claim an empty seat, so removing the active service lets a survivor take over.
    void tick() MM_NONBLOCKING override { seat_.claim(); }

    /// Vacate the seat for any survivor.
    void release() override {
        stopCapture();
        platform::videoCaptureShutdown();
        seat_.vacate();
        MoonModule::release();
    }

    /// Name the capture's health once a second: the size it publishes, the rate it holds and whatever is going wrong.
    void tick1s() MM_NONBLOCKING override {
        if (source != kUsbCapture) return;
        dropStalePicture();   // first, so a device that went away puts the lights out on this tick and not the next
        // A grabber arriving or leaving rewrites the device list: an unplug closes the stream, which frees the port for a replug, and a replug opens it again.
        const uint32_t gen = platform::videoCaptureFormatGeneration();
        if (gen != openedGen_) {
            openedGen_ = gen;   // asked once; the open the prepare runs records it again
            if (Scheduler* s = Scheduler::instance()) s->requestPrepare(this);   // this service alone: the picture keeps its size, so no reader re-prepares
            return;
        }
        if (!task_.impl) {
            // Nothing to open over while the last teardown unwinds, so the open waits for it rather than failing in a way worth naming.
            if (platform::videoCaptureSettling())
                if (Scheduler* s = Scheduler::instance()) s->requestPrepare(this);
            return;
        }
        if (!lock_.tryAcquire()) return;   // the decode task is publishing: the next second reads it
        const bool one = !back_;   // the writer swaps back_ under the lock
        lock_.release();
        const unsigned w = bufW_, h = bufH_;   // the size a writer fills, which a dropped picture does not carry
        const platform::VideoCaptureStats st = platform::videoCaptureStats();
        // Per second, not since boot: a running total says nothing about whether the capture is live now.
        const uint32_t used = st.decoded - lastDecoded_, skipped = st.dropped - lastDropped_;
        const uint32_t failed = st.failed - lastFailed_;
        lastDecoded_ = st.decoded;
        lastDropped_ = st.dropped;
        lastFailed_ = st.failed;
        // Only what is happening, and skipped frames are the device offering more than the render takes, so only a refused frame warns.
        char skipText[24] = {}, tail[56] = {};
        if (skipped) mm::formatTo(skipText, sizeof(skipText), ", %u skipped", static_cast<unsigned>(skipped));
        mm::formatTo(tail, sizeof(tail), "%s%s%s", skipText, failed ? ", frames failing" : "",
                     one ? ", one picture" : "");
        if (used) mm::formatTo(status_, sizeof(status_), "live, %u x %u at %u fps%s", w, h,
                               static_cast<unsigned>(used), tail);
        else mm::formatTo(status_, sizeof(status_), "no frames, %u x %u%s", w, h, tail);
        setStatus(status_, failed || one || !used ? Severity::Warning : Severity::Status);
    }

    /// A gap of one frame is normal, since a source runs at its own rate. A long one means it stopped, and holding the picture would light the room from a frozen frame.
    void dropStalePicture() MM_NONBLOCKING {
        if (!staleMs || platform::millis() - lastFrameMs_.load(std::memory_order_relaxed) <= staleMs) return;
        if (!lock_.tryAcquire()) return;   // a writer is publishing, so the next second decides
        // Under the one hold, since the writer assigns the whole frame. The generation moves too, or an effect's redraw guard skips the change and the lights stay lit.
        if (frame_.rgb) frame_ = PictureFrame{nullptr, 0, 0, frame_.generation + 1};
        lock_.release();
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
    static constexpr bool kRunsInTask[kSourceCount] = {false, true};
    /// How long the task waits for a frame before checking for a stop, which bounds how long a control change waits on it.
    static constexpr uint32_t kFrameWaitMs = 50;

    /// Open the grabber and decode on a task of its own, so the render loop never waits on the device.
    void startCapture() {
        openedGen_ = platform::videoCaptureFormatGeneration();   // the device list this open saw, which tick1s watches
        // A teardown still unwinding is the one failure worth waiting on, since nothing installs over it; tick1s asks again until it has.
        if (platform::videoCaptureSettling()) {
            setStatus("looking for a grabber", Severity::Status);
            return;
        }
        if (!platform::videoCaptureInit(capture_, width, height, fps)) {
            // The step the platform stopped at, since a device that enumerated and still refused is a different fault from an empty port.
            const char* why = platform::videoCaptureFault();
            setStatus(why ? why : "no grabber attached", Severity::Warning);
            return;
        }
        if (back_) clearStatus();   // before the task swaps back_; with one picture, prepareBuffers' warning stays until the first status line
        // The open zeroes the device's counters, so the baselines go with them, and the frame clock starts now rather than at boot.
        lastDecoded_ = lastDropped_ = lastFailed_ = 0;
        lastFrameMs_.store(platform::millis(), std::memory_order_relaxed);
        stop_.store(false, std::memory_order_release);
        // Core 1, so a decode never lands on the render core, at priority 6 above the UVC driver (5) and the pump (4). A decode preempted mid-frame holds the only pending frame, and 60 fps has no room for that.
        if (!platform::spawnPinnedTask(task_, "mmPicCap", &captureTrampoline, this, 4096, 6, 1)) {
            platform::videoCaptureDeinit(capture_);
            setStatus("no task for the capture", Severity::Error);
            return;
        }
    }

    /// Stop the decode and release the device, in that order, so nothing writes a picture that has moved.
    void stopCapture() {
        if (task_.impl) {
            stop_.store(true, std::memory_order_release);
            platform::stopPinnedTask(task_);
        }
        if (capture_.impl) platform::videoCaptureDeinit(capture_);
    }

    static void captureTrampoline(void* self) { static_cast<PictureService*>(self)->runCapture(); }

    /// Wait for a frame, then decode it straight into the back picture and publish it, until prepare or release stops it.
    void runCapture() {
        while (!stop_.load(std::memory_order_acquire)) {
            // The wait comes before beginWrite, so with one picture the lock covers the decode alone and readers sample between frames.
            if (!platform::videoCaptureAwaitFrame(capture_, kFrameWaitMs)) continue;
            uint16_t w = 0, h = 0;
            uint8_t* px = beginWrite();
            // The size and the floor the pictures were prepared with, never a control a write changed since.
            if (platform::videoCaptureDecodeInto(capture_, px, bufW_, bufH_, w, h, floor_)) {
                lastFrameMs_.store(platform::millis(), std::memory_order_relaxed);
                endWrite(w, h);
            }
            else abortWrite();   // a refused frame: the lights keep the one they have rather than a new generation of nothing
        }
    }

    /// Whether an optional picture of `bytes` fits one free block with the heap's reserve intact, the rule the mapping uses; 0 from the platform means no limit.
    static bool secondFits(size_t bytes) {
        const size_t block = platform::maxAllocBlock();
        if (block != 0 && block < bytes) return false;
        const size_t free = platform::freeHeap();
        return free == 0 || (free > platform::HEAP_RESERVE && free - platform::HEAP_RESERVE >= bytes);
    }

    uint32_t openedGen_ = 0;   ///< the device list the last open saw, so a grabber arriving or leaving re-opens
    ScratchBuffer<uint8_t> a_{*this}, b_{*this};   ///< the pictures, three bytes a pixel; b_ only for a writer task
    uint8_t* front_ = nullptr;   ///< what readers see
    uint8_t* back_ = nullptr;    ///< what a writer fills, or null with one picture
    PictureFrame frame_;         ///< the front picture, as a Reader hands it out
    uint16_t bufW_ = 0, bufH_ = 0;   ///< the size the pictures were allocated at, which a writer fills; the controls may already say otherwise
    uint8_t floor_ = 0;              ///< the noise floor the decode reads, taken with the size so the task owns both
    TryLock lock_;               ///< held by a reader for its gather, by the writer to swap or to fill the only picture
    platform::VideoCaptureHandle capture_;   ///< the open grabber, or an empty handle
    platform::WorkerTask task_;              ///< the decode task, or an empty handle
    std::atomic<bool> stop_{false};          ///< tells the decode task to return
    uint32_t lastDecoded_ = 0, lastDropped_ = 0, lastFailed_ = 0;   ///< the counters a second ago, for the rates
    std::atomic<uint32_t> lastFrameMs_{0};   ///< when a picture last landed, for the stale drop
    char status_[96] = {};                   ///< the capture's status line, rebuilt once a second
    ActiveInstance<PictureService> seat_{*this};
};

}  // namespace mm
