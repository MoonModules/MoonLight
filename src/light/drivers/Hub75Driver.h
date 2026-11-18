#pragma once

#include "light/drivers/DriverBase.h"
#include "light/drivers/Hub75BusOutput.h"   // the output every backend this driver offers takes
#include "light/drivers/Hub75Output.h"      // the strategy that takes the corrected frame to the panel
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

/// Drives HUB75 LED panels straight from the board's own pins, with no receiving card in between.
/// @card Hub75Driver.png
///
/// The sibling of @ref PanelCardDriver, which drives the same panels over Ethernet. A [MoonHub75](https://moonmodules.org/projects/hardware/#moonhub75-pcb) runs a 64x64 1/32-scan panel at 6-bit and 1,508 Hz; larger walls wait on reports.
///
/// @attribution LostHope, Delphium, RobWomp, mrcodetastic, hzeller, ESPHome, Adafruit, Waveshare
///
/// @guide
///
/// ## Which of the two paths
///
/// A receiving card (@ref PanelCardDriver) earns its place above roughly 16,384 pixels; below that this driver is the small end of the split. [HUB75 panels](https://moonmodules.org/MoonLight/reference/hardware/hub75-panels.html#which-of-the-two-paths) has the choice.
///
/// @technical
///
/// **Output.** The driver corrects the layout into one RGB frame in light order and hands it to its Hub75Output, the bus output, which owns the board table, binds the pins and builds the panel's geometry. The output drives either peripheral, LCD_CAM or Parlio.
///
/// **Continuous output.** A HUB75 panel is lit only while clocked, so the platform arms one scan and re-sends the buffer forever. The bus output writes the next frame into it between scans, so tick() transmits nothing. The wire format is Hub75Encode.h.
///
/// **Brightness.** Binary coded modulation lights plane `p` for 2^p time units. Each plane is stored once, not 2^p times: `Hub75Geometry::frameSlots()` has the sizes.
///
/// **Pins.** The first definition fills them from the chip's first board; the members stay -1, since a blind guess lands on octal PSRAM, USB, UART0 or a strapping pin. WLED's pin array ends `...LAT,OE,CLK`, so a map transcribed as `...CLK,LAT,OE` swaps three lines silently.
class Hub75Driver : public DriverBase, public Hub75Settings {
public:
    /// The silicon block that drives the panel, LCD_CAM or Parlio, where the chip has both.
    /// @guide A P4 has one LCD_CAM and one Parlio, so a board already driving WS2812 strips from one needs the panel on the other. Only you know which way round.
    uint8_t peripheralSel_ = 0;

    /// Bind the bus output to this driver, which it reads its settings through.
    Hub75Driver() { output_->attach(this); }

