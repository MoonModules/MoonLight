#pragma once
#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/Control.h"
#include "core/util/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace mm {

/// Output driver: publishes the rendered frame as an H.264 stream over HLS, from the device's own HTTP server. A TV, VLC or a browser can play it. It complements NdiDriver: NDI is the pro-tools path, HLS is the consumer-playback path.
///
/// The driver states numbers and the platform encodes. This driver packs the corrected frame and hands it over with the geometry, rate and bitrate. How those become H.264 differs per platform.
///
/// Prior art: HLS is Apple's, described in RFC 8216, and ffmpeg does the desktop encoding.
///
/// @moreinfo
///
/// ## Pixel-exact contract
///
/// The encoded frame is the grid from the layer, letterboxed by the display. Above scale 1 a light becomes a solid square block, still pixel-exact since replication invents no color. Scaling exists because a hardware encoder has a minimum frame size, and a small wall at one to one is a postage stamp.
///
/// ## Frame pacing
///
/// A fixed schedule rather than a last-sent timestamp. Millisecond division truncates and re-basing on arrival lets one late tick shift the schedule, and either drift ends in the player re-buffering.
///
/// ## Where it runs
///
/// H.264 needs a desktop-class CPU or a hardware encoder, so desktop and the ESP32-P4. Every other board reaches viewers through the preview instead.
/// @card HlsDriver.png
class HlsDriver : public DriverBase {
public:
    /// A destroyed driver releases the encoder, since nothing else can: a claim outliving its owner would refuse every later driver, and the next one can even land on this address.
    ~HlsDriver() override { platform::encoderRelease(this); }

    /// The catalog tag this driver carries.
    static constexpr const char* kTags = "🖥️";
    /// Where the encoder's segments are written, under the filesystem mount.
    static constexpr const char* kSegmentDir = "/.hls";   // under the fs mount; served at /hls/
                                                          // same literal in the file server

    /// Bind the two scratch buffers to this module, so their memory is accounted for.
    HlsDriver() : rgb_(*this), corrScratch_(*this) {}

    /// The catalog tags shown on this driver's card.
    const char* tags() const override { return kTags; }

    /// Point the driver at the shared source buffer.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Bind the frame rate, the scale, and the encoder choice where there is one.
    void defineDriverControls() override {
        controls_.addControl("targetFps", targetFps, 1, 120);
        // Auto picks the smallest factor clearing the encoder's floor, so a wall is never refused.
        controls_.addControl("scale", scale, 0, kMaxScale);
        // Absent rather than a one-entry dropdown where the platform has a single encoder.
        if constexpr (platform::hasEncoderChoice) {
            controls_.addSelect("encoder", encoderSel_, kEncoderOptions, kEncoderOptionCount);
            // Persisted by LABEL: editing the option list must never remap a stored pick.
            controls_.setPersistLabel(controls_.count() - 1);
        }
        // The playable address, one copy away from VLC or a Safari AirPlay hand-off.
        controls_.addReadOnly("url", urlBuf_, sizeof(urlBuf_));
    }

