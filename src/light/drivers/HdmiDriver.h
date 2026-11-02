#pragma once
#include "core/module/Control.h"
#include "core/util/ScratchBuffer.h"
#include "core/util/format.h"   // formatTo: the monitor line, written once a second without blocking
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Shows the layer on a monitor through the board's HDMI port, scaled to the screen.
///
/// One light per pixel, corrected as the wall is, for a monitor, a projector or an LED wall's sending card (Novastar, ColorLight). ESP32-P4 boards with an LT8912B HDMI bridge, such as the Olimex ESP32-P4-PC.
///
/// @attribution Espressif, Olimex
/// @card HdmiDriver.png
///
/// @guide
///
/// ## Connect the monitor
///
/// A monitor plugged in or woken later is picked up within a second. Set its input to HDMI, not automatic, which some monitors leave for another input. Noise after a cold start: reset the board.
///
/// ## Status line
///
/// | It says | It means |
/// |---|---|
/// | `no I2C bus for the bridge` | Set System's I2C pins, as the device model does |
/// | `bridge did not come up, retrying` | No LT8912B answers on that bus |
/// | `no monitor connected` | Plug it in, or wake it |
/// | `<w>x<h> on <mode>` | Live; orange when the monitor does not list the mode |
class HdmiDriver : public DriverBase {
public:
    /// The catalog tag this driver carries.
    static constexpr const char* kTags = "🖥️";

    /// Bind the frame buffer to this module, so its registration puts its memory in the report.
    HdmiDriver() : rgb_(*this) {}

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
        /// What the monitor says of itself in its EDID: name, preferred size, a mode it lacks.
        controls_.addReadOnly("monitor", monitorStr_, sizeof(monitorStr_));
        /// The bridge's synthesizer word against the one programmed, and the sync it measures.
        /// @guide **DDS**, the bridge's clock read back, sits near the programmed value on a clean picture (5455 against 5555 at 1280x720) and further off in noise (571e). **Sync**'s second number is lines per frame: 741 at 1280x720.
        controls_.addReadOnly("bridge", bridgeStr_, sizeof(bridgeStr_));
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
        if (!sizeLightScratch()) {   // its status names the shortage
            release();
            return;
        }
        formatLiveLine();
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

    /// Follow the monitor coming and going, its EDID arriving, and the bridge's state.
    void tick1s() MM_NONBLOCKING override {
        DriverBase::tick1s();
        if (!open_) return;
        edid::Monitor m;
        const bool read = platform::hdmiMonitor(m);
        if (platform::hdmiLink() != lastLink_ || read != monitorRead_ || (read && m.lists != monitor_.lists)) writeStatus();
        formatBridgeLine();
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
        uint8_t* dst = &rgb_[0];
        correctFrameToRgb(sourceBuffer_->data(), srcCh, dst, n);   // corrected like every other driver, so the screen shows what the wall shows
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

    /// The status for the link's state now: the line prepare() formatted while a monitor is connected, a warning when that monitor does not list the mode.
    void writeStatus() MM_NONBLOCKING {
        lastLink_ = platform::hdmiLink();
        monitorRead_ = platform::hdmiMonitor(monitor_);
        formatMonitorLine();
        switch (lastLink_) {
            case platform::HdmiLink::Down:      setStatus("bridge did not come up, retrying", Severity::Error); return;
            case platform::HdmiLink::NoMonitor: setStatus("no monitor connected", Severity::Warning); return;
            case platform::HdmiLink::Monitor:   break;
        }
        setStatus(statusBuf_, modeListed() ? Severity::Status : Severity::Warning);
    }

    /// Whether the monitor lists the mode; true while nothing is known, so an unread EDID raises no warning.
    bool modeListed() const MM_NONBLOCKING { return !monitorRead_ || (monitor_.lists >> mode) & 1; }

    /// The bridge readout, as the platform last read it, or empty while there is nothing to read.
    void formatBridgeLine() MM_NONBLOCKING {
        platform::HdmiBridgeState b;
        if (!platform::hdmiBridge(b)) { bridgeStr_[0] = '\0'; return; }
        mm::formatTo(bridgeStr_, sizeof(bridgeStr_), "DDS %04x, wants %04x, sync %u/%u", static_cast<unsigned>(b.dds),
                     static_cast<unsigned>(b.ddsWant), static_cast<unsigned>(b.hsync), static_cast<unsigned>(b.vsync));
    }

    /// The monitor readout: empty without one, else its name, its preferred size and the mode it does not list.
    void formatMonitorLine() MM_NONBLOCKING {
        if (lastLink_ != platform::HdmiLink::Monitor) { monitorStr_[0] = '\0'; return; }
        if (!monitorRead_) { mm::formatTo(monitorStr_, sizeof(monitorStr_), "connected, no EDID read"); return; }
        const platform::HdmiMode* modes = nullptr;
        const size_t count = platform::hdmiModes(&modes);
        const char* name = monitor_.name[0] ? monitor_.name : "unnamed monitor";
        if (modeListed() || mode >= count)
            mm::formatTo(monitorStr_, sizeof(monitorStr_), "%s, prefers %ux%u", name,
                         static_cast<unsigned>(monitor_.preferredW), static_cast<unsigned>(monitor_.preferredH));
        else
            mm::formatTo(monitorStr_, sizeof(monitorStr_), "%s, prefers %ux%u, does not list %ux%u", name,
                         static_cast<unsigned>(monitor_.preferredW), static_cast<unsigned>(monitor_.preferredH),
                         static_cast<unsigned>(modes[mode].width), static_cast<unsigned>(modes[mode].height));
    }

    Buffer*        sourceBuffer_ = nullptr;
    lengthType     width_  = 0;
    lengthType     height_ = 0;
    bool           open_   = false;
    uint8_t        openMode_ = 0;         ///< the mode the output runs in, so a re-prepare in that mode keeps it
    platform::HdmiLink lastLink_ = platform::HdmiLink::NoMonitor;   ///< the link the status line describes
    edid::Monitor monitor_;     ///< the monitor the readout describes
    bool           monitorRead_ = false;  ///< whether monitor_ holds a read EDID
    uint32_t       lastSendMs_ = 0;
    uint8_t        modeCount_ = 0;
    char           modeNames_[kMaxModes][20]{};
    const char*    modeOptions_[kMaxModes]{};
    ScratchBuffer<uint8_t> rgb_;          ///< tight RGB staging, sized in prepare()
    char           statusBuf_[48]{};
    char           monitorStr_[64]{};
    char           bridgeStr_[48]{};
};

}  // namespace mm
