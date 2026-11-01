#pragma once
#include "core/module/Control.h"
#include "core/util/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Shows the layer on a monitor through the board's HDMI port, scaled to the screen.
///
/// The grid becomes the picture, one light per pixel, output correction applied, so the screen shows what the wall shows. A monitor, a projector, or the HDMI input of an LED wall's sending card (Novastar, ColorLight) takes it.
///
/// ESP32-P4 boards with an LT8912B bridge, such as the Olimex ESP32-P4-PC; the bridge sits on the board's I2C bus, which the device model sets. The bridge, its clocks and hot-plug are the platform's (platform_esp32_hdmi.cpp).
///
/// @attribution Espressif, Olimex
/// @card HdmiDriver.png
///
/// @guide
///
/// ## Connect the monitor
///
/// A monitor connected or woken later is picked up within a second. Set its input to HDMI rather than automatic: on automatic some monitors leave the signal for another input. A picture of noise after a cold start is the bridge missing the stream on its first configuration: reset the board.
///
/// ## Status line
///
/// | It says | It means |
/// |---|---|
/// | `no I2C bus for the bridge` | Set System's I2C bus pins, as the board's device model does |
/// | `no monitor connected` | Plug it in, or wake it |
/// | `<w>x<h> on <mode>` | Live |
class HdmiDriver : public DriverBase {
public:
    /// The catalog tag this driver carries.
    static constexpr const char* kTags = "🖥️";

    /// Bind the staging buffers to this module, so their registration puts their memory in the report.
    HdmiDriver() : rgb_(*this), corrScratch_(*this) {}

    /// The catalog tags shown on this driver's card.
    const char* tags() const override { return kTags; }

    /// Point the driver at the shared source buffer.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Bind the display mode, fit or stretch, and the frame-rate ceiling.
    void defineDriverControls() override {
        const platform::HdmiMode* modes = nullptr;
        modeCount_ = static_cast<uint8_t>(platform::hdmiModes(&modes));
        if (modeCount_ > kMaxModes) modeCount_ = kMaxModes;
        for (uint8_t i = 0; i < modeCount_; i++) {
            std::snprintf(modeNames_[i], sizeof(modeNames_[i]), "%ux%u %u Hz",
                          static_cast<unsigned>(modes[i].width), static_cast<unsigned>(modes[i].height),
                          static_cast<unsigned>(modes[i].hz));
            modeOptions_[i] = modeNames_[i];
        }
        if (mode >= modeCount_) mode = modeCount_ > kDefaultMode ? kDefaultMode : 0;
        controls_.addSelect("mode", mode, modeOptions_, modeCount_);
        controls_.addSelect("scaling", scaling, kScalingOptions, 2);
        controls_.addControl("fps", fps, 1, 60);
    }