    /// The backend the `peripheral` select names, for the output that drives it.
    platform::Hub75Backend selectedBackend() const override {
        return backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0];
    }

    /// A panel shows a level only as lit time, so brightness shortens the planes' windows rather than scaling values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }

    /// The peripheral block this driver holds while the bus is up, so a failed init claims nothing; the sibling claim guard reads it.
    LedHwBlock hwBlock() const override {
        return running_ ? output_->hwBlock() : LedHwBlock::None;
    }

    /// Bind the rendered frame this driver corrects from.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Publish the output's wiring, the peripheral select, then the output's own controls.
    void defineDriverControls() override {
        buildBackendOptions();
        output_->addWiringControls(controls_);
        controls_.addSelect("peripheral", peripheralSel_, backendOptions_, backendOptionCount_);
        output_->addControls(controls_);
    }

    /// Rebuild the card when the output says a change altered it, such as a board writing its pins.
    void onControlChanged(const char* name) override {
        // rebuildControls CLEARS first; a bare defineControls would append the rows again.
        if (name && output_->onControlChanged(name)) rebuildControls();
        DriverBase::onControlChanged(name);
    }

    /// Whether changing `name` forces a re-prepare: the peripheral and what the output says; a correction control, such as the fixture, resizes the scratch through onCorrectionChanged instead.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "peripheral") == 0 || output_->affectsPrepare(name);
    }

    /// Stop the scan, release the panel's peripheral, and free the correction scratch.
    void release() override {
        stopScan();
        DriverBase::release();
    }

    /// Open the output, then size the correction scratch to its box.
    void prepare() override {
        stopScan();   // a reinit keeps the correction scratch, which release() frees
        if (!layer_) {
            setStatus("no layer", Severity::Warning);
            return;
        }
        if (const char* why = output_->open(static_cast<uint16_t>(layer_->physicalWidth()),
                                            static_cast<uint16_t>(layer_->physicalHeight()))) {
            setStatus(why, Severity::Error);
            return;
        }
        // One whole frame of packed RGB: the output needs the frame, not one light at a time.
        ensureWire(output_->lights() * 3);
        if (!wire_) {
            setStatus("not enough memory for the correction buffer", Severity::Error);
            output_->close();
            return;
        }
        if (!sizeLightScratch()) {   // its status names the shortage
            output_->close();
            return;
        }

        running_ = true;
        setStatus(output_->describe(), Severity::Status);
    }

    /// Correct the frame into one RGB frame and hand it to the output.
    void tick() MM_NONBLOCKING override {
        if (!running_ || !sourceBuffer_ || !sourceBuffer_->data()) return;

        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t outCh = correction_.outChannels;
        const nrOfLightsType lights = static_cast<nrOfLightsType>(output_->lights());
        if (srcCh == 0 || outCh < 3 || lights == 0) return;
        if (!wire_ || wireCap_ < static_cast<size_t>(lights) * 3) return;   // not ready
        // A layout with fewer lights than the panel's box leaves the rest of the panel dark.
        const nrOfLightsType have = sourceBuffer_->count() < lights ? sourceBuffer_->count() : lights;

        // A plane reads (x, r) and the pixel half a panel below in one pass, so correct first.
        correctFrameToRgb(sourceBuffer_->data(), srcCh, wire_, have);
        std::memset(wire_ + static_cast<size_t>(have) * 3, 0, static_cast<size_t>(lights - have) * 3);

        // Brightness is the planes' lit time, read each frame so the slider applies on the next one.
        output_->show(wire_, effectiveBrightness());
    }

    /// Resize the correction scratch when the wiring changes, once core 1 has finished the frame it writes through it.
    void onCorrectionChanged() override {
        if (!running_) return;
        const size_t frameBytes = output_->lights() * 3;
        if (wireCap_ >= frameBytes && !outChannelsChanged()) return;   // a brightness change moves no buffer
        if (parent()) parent()->quiesce();   // a resize frees the block the encode worker may be writing
        ensureWire(frameBytes);
        sizeLightScratch();
    }

    /// Let the output publish its measured readouts; MM_NONBLOCKING stays, or the base's contract widens and the compiler refuses.
    void tick1s() MM_NONBLOCKING override {
        if (running_) output_->tick1s();
    }

private:
    /// Stop the scan and release the panel's peripheral, the reinit both release() and prepare() begin with.
    void stopScan() {
        output_->close();
        running_ = false;
    }

    /// Offer the backends this silicon has and that can carry this geometry, re-pointing by label so a dropped backend cannot select another.
    void buildBackendOptions() {
        const char* current = (peripheralSel_ < backendOptionCount_)
                                  ? backendOptions_[peripheralSel_] : nullptr;
        // Zero while the layer is unknown, which asks only whether the silicon exists.
        const size_t bytes = layer_
            ? output_->frameBytes(static_cast<uint16_t>(layer_->physicalWidth()),
                                  static_cast<uint16_t>(layer_->physicalHeight()))
            : 0;

        backendOptionCount_ = 0;
        for (uint8_t i = 0; i < kBackendCount; i++) {
            const auto b = static_cast<platform::Hub75Backend>(i);
            if (!platform::hub75BackendAvailable(b, bytes)) continue;
            backendOptions_[backendOptionCount_] = platform::hub75BackendLabel(b);
            backendIndex_[backendOptionCount_] = b;
            backendOptionCount_++;
        }
        if (backendOptionCount_ == 0) {      // no usable backend (host, classic ESP32)
            backendOptions_[0] = "(none)";
            backendIndex_[0] = platform::Hub75Backend::LcdCam;
            backendOptionCount_ = 1;
        }
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < backendOptionCount_; k++) {
                if (std::strcmp(backendOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        peripheralSel_ = sel;
    }

    /// The output's lines for the pin map, since their hidden controls read as free there.
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        return output_->fixedPins(out, max);
    }

    static constexpr uint8_t kBackendCount = 2;
    const char* backendOptions_[kBackendCount + 1] = {};
    platform::Hub75Backend backendIndex_[kBackendCount + 1] = {};
    uint8_t backendOptionCount_ = 0;

    /// The bus output, held by value so the driver's size and memory readout count it.
    Hub75BusOutput busOutput_;
    Hub75Output* output_ = &busOutput_;   // the strategy every call goes through
    Buffer* sourceBuffer_ = nullptr;
    bool running_ = false;
};

}  // namespace mm
