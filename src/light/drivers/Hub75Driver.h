#pragma once

#include "light/drivers/DriverBase.h"
#include "light/drivers/Hub75BusOutput.h"   // the output that drives the LCD_CAM and Parlio backends
#include "light/drivers/Hub75Fit.h"         // kGeometryMessageMax: the longest status the Pi output writes
#include "light/drivers/Hub75Output.h"      // the strategy that takes the corrected frame to the panel
#include "light/drivers/Hub75PioOutput.h"   // the output that drives the Pi 5's PIO backend
#include "light/layers/Layer.h"
#include "platform/machine.h"      // which machine this is: the output follows it
#include "platform/platform.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace mm {

/// What the driver holds in the PIO output's place where no Pi 5 can have one: an empty type, so an ESP32 image carries none of it.
struct NoPioOutput {
    /// Takes the driver as the PIO output does, and discards it.
    explicit NoPioOutput(MoonModule& /*owner*/) {}
};

/// `output` as a Hub75Output: the PIO output itself.
inline Hub75Output* asHub75Output(Hub75PioOutput& output) { return &output; }
/// Where no PIO output exists there is no output to give.
inline Hub75Output* asHub75Output(NoPioOutput& /*output*/) { return nullptr; }

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
/// **One driver per peripheral.** A second driver that asks for LCD_CAM, Parlio or a Pi's header while another holds it stays idle with a status naming why, whichever of the two prepares first keeping it. This applies to the ESP32 too: a HUB75 driver yields to a strip driver holding LCD_CAM, and the strip driver to it.
///
/// **Outputs.** The driver corrects the layout into one RGB frame in light order for a Hub75Output. The bus output reads a row-major raster; the PIO output's canvas is a stack of lanes of panels, so it places its lights first (Hub75Canvas.h) and hands the frame to the seam. Both are held by value, so the driver's size and memory readout count them; the PIO output is empty where no Pi 5 runs.
///
/// **Continuous output.** A HUB75 panel is lit only while clocked, so on an ESP32 the platform re-sends one buffer forever and the bus output writes the next frame into it between scans, while tick() starts no transfer; the PIO output hands each frame to the seam, which hands it to the engine's threads.
///
/// **Brightness.** Binary coded modulation lights plane `p` for 2^p time units. Each plane is stored once, not 2^p times: `Hub75Geometry::frameSlots()` has the sizes.
///
/// **The output follows the machine** (platform/machine.h), a case tree with one case per kind and no default. An ESP32 takes the bus output and the backends it offers, LCD_CAM and Parlio per chip. A Pi 5 takes the PIO output, which stays selected when the PIO is refused and shows the reason as its status.
///
/// An earlier Pi shows the Pi card and a refusal. Any other host takes the bus output and its refusal, or the test port's backends.
///
/// **The Pi's settings** (rows, columns and panels per lane, the pixel clock and the latch blank) live on the driver, bound on every host and shown only on the Pi card, so a saved card keeps them, the 8-bit depth included, through a boot where the PIO refuses.
///
/// **Swapping outputs.** A machine that changes, which a test makes, quiesces the render worker, which reads the output in tick(), closes the running output and points the driver at the other, moving the depth into its range. The change reaches prepare() as a whole-tree re-prepare, the default prepare scope, which joins the encode worker before the output closes.
///
/// **Pins.** The first definition fills them from the chip's first board; the members stay -1, since a blind guess lands on octal PSRAM, USB, UART0 or a strapping pin. WLED's pin array ends `...LAT,OE,CLK`, so a map transcribed as `...CLK,LAT,OE` swaps three lines silently.
class Hub75Driver : public DriverBase, public Hub75Settings {
public:
    /// The silicon block that drives the panel: LCD_CAM, Parlio or a Pi 5's PIO.
    /// @guide A P4 has one LCD_CAM and one Parlio, so a board already driving WS2812 strips from one needs the panel on the other. Only you know which way round.
    uint8_t peripheralSel_ = 0;

    /// Bind both outputs to this driver, which they read their settings through.
    Hub75Driver() : pioOutput_(*this) {
        busOutput_.attach(this);
        if (Hub75Output* pio = asHub75Output(pioOutput_)) pio->attach(this);
    }

    /// Test-only: the output the driver calls through, which a test reads, or replaces with a counting double before the card is defined; a swap here skips selectOutput()'s quiesce and clamp, and defineControls() reverts it.
    Hub75Output*& outputForTest() { return output_; }

