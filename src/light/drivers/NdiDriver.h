#pragma once
#include "core/module/Control.h"
#include "core/util/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Output driver: publishes the rendered frame as an NDI video source. It reaches OBS, Resolume, TouchDesigner or any other receiver, on this machine or another.
///
/// NDI rather than Spout or Syphon because one implementation covers every desktop platform, discovers by name, and crosses machines. At LED-wall pixel counts the latency difference sits far below one frame, so coverage decides.
///
/// Prior art: the NDI protocol and SDK are NewTek and Vizrt's. This is our own code against the documented C API.
///
/// @moreinfo
///
/// ## Desktop only
///
/// The NDI runtime is a closed binary built only for Intel and ARM. No ESP32 can load one, and there is no source to port. A board reaches the same receivers over the pixel protocols.
///
/// ## The runtime is the user's
///
/// MoonLight is GPL-3.0 and the runtime is proprietary, so it is never bundled or linked. The platform layer resolves it on demand. A machine without it runs normally and says so in the status, and no NDI type appears in this header.
///
/// @card NdiDriver.png
class NdiDriver : public DriverBase {
public:
    /// The catalog tag this driver carries.
    static constexpr const char* kTags = "🖥️";

    // The registration is what puts these buffers in the memory report.
    /// Bind the two scratch buffers to this module, so their memory is accounted for.
    NdiDriver() : rgb_(*this), corrScratch_(*this) {}

    /// The catalog tags shown on this driver's card.
    const char* tags() const override { return kTags; }

    /// Point the driver at the shared source buffer.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Bind the source name a receiver lists, and the frame-rate ceiling.
    void defineDriverControls() override {
        // Blank means the device's own name, which is what a user scanning a source list expects.
        controls_.addText("sourceName", sourceName, sizeof(sourceName));
        controls_.addControl("fps", fps, 1, 120);
    }

    /// A geometry change resizes the frame; a name change re-creates the sender (NDI has no rename).
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "sourceName") == 0 || isCorrectionControl(name);
    }

    /// Open the sender and size the staging buffers for the current geometry.
    void prepare() override {
        release();
        if (!layer_) return;

        width_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        height_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;

        if (!platform::ndiAvailable()) {
            // Not an error: the feature is not installed, and the fix is a user action.
            setStatus("NDI runtime not installed - see the docs", Severity::Warning);
            return;
        }
        if (!platform::ndiSenderOpen(sourceName[0] ? sourceName : nullptr)) {
            setStatus("could not create the NDI source", Severity::Error);
            return;
        }
        // Set BEFORE the staging, so release owns the sender from the moment it exists.
        open_ = true;

        // Sized off the hot path: one tight frame, plus a scratch for a wider wiring.
        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!rgb_.resize(pixels * 3)) {
            release();                       // closes the sender we opened
            setStatus("out of memory for the NDI frame", Severity::Error);
            return;
        }
        if (correction_.outChannels > 3) corrScratch_.resize(correction_.outChannels);

        std::snprintf(statusBuf_, sizeof(statusBuf_), "sending %ux%u at %u fps",
                      static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                      static_cast<unsigned>(fps));
        setStatus(statusBuf_, Severity::Status);
    }

    /// Close the sender, so the source stops being advertised.
    void release() override {
        if (open_) { platform::ndiSenderClose(); open_ = false; }
    }

    void tick() MM_NONBLOCKING override {
        if (!open_ || fps == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;

        // A CEILING: the protocol's own clock paces the receiver, this paces the build.
        const uint32_t now = platform::millis();
        if (now - lastSendMs_ < 1000u / fps) return;
        lastSendMs_ = now;

        const nrOfLightsType want = static_cast<nrOfLightsType>(width_) * height_;
        const nrOfLightsType have = sourceBuffer_->count();
        const nrOfLightsType n    = want < have ? want : have;
        if (n == 0) return;

        // Corrected like every other driver, so a receiver sees what the wall sees.
        if (rgb_.count() < static_cast<size_t>(want) * 3) return;
        uint8_t* dst = &rgb_[0];
        const uint8_t* src   = sourceBuffer_->data();
        const uint8_t  srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t  outCh = correction_.outChannels;
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no frame to send

        // A wider wiring corrects into a one-light scratch, since the protocol carries RGB only.
        const bool wide = outCh > 3 && corrScratch_.count() >= outCh;
        for (nrOfLightsType i = 0; i < n; i++) {
            const uint8_t* s = src + static_cast<size_t>(i) * srcCh;
            uint8_t* d = dst + static_cast<size_t>(i) * 3;
            if (outCh == 3) {
                correction_.apply(s, d, srcCh);
            } else if (wide) {
                uint8_t* c = &corrScratch_[0];
                correction_.apply(s, c, srcCh);
                d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
            } else {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];   // passthrough, same fallback as NetworkSend
            }
        }
        // Blanked, so a layout that shrank cannot show the previous frame's tail.
        if (n < want) std::memset(dst + static_cast<size_t>(n) * 3, 0,
                                  static_cast<size_t>(want - n) * 3);

        platform::ndiSendFrame(dst, static_cast<uint16_t>(width_),
                               static_cast<uint16_t>(height_), fps);
    }

    /// The name a receiver lists this source under; blank uses the device's own.
    char    sourceName[32] = "";
    /// The frame-rate ceiling, declared in each frame; the link may deliver fewer.
    uint8_t fps            = 30;

private:
    Buffer*        sourceBuffer_ = nullptr;
    lengthType     width_  = 0;
    lengthType     height_ = 0;
    bool           open_   = false;
    uint32_t       lastSendMs_ = 0;
    ScratchBuffer<uint8_t> rgb_;          // tight RGB staging, sized in prepare()
    ScratchBuffer<uint8_t> corrScratch_;  // one corrected light, when the wiring is wider than RGB
    char           statusBuf_[64]{};
};

}  // namespace mm