    /// A mode change rebuilds the output; a geometry change resizes the frame.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "mode") == 0 || isCorrectionControl(name);
    }

    /// Start the output in the chosen mode and size the staging buffers for the current geometry; a re-prepare in the same mode keeps the output running, since every tree sweep reaches here and a monitor takes seconds to lock onto a restarted signal.
    void prepare() override {
        if (!layer_) { release(); return; }
        width_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        height_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        if (open_ && openMode_ != mode) { platform::hdmiClose(); open_ = false; }   // the output alone; the staging is resized below
        if (!open_) {
            if (!platform::hdmiOpen(mode)) {
                setStatus("no I2C bus for the bridge, or no HDMI on this board", Severity::Error);
                return;
            }
            open_ = true;   // set BEFORE the staging, so release owns the output from the moment it exists
            openMode_ = mode;
        }
        if (!rgb_.resize(static_cast<size_t>(width_) * height_ * 3)) {
            release();
            setStatus("out of memory for the HDMI frame", Severity::Error);
            return;
        }
        if (correction_.outChannels > 3) corrScratch_.resize(correction_.outChannels);
        formatLiveLine();
        lastConnected_ = !platform::hdmiConnected();   // forces the first status line
        writeStatus();
    }

    /// The live status line, formatted once here so the tick that shows it only points at it.
    void formatLiveLine() {
        const platform::HdmiMode* modes = nullptr;
        const size_t count = platform::hdmiModes(&modes);
        if (mode >= count) { std::snprintf(statusBuf_, sizeof(statusBuf_), "unknown mode"); return; }
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%ux%u on %ux%u %u Hz", static_cast<unsigned>(width_),
                      static_cast<unsigned>(height_), static_cast<unsigned>(modes[mode].width),
                      static_cast<unsigned>(modes[mode].height), static_cast<unsigned>(modes[mode].hz));
    }

    /// Stop the output, so the screen goes dark and its memory is freed.
    void release() override {
        if (open_) { platform::hdmiClose(); open_ = false; }
        DriverBase::release();
    }

    /// Follow the monitor coming and going.
    void tick1s() MM_NONBLOCKING override {
        DriverBase::tick1s();
        if (open_ && platform::hdmiConnected() != lastConnected_) writeStatus();
    }

    /// Hand a corrected frame to the output, at most `fps` times a second.
    void tick() MM_NONBLOCKING override {
        if (!open_ || fps == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;
        const uint32_t now = platform::millis();
        if (now - lastSendMs_ < 1000u / fps) return;
        lastSendMs_ = now;

        const nrOfLightsType want = static_cast<nrOfLightsType>(width_) * height_;
        const nrOfLightsType have = sourceBuffer_->count();
        const nrOfLightsType n    = want < have ? want : have;
        if (n == 0 || rgb_.count() < static_cast<size_t>(want) * 3) return;
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no picture
        const uint8_t outCh = correction_.outChannels;
        uint8_t* dst = &rgb_[0];
        // Corrected like every other driver, so the screen shows what the wall shows.
        const bool wide = outCh > 3 && corrScratch_.count() >= outCh;
        correctToRgb(sourceBuffer_->data(), srcCh, dst, n, wide ? &corrScratch_[0] : nullptr);
        if (n < want) std::memset(dst + static_cast<size_t>(n) * 3, 0, static_cast<size_t>(want - n) * 3);
        platform::hdmiWrite(dst, static_cast<uint16_t>(width_), static_cast<uint16_t>(height_), scaling == 0);
    }

    /// The screen mode; 1280x720 suits a 1080p monitor, which scales it by exactly 1.5.
    uint8_t mode    = kDefaultMode;
    /// 0 fit, 1 stretch: fit keeps the grid's shape with black bars, stretch fills the screen.
    uint8_t scaling = 0;
    /// Frame-rate ceiling; the screen refreshes on its own.
    uint8_t fps     = 30;

private:
    static constexpr uint8_t kMaxModes = 8;
    static constexpr uint8_t kDefaultMode = 2;   // 1280x720 at 60 Hz in the platform's list
    static constexpr const char* kScalingOptions[2] = {"fit", "stretch"};

    /// The status for the monitor's state now: the line prepare() formatted while one is connected.
    void writeStatus() MM_NONBLOCKING {
        lastConnected_ = platform::hdmiConnected();
        if (!lastConnected_) { setStatus("no monitor connected", Severity::Warning); return; }
        setStatus(statusBuf_, Severity::Status);
    }

    Buffer*        sourceBuffer_ = nullptr;
    lengthType     width_  = 0;
    lengthType     height_ = 0;
    bool           open_   = false;
    uint8_t        openMode_ = 0;         ///< the mode the output runs in, so a re-prepare in that mode keeps it
    bool           lastConnected_ = false;
    uint32_t       lastSendMs_ = 0;
    uint8_t        modeCount_ = 0;
    char           modeNames_[kMaxModes][20]{};
    const char*    modeOptions_[kMaxModes]{};
    ScratchBuffer<uint8_t> rgb_;          ///< tight RGB staging, sized in prepare()
    ScratchBuffer<uint8_t> corrScratch_;  ///< one corrected light, when the wiring is wider than RGB
    char           statusBuf_[48]{};
};

}  // namespace mm