    /// The backend the `peripheral` select names, for the output that drives it.
    platform::Hub75Backend selectedBackend() const override {
        return backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0];
    }

    /// The layer this driver reads its layout from, which the Pi output cuts into panels.
    Layer* drivenLayer() const override { return layer(); }

    /// A panel shows a level only as lit time, so brightness shortens the planes' windows rather than scaling values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }

    /// The peripheral block this driver holds while the panel runs, so a block is claimed only by an init that succeeded; the sibling claim guard reads it.
    LedHwBlock hwBlock() const override {
        return running_ ? output_->hwBlock() : LedHwBlock::None;
    }

    /// Bind the rendered frame this driver corrects from.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// No window: the panels open at the layer's full size and take its lights in one pass.
    bool hasWindowControls() const override { return false; }

    /// Publish the Pi's panel settings where a Pi can run, the output's wiring, the peripheral select, then the output's own controls, the output being the one the select names.
    void defineDriverControls() override {
        buildBackendOptions();
        selectOutput();
        if constexpr (platform::hasHub75Pio) addPanelControls();
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

    /// Stop the scan, release the panel's peripheral, and free the correction scratch; a close the kernel left unfinished leaves its reason as the status.
    void release() override {
        stopScan();
        DriverBase::release();
        if (const char* why = output_->warning()) setStatus(why, Severity::Warning);
    }

    /// Refuse a block a sibling holds, open the output, then size the correction scratch to its box.
    void prepare() override {
        stopScan();   // a reinit keeps the correction scratch, which release() frees
        if (!layer_) {
            setStatus("no layer", Severity::Warning);
            return;
        }
        if (const char* why = machineRefusal()) {
            setStatus(why, Severity::Error);
            return;
        }
        // One block per peripheral, so a sibling holding it means idle with a status rather than fight, the first to prepare keeping it.
        if (siblingClaims(output_->hwBlock())) {
            setConfigErr(kBlockInUse);
            return;
        }
        if (const char* why = output_->open(static_cast<uint16_t>(layer_->physicalWidth()),
                                            static_cast<uint16_t>(layer_->physicalHeight()))) {
            setStatus(why, Severity::Error);
            return;
        }
        // The output's box is fixed while it runs, so it is read here once and tick() reuses it.
        lights_ = output_->lights();
        // One whole frame of packed RGB: the output needs the frame, not one light at a time.
        ensureWire(lights_ * 3);
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
        showStatus();   // a rebuild before this prepare may have left the gains waiting for tables, which the status says too
    }

    /// Correct the frame into one RGB frame and hand it to the output.
    void tick() MM_NONBLOCKING override {
        if (!running_ || !sourceBuffer_ || !sourceBuffer_->data()) return;

        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t outCh = correction_.outChannels;
        const nrOfLightsType lights = static_cast<nrOfLightsType>(lights_);
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
        const size_t frameBytes = lights_ * 3;
        const bool rewired = outChannelsChanged();
        // A brightness change moves no buffer; a light scratch still missing retries at the same width.
        if (wireCap_ < frameBytes || rewired || lightScratchShort()) {
            if (parent()) parent()->quiesce();   // a resize frees the block the encode worker may be writing
            ensureWire(frameBytes);
            sizeLightScratch();
        }
        showStatus();   // the rebuild's verdicts take their place in the order
    }

    /// Let the output publish its measured readouts, then show the status its warning and the base's verdicts decide; MM_NONBLOCKING stays, or the base's contract widens and the compiler refuses.
    void tick1s() MM_NONBLOCKING override {
        if (!running_) return;
        output_->tick1s();
        showStatus();
    }

private:
    /// The one status the running panel shows, the same one every call, in this order: a missing light scratch, the output's warning in full, the gains waiting for tables, then the output's describe() while all is healthy.
    void showStatus() MM_NONBLOCKING {
        if (lightScratchShort()) setStatus(kNoLightMemory, Severity::Error);
        else if (const char* warn = output_->warning()) setStatus(warn, Severity::Warning);
        else if (gainsDropped_) setStatus(kNoGainMemory, Severity::Warning);
        else setStatus(output_->describe(), Severity::Status);
    }

    /// Stop the scan and release the panel's peripheral, the reinit both release() and prepare() begin with.
    void stopScan() {
        output_->close();
        running_ = false;
    }

    /// Why this machine refuses the HUB75 output, or null; an earlier Pi's reason names the Pi 5's PIO. One case per machine.
    static const char* machineRefusal() {
        switch (platform::machine()) {
            case platform::Machine::PiGpio: return "HUB75 in this build needs a Pi 5's PIO";
            case platform::Machine::Esp32: return nullptr;
            case platform::Machine::Pi5: return nullptr;   // the PIO refuses for itself, with its reason
            case platform::Machine::Other: return nullptr;   // the bus output refuses for itself
        }
        return nullptr;
    }

    /// The output a backend drives: the Pi's has its own, and LCD_CAM and Parlio share the bus output.
    Hub75Output* outputFor(platform::Hub75Backend backend) {
        if (backend == platform::Hub75Backend::Pio) {
            if (Hub75Output* pio = asHub75Output(pioOutput_)) return pio;
        }
        return &busOutput_;
    }

    /// Point the driver at the output the selected backend drives, when that is not the one it holds; LCD_CAM and Parlio share the bus output, which keeps its state across them.
    void selectOutput() {
        Hub75Output* want = outputFor(selectedBackend());
        if (want == output_) return;
        MoonModule::notifyQuiesceRender();   // the encode worker reads output_ in tick()
        stopScan();
        output_ = want;
        // The depth is one setting for both outputs, so it moves into the new output's range.
        const DepthRange range = output_->depthRange();
        bitDepth = std::clamp(bitDepth, range.min, range.max);
    }

    /// Add the Pi's panel and clock settings, hidden while another output runs: they stay bound, so a saved card keeps them.
    void addPanelControls() {
        const bool shown = output_->hwBlock() == LedHwBlock::PiHeader;
        controls_.addControl("rows", panelRows, 16, 64);      // 16 rows scan at 1/8, the fewest the engine drives; 64 is the panel the board is built for
        controls_.setHidden(controls_.count() - 1, !shown);
        controls_.addControl("cols", panelCols, 16, 512);     // 16 is the narrowest panel; 512 by kPanelsPerLaneMax keeps the canvas width, 16,384, inside 16 bits
        controls_.setHidden(controls_.count() - 1, !shown);
        controls_.addControl("panelsPerLane", panelsPerLane, 1, kPanelsPerLaneMax);
        controls_.setHidden(controls_.count() - 1, !shown);
        controls_.addControl("pixelClock", pixelClockKhz, 1000, 40000);   // 1 to 40 MHz: below 1 MHz the wall crawls, above 40 the divider bottoms out
        controls_.setHidden(controls_.count() - 1, !shown);
        controls_.addControl("latchBlank", latchBlankWords, 0, 8);   // extra dark words before each latch, for a panel that ghosts
        controls_.setHidden(controls_.count() - 1, !shown);
    }

    /// Offer the backends the machine has, one case per machine: re-pointing by label, so a dropped backend cannot select another.
    void buildBackendOptions() {
        const char* current = (peripheralSel_ < backendOptionCount_)
                                  ? backendOptions_[peripheralSel_] : nullptr;
        backendOptionCount_ = 0;
        switch (platform::machine()) {
            case platform::Machine::Pi5:   // the PIO is offered whether or not it opens, so a refused boot keeps its card
                offer(platform::Hub75Backend::Pio);
                break;
            case platform::Machine::PiGpio:   // no output: the Pi card, labeled as none
                backendOptions_[0] = "(none)";
                backendIndex_[0] = platform::Hub75Backend::Pio;
                backendOptionCount_ = 1;
                break;
            case platform::Machine::Esp32:
            case platform::Machine::Other:
                offerBus();
                break;
        }
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < backendOptionCount_; k++) {
                if (std::strcmp(backendOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        peripheralSel_ = sel;
    }

    /// Add `backend` to the offer under its label.
    void offer(platform::Hub75Backend backend) {
        backendOptions_[backendOptionCount_] = platform::hub75BackendLabel(backend);
        backendIndex_[backendOptionCount_] = backend;
        backendOptionCount_++;
    }

    /// Offer the bus backends this silicon has and that can carry this geometry, or "(none)" when there are none (a host, a classic ESP32).
    void offerBus() {
        // Zero while the layer is unknown, which asks only whether the silicon exists.
        const size_t bytes = layer_
            ? output_->frameBytes(static_cast<uint16_t>(layer_->physicalWidth()),
                                  static_cast<uint16_t>(layer_->physicalHeight()))
            : 0;
        for (uint8_t i = 0; i < kBackendCount; i++) {
            const auto b = static_cast<platform::Hub75Backend>(i);
            if (platform::hub75BackendAvailable(b, bytes)) offer(b);
        }
        if (backendOptionCount_ == 0) {
            backendOptions_[0] = "(none)";
            backendIndex_[0] = platform::Hub75Backend::LcdCam;
            backendOptionCount_ = 1;
        }
    }

    /// The output's lines for the pin map, since their hidden controls read as free there.
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        return output_->fixedPins(out, max);
    }

    static constexpr uint8_t kBackendCount = 2;   // the bus backends, LCD_CAM and Parlio
    const char* backendOptions_[kBackendCount + 1] = {};
    platform::Hub75Backend backendIndex_[kBackendCount + 1] = {};
    uint8_t backendOptionCount_ = 0;

    /// The two outputs, held by value so the driver's size and memory readout count them; the PIO output is an empty type where `hasHub75Pio` is false.
    Hub75BusOutput busOutput_;
    std::conditional_t<platform::hasHub75Pio, Hub75PioOutput, NoPioOutput> pioOutput_;
    Hub75Output* output_ = &busOutput_;   // the strategy every call goes through
    Buffer* sourceBuffer_ = nullptr;
    size_t lights_ = 0;   ///< the output's lights() as prepare() read it, the length of the frame tick() hands over
    bool running_ = false;
};

}  // namespace mm