    /// Which controls need a fresh encode, the encoder fixing its rate at spawn.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "targetFps") == 0 || std::strcmp(name, "scale") == 0 ||
               std::strcmp(name, "encoder") == 0 || isCorrectionControl(name);
    }

    /// Derive the scaled geometry, make the segment directory, and start the encoder.
    void prepare() override {
        release();
        if (!layer_) return;

        srcWidth_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        srcHeight_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        // An explicit scale is honoured except below a hardware encoder's own minimum frame.
        const uint8_t floorScale = platform::hasEncoderChoice ? 1 : autoScale();
        scale_     = scale ? (scale > floorScale ? scale : floorScale) : autoScale();
        // Widened BEFORE multiplying: both operands are sane while their product need not be.
        uint32_t scaledW = static_cast<uint32_t>(srcWidth_)  * scale_;
        uint32_t scaledH = static_cast<uint32_t>(srcHeight_) * scale_;
        // Chroma is sampled in 2x2 blocks, so a hardware encoder takes even dimensions only.
        if constexpr (!platform::hasEncoderChoice) {
            if ((scaledW & 1u) || (scaledH & 1u)) {
                scale_  = static_cast<uint8_t>(scale_ * 2);
                scaledW *= 2;
                scaledH *= 2;
            }
        }
        if (scaledW > kMaxEncodeWidth || scaledH > kMaxEncodeHeight) {
            std::snprintf(statusBuf_, sizeof(statusBuf_),
                          "%ux%u at scale %u exceeds the encoder's %ux%u",
                          static_cast<unsigned>(srcWidth_), static_cast<unsigned>(srcHeight_),
                          static_cast<unsigned>(scale_),
                          static_cast<unsigned>(kMaxEncodeWidth),
                          static_cast<unsigned>(kMaxEncodeHeight));
            setStatus(statusBuf_, Severity::Warning);
            return;
        }
        width_     = static_cast<lengthType>(scaledW);
        height_    = static_cast<lengthType>(scaledH);

        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!rgb_.resize(pixels * 3)) {
            setStatus("out of memory for the video frame", Severity::Error);
            return;
        }
        if (correction_.outChannels > 3) corrScratch_.resize(correction_.outChannels);

        // Only where the encoder writes segments to disk rather than keeping them in RAM.
        if constexpr (platform::hasFsSegments) {
            platform::fsMkdir(kSegmentDir);
            clearSegments();                 // a stale playlist must not serve the old geometry
        }
        restartsLeft_ = kMaxRestarts;
        sendEpochMs_ = platform::millis();   // the schedule the frame pacing counts from
        nextSendMs_  = sendEpochMs_;
        frameIndex_  = 0;
        if (!startEncoder()) return;         // startEncoder set the status

        // RELATIVE: the browser resolves it against the host it loaded from, so it never goes stale.
        std::snprintf(urlBuf_, sizeof(urlBuf_), "/hls/stream.m3u8");
        if (scale_ > 1) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "streaming %ux%u as %ux%u at %u fps",
                          static_cast<unsigned>(srcWidth_), static_cast<unsigned>(srcHeight_),
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(targetFps));
        } else {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "streaming %ux%u at %u fps",
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(targetFps));
        }
        setStatus(statusBuf_, Severity::Status);
    }

    /// Stop the encoder and drop the segments it wrote.
    void release() override {
        if (open_) {
            platform::encoderRelease(this);   // stops it, and only where this driver holds the claim
            open_ = false;
            if constexpr (platform::hasFsSegments) clearSegments();   // transient; nothing to keep
        }
        DriverBase::release();
    }

    void tick() MM_NONBLOCKING override {
        // Gates HARD: after a mid-frame failure the pipe holds a partial frame and writes land shifted.
        if (!open_ || encoderDied_ || targetFps == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;
        // Nothing is sent until the encoder is demonstrably up, or a stall reads as a death.
        if (static_cast<int32_t>(platform::millis() - warmupUntilMs_) < 0) return;

        // A FIXED schedule, so the rate is exact at any fps and a late tick shifts nothing.
        const uint32_t now = platform::millis();
        const uint32_t periodMs = 1000u / targetFps;
        if (static_cast<int32_t>(now - nextSendMs_) < 0) return;
        frameIndex_++;
        nextSendMs_ = sendEpochMs_ + static_cast<uint32_t>(
            (static_cast<uint64_t>(frameIndex_) * 1000u) / targetFps);
        if (static_cast<int32_t>(now - nextSendMs_) > static_cast<int32_t>(periodMs * 4)) {
            // Far behind: restart the schedule here rather than firing a burst to catch up.
            sendEpochMs_ = now;
            // ONE, not zero: this tick's frame is frame 0, so the next one is frame 1.
            frameIndex_  = 1;
            nextSendMs_  = now + periodMs;
        }

        // A wide type: the light type is narrow on some boards and the product would overflow it.
        const size_t lights = static_cast<size_t>(srcWidth_) * srcHeight_;
        const size_t have   = sourceBuffer_->count();
        const size_t n      = lights < have ? lights : have;
        const size_t frameBytes = static_cast<size_t>(width_) * height_ * 3;
        if (n == 0 || rgb_.count() < frameBytes) return;

        const uint8_t* src   = sourceBuffer_->data();
        const uint8_t  srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t  outCh = correction_.outChannels;
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no frame to send

        // Packed with the output correction, so the stream shows what the wall shows.
        uint8_t* dst = &rgb_[0];
        const bool wide = outCh > 3 && corrScratch_.count() >= outCh;
        const size_t rowBytes = static_cast<size_t>(width_) * 3;
        for (nrOfLightsType i = 0; i < n; i++) {
            const uint8_t* s = src + static_cast<size_t>(i) * srcCh;
            uint8_t rgb[3];
            if (outCh == 3) {
                correction_.apply(s, rgb, srcCh);
            } else if (wide) {
                uint8_t* c = &corrScratch_[0];
                correction_.apply(s, c, srcCh);
                rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2];
            } else {
                rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2];
            }
            const lengthType sx = static_cast<lengthType>(i % srcWidth_);
            const lengthType sy = static_cast<lengthType>(i / srcWidth_);
            // One color conversion per light; the replication is a copy, not per-pixel work.
            uint8_t* row0 = dst + (static_cast<size_t>(sy) * scale_) * rowBytes
                                + (static_cast<size_t>(sx) * scale_) * 3;
            for (uint8_t px = 0; px < scale_; px++) {
                row0[px * 3 + 0] = rgb[0];
                row0[px * 3 + 1] = rgb[1];
                row0[px * 3 + 2] = rgb[2];
            }
            for (uint8_t py = 1; py < scale_; py++)
                std::memcpy(row0 + static_cast<size_t>(py) * rowBytes, row0,
                            static_cast<size_t>(scale_) * 3);
        }
        // Lights the source buffer never supplied leave their blocks black.
        if (n < lights) {
            for (nrOfLightsType i = n; i < lights; i++) {
                const lengthType sx = static_cast<lengthType>(i % srcWidth_);
                const lengthType sy = static_cast<lengthType>(i / srcWidth_);
                uint8_t* row0 = dst + (static_cast<size_t>(sy) * scale_) * rowBytes
                                    + (static_cast<size_t>(sx) * scale_) * 3;
                for (uint8_t py = 0; py < scale_; py++)
                    std::memset(row0 + static_cast<size_t>(py) * rowBytes, 0,
                                static_cast<size_t>(scale_) * 3);
            }
        }

        // Non-blocking: a full pipe drops the frame, and a dead process schedules a restart.
        const int wrote = platform::encoderWrite(dst, frameBytes);
        if (wrote == 0) droppedFrames_++;
        else if (wrote < 0) encoderDied_ = true;
    }

    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        if (encoderDied_ || !platform::encoderRunning()) {
            encoderDied_ = true;   // stays set until a restart SUCCEEDS: the pipe is dirty
            healthySecs_ = 0;
            // The backoff doubles per attempt, and the budget replenishes after sustained health.
            if (restartsLeft_ > 0 && restartWaitS_ > 0) { restartWaitS_--; return; }
            if (restartsLeft_ > 0) {
                restartsLeft_--;
                restartWaitS_ = static_cast<uint8_t>(1u << (kMaxRestarts - restartsLeft_));
                if (startEncoder()) {
                    encoderDied_ = false;   // fresh process, clean pipe: writing may resume
                    statusStale_ = true;    // the healthy branch repaints "streaming" next second
                    setStatus("encoder restarted", Severity::Warning);
                    return;
                }
            }
            open_ = false;
            setStatus("encoder exited - check ffmpeg (see the docs)", Severity::Error);
            return;
        }
        if (healthySecs_ < 60) healthySecs_++;
        else { restartsLeft_ = kMaxRestarts; restartWaitS_ = 0; }
        if (statusStale_ || droppedFrames_ != lastReportedDrops_) {
            statusStale_ = false;
            lastReportedDrops_ = droppedFrames_;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "streaming %ux%u, %u frames dropped",
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(droppedFrames_));
            setStatus(statusBuf_, Severity::Status);
        }
    }

    /// Video pixels per light, 0 being auto; one factor for both axes.
    uint8_t  scale       = 0;
    /// Encode-rate ceiling; the render loop runs faster and extra frames are not encoded.
    uint8_t  targetFps   = 30;
    /// The ffmpeg encoder pick; hardware entries offload the encode (see the control's comment).
    uint8_t  encoderSel_ = 0;   // index into kEncoderOptions; 0 = libx264, the universal default

private:
    static constexpr uint8_t kMaxRestarts = 3;
    /// The smallest frame the strictest supported encoder accepts.
    static constexpr uint16_t kMinEncodeSize = 80;
    /// The largest frame width the strictest supported encoder accepts.
    static constexpr uint16_t kMaxEncodeWidth  = 1920;
    static constexpr uint16_t kMaxEncodeHeight = 2032;
    /// A ceiling on the blow-up, so a slider cannot ask for a frame no encoder will take.
    static constexpr uint8_t kMaxScale = 80;
    static_assert(kMaxScale >= kMinEncodeSize,
                  "auto-scale must be able to reach the encoder's minimum from a 1-pixel axis");

    static constexpr uint32_t kWarmupMs = 750;   // encoder init headroom before the first frame
    // vaapi is deliberately absent: it needs filter plumbing this argument list does not build.
    static constexpr const char* kEncoderOptions[] = {
        "libx264",            // software; practically every ffmpeg distribution ships it
        "h264_videotoolbox",  // macOS media engine
        "h264_v4l2m2m",       // Raspberry Pi
        "h264_nvenc",         // NVIDIA
    };
    static constexpr uint8_t kEncoderOptionCount = 4;

    /// The smallest whole factor that lifts both axes to the encoder's minimum frame size.
    uint8_t autoScale() const {
        uint32_t f = 1;
        while (f < kMaxScale &&
               (srcWidth_ * f < kMinEncodeSize || srcHeight_ * f < kMinEncodeSize)) f++;
        return static_cast<uint8_t>(f);
    }

    // Derived, not asked for: the knob a user wants for bandwidth is the frame rate.
    /// The encode bitrate, following from the pixels and the frame rate.
    uint16_t autoBitrateKbit() const {
        const uint64_t pixels = static_cast<uint64_t>(width_) * height_;
        const uint64_t kbit = (pixels * targetFps) / 10000u;   // 0.1 bpp, expressed in kbit
        if (kbit < 500) return 500;
        if (kbit > 40000) return 40000;
        return static_cast<uint16_t>(kbit);
    }

    bool startEncoder() {
        char outDir[192];
        mm::formatTo(outDir, sizeof(outDir), "%s%s", platform::fsRootPath(), kSegmentDir);
        platform::EncoderConfig cfg{};
        cfg.width       = static_cast<uint16_t>(width_);
        cfg.height      = static_cast<uint16_t>(height_);
        cfg.fps         = targetFps;
        cfg.bitrateKbit = autoBitrateKbit();
        cfg.encoderName = kEncoderOptions[encoderSel_ < kEncoderOptionCount ? encoderSel_ : 0];
        cfg.outDir      = outDir;
        // One encoder, one claimant: a second driver is refused rather than silently reconfiguring this one's stream.
        if (!platform::encoderClaim(this)) {
            setStatus("the video encoder is in use by another driver", Severity::Warning);
            return false;
        }
        if (!platform::encoderStart(cfg)) {
            platform::encoderRelease(this);
            // Why it failed differs per platform, and a wrong reason sends the user hunting.
            if constexpr (platform::hasEncoderChoice) {
                setStatus("ffmpeg not found - see the docs", Severity::Warning);
            } else if (width_ < 80 || height_ < 80 || width_ > 1920 || height_ > 2032) {
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "%ux%u is outside the encoder's 80x80 to 1920x2032",
                              static_cast<unsigned>(width_), static_cast<unsigned>(height_));
                setStatus(statusBuf_, Severity::Warning);
            } else {
                setStatus("the hardware encoder did not start", Severity::Error);
            }
            return false;
        }
        warmupUntilMs_ = platform::millis() + kWarmupMs;
        open_ = true;
        return true;
    }

    void clearSegments() {
        // Collect first, remove after: deleting while the walk is live is unspecified.
        struct Names { char items[16][48]; uint8_t count = 0; } names;
        platform::fsList(kSegmentDir, [](const char* name, bool isDir, uint32_t, void* user) {
            auto* n = static_cast<Names*>(user);
            if (!isDir && n->count < 16) {
                std::snprintf(n->items[n->count], sizeof(n->items[0]), "%s", name);
                n->count++;
            }
        }, &names);
        for (uint8_t i = 0; i < names.count; i++) {
            char path[96];
            std::snprintf(path, sizeof(path), "%s/%s", kSegmentDir, names.items[i]);
            platform::fsRemove(path);
        }
    }

    Buffer*        sourceBuffer_ = nullptr;
    lengthType     srcWidth_  = 0;   // the wall
    lengthType     srcHeight_ = 0;
    lengthType     width_  = 0;      // the encoded frame: the wall times scale_
    lengthType     height_ = 0;
    uint8_t        scale_  = 1;      // the factor in use: the control, or autoScale
    // The two ticks run on different tasks, so every field both touch is an atomic.
    std::atomic<bool>     open_{false};
    std::atomic<bool>     encoderDied_{false};
    bool           statusStale_ = false;   // tick1s-only
    uint8_t        restartsLeft_ = kMaxRestarts;
    uint8_t        restartWaitS_ = 0;    // backoff countdown, in tick1s steps
    uint8_t        healthySecs_  = 0;    // sustained-health counter that replenishes the budget
    // A fixed schedule rather than a last-sent timestamp, so a late tick shifts nothing.
    uint32_t       sendEpochMs_ = 0;
    uint32_t       nextSendMs_  = 0;
    uint32_t       frameIndex_  = 0;
    std::atomic<uint32_t> warmupUntilMs_{0};
    std::atomic<uint32_t> droppedFrames_{0};
    uint32_t       lastReportedDrops_ = 0; // tick1s-only
    ScratchBuffer<uint8_t> rgb_;          // tight RGB staging, sized in prepare()
    ScratchBuffer<uint8_t> corrScratch_;  // one corrected light, when the wiring is wider than RGB
    char           urlBuf_[64]{};
    char           statusBuf_[64]{};
};

}  // namespace mm
