// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "light/drivers/Hub75PiOutput.h"   // the panel geometry, the fit and the lane canvas both Pi outputs share
#include "platform/Hub75Gpio.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace mm {

/// The HUB75 driver's output on a Raspberry Pi 3/4's header, through the CPU bit-bang + PWM OE pulser engine: it hands the corrected frame to the GPIO seam, behind which a realtime thread clocks the panels (why not DMA: platform/rpi/hub75_gpio_backend.h); the engine scales the codes by the brightness (@xref{brightness-and-the-curve|brightness and the curve}).
///
/// Its pins are the active 3-channel board's (hub75_board_pins.h). The defaults are a 256x192 wall: 128x64 panels, 2 to a lane, 3 lanes in parallel, stacked to 256x192 at 1/32 scan and 6-bit, lit at a 130 ns LSB; the card offers 2 to 8 planes (@xref{8-bit-color|8-bit color}).
///
/// **The scan** follows the rows, so no scan control exists (@xref{scan}).
///
/// **Multiplexing.** An outdoor panel whose rows are wired through an internal multiplexer takes `multiplexing` Stripe, which reshapes the frame before the encoder: a 32x64 panel is driven as 16 rows x 128 columns at 1/8 scan (@xref{multiplexing}).
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Scan
///
/// The scan follows the rows, rows/2: a 64-row panel is 1/32, a 32-row one 1/16. Under Stripe the scan is the driven panel's, rows/4: a 32-row Stripe panel is 1/8.
///
/// No scan control exists, so the wall cannot come up striped from a stale default: the Pi 5 wall came up with black stripes from a 1/16 default on 1/32 panels. A rows value whose scan is not 8, 16 or 32 is refused with the status naming the rows that work.
///
/// ## Multiplexing
///
/// Stripe has stretch factor 2: a 32x64 panel is driven as 16 rows x 128 columns, rows 0..7 and 16..23 folding to columns 64..127, at 1/8 scan (hub75_gpio_multiplex.h).
///
/// The mapping is hzeller's Stripe algorithm, reimplemented from the algorithm with attribution (see that header's license caveat), and BENCH-V5 verified it on the Pi 4 sign: the test pattern renders right, red rows top to bottom, green columns left to right, held row 0 at top.
///
/// The mapping is one entry of the multiplex table there, normal first and Stripe second; a further mapping is a further entry, and the `multiplexing` options mirror the table's order.
///
/// ## The refresh
///
/// The frame is `bitDepth * scanRows * laneWidth` columns at the bench-calibrated column time (hub75GpioPredictedColumnTimeNs) plus the dark intervals, and a long LSB makes the top planes OE-bound instead; the rates, predicted and measured, are in docs/reference/hardware/hub75-bench.md.
///
/// The arrangement rate uses one column step per pixel at the engine's model through the seam (`hub75GpioColumnTimeNs`), auto quoting the slowdown the engine resolves for this host, and 0.5 us of dark interval per row for the address and latch.
///
/// ## 8-bit color
///
/// The card offers 2 to 8 planes, starting at 6; at 8 the pulser, not the encoder's OE bits, lights every plane at its true binary weight, so a narrow row's hold words cost only the stride (docs/reference/hardware/hub75-panels.md).
///
/// ## Brightness and the curve
///
/// The correction's table bakes the curve at full values and the engine scales the codes by the brightness, so every plane keeps its full lit time; `carryBits` carries that many bits of the fraction each pixel loses to its next refresh, 0 off (hub75_gpio_dither.h).
///
/// ## Slowdown
///
/// The card asks for -1 (auto) or 0..4. The engine resolves auto by model at start(), and the rate prediction resolves it the same way through the seam, so both quote one slowdown.
///
/// The card stores the select's INDEX: `kSlowdown2` is slowdown 2, and its stored value is 3. The slowdownTrue readout publishes the resolved value, so a bench note quotes the slowdown the engine runs (BENCH-V6).
class Hub75GpioOutput final : public Hub75PiOutput {
public:
    /// The lane canvas is `owner`'s memory, counted in its readout and freed on its release.
    explicit Hub75GpioOutput(MoonModule& owner) : Hub75PiOutput(owner, "GPIO") {}

    /// The slowdown select's options, in order: auto takes the engine's own choice for the model (Pi 4: 2, Pi 3: 1), and 0 to 4 slow the column writes for a panel that wants more setup time.
    enum : uint8_t { kSlowdownAuto, kSlowdown0, kSlowdown1, kSlowdown2, kSlowdown3, kSlowdown4, kSlowdownCount };
    /// The labels `slowdownSel` indexes.
    static constexpr const char* kSlowdownOptions[kSlowdownCount] = {"auto", "0", "1", "2", "3", "4"};

    uint8_t  multiplexingSel = platform::kHub75MultiplexNormal;   ///< An outdoor panel's scan wiring
    uint16_t pwmLsbNs = platform::Hub75GpioOptions{}.pwmLsbNs;    ///< The shortest lit time in nanoseconds
    uint8_t  carryBits = platform::kHub75GpioCarryBitsDefault;    ///< Bits of each pixel's lost shade carried to its next refresh, 0 off
    uint8_t  slowdownSel = kSlowdownAuto;                         ///< Slows the GPIO for a fast Pi; auto is the model's own

    /// The seam's options and the frames shown each second, below the panel's rows, columns and panels per lane.
    void addSeamControls(ControlList& controls) override {
        controls.addSelect("multiplexing", multiplexingSel, platform::kHub75GpioMultiplexNames, platform::kHub75MultiplexCount);   // normal is direct, Stripe drives the panel stretched (half the rows, double the columns), as `--led-multiplexing` does
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        controls.addSelect("clockEdge", owner_->clockEdgeSel, Hub75Settings::kEdgeOptions, Hub75Settings::kEdgeCount);
        controls.addControl("carryBits", carryBits, 0, platform::kHub75GpioCarryBitsMax);   // the pattern repeats within 2^bits encodes (hub75_gpio_dither.h)
        controls.addControl("pwmLsbNs", pwmLsbNs, platform::kHub75GpioPwmLsbMinNs, platform::kHub75GpioPwmLsbMaxNs);   // shorter is faster and thinner at the low end, longer steadies a dim panel; the PWM clock quantizes it (hub75_gpio_pwm.h)
        controls.addSelect("slowdown", slowdownSel, kSlowdownOptions, kSlowdownCount);   // stores the INDEX, not the slowdown (slowdownRequested, the slowdownTrue readout)
        // The slowdown the engine runs, resolved through the seam (auto takes the model's own), since the card stores the select's INDEX.
        controls.addReadOnly("slowdownTrue", slowdownTrueStr_, sizeof(slowdownTrueStr_));
        controls.addReadOnly("shown", shownStr_, sizeof(shownStr_));
    }

    /// Every GPIO option but `carryBits` reopens the panels, and the read-only rate does not. The scan is not listed: it follows the rows, and rows reopens.
    bool affectsPrepare(const char* name) const override {
        static const char* const kReopen[] = {
            "rows", "cols", "panelsPerLane", "multiplexing", "bitDepth", "clockEdge", "pwmLsbNs", "slowdown",
        };
        return isOneOf(name, kReopen);
    }

    /// The options as the platform takes them.
    platform::Hub75GpioOptions options() const {
        platform::Hub75GpioOptions o;
        fillOptions(o);
        o.scanRate = scanRate();
        o.multiplexing = multiplexingSel;
        o.pwmLsbNs = pwmLsbNs;
        o.slowdown = slowdownRequested();
        return o;
    }

    /// Open the panels, then show the slowdown the engine runs.
    const char* open(uint16_t width, uint16_t height) override {
        const char* why = Hub75PiOutput::open(width, height);
        if (!why) std::snprintf(slowdownTrueStr_, sizeof(slowdownTrueStr_), "%u", static_cast<unsigned>(resolvedSlowdown()));
        return why;
    }

    /// Stop the refresh and darken the panels, with no slowdown to show.
    void close() override {
        Hub75PiOutput::close();
        slowdownTrueStr_[0] = '\0';
    }

protected:
    /// The rows that give a scan the engine drives, 8, 16 or 32 at rows / 2 (rows/4 under Stripe): the status names the rows that work.
    const char* optionsRefusal() const override {
        if (platform::hub75GpioRowsDriveScan(multiplexingSel, owner_->panelRows)) return nullptr;
        static const char* const kRowsRefusal[platform::kHub75MultiplexCount] = {
            "the scan is half the panel's rows, so rows must be 16, 32 or 64",
            "the Stripe scan is a quarter of the panel's rows, so rows must be 32 or 64",
        };
        return kRowsRefusal[std::min<uint8_t>(multiplexingSel, platform::kHub75MultiplexCount - 1)];
    }

    /// The panel as the rate counts it: rows/2 address steps of cols, or rows/4 of cols*2 under Stripe, at the engine's column time (@xref{the-refresh}).
    PanelSpec panelSpec(uint8_t bits) const override {
        // The dump skips the encoder's hold words, since the pulser sets the lit time, so the rate counts the shift words alone at every depth.
        const uint16_t shiftTenths = static_cast<uint16_t>(platform::hub75GpioPredictedColumnTimeNs(resolvedSlowdown()) * 10);
        const uint16_t darkTenths = static_cast<uint16_t>(platform::kHub75GpioDarkIntervalNs * 10);
        PanelSpec panel{owner_->panelRows, owner_->panelCols, scanRate(), bits, pwmLsbNs, shiftTenths, darkTenths};
        panel.rateCols = static_cast<uint16_t>(platform::hub75GpioDrivenCols(multiplexingSel, owner_->panelCols));   // the card holds cols to 512, so the driven width fits
        return panel;
    }

    bool openSeam(uint16_t& width, uint16_t& height) override {
        if (!platform::hub75GpioOpen(options())) return false;
        width = platform::hub75GpioWidth();
        height = platform::hub75GpioHeight();
        return true;
    }
    const char* seamError() const override { return platform::hub75GpioLastError(); }
    const char* seamWarning() const override { return platform::hub75GpioWarning(); }
    const char* seamCloseError() const override { return platform::hub75GpioCloseError(); }
    void closeSeam() override { platform::hub75GpioClose(); }
    uint16_t framesShown() const MM_NONBLOCKING override { return platform::hub75GpioFps(); }
    uint32_t submitErrors() const MM_NONBLOCKING override { return platform::hub75GpioSubmitErrors(); }
    bool seamStalled() const MM_NONBLOCKING override { return platform::hub75GpioStalled(); }
    /// The engine scales its codes by the brightness, and carries `carryBits` of each pixel's lost shade to its next refresh.
    void applyBrightness(uint8_t brightness) MM_NONBLOCKING override { platform::hub75GpioCorrection(brightness, carryBits); }
    void push(const uint8_t* rgb) MM_NONBLOCKING override { platform::hub75GpioPush(rgb); }

private:
    /// The slowdown the card asks for: -1 for auto, else 0..4, from the select's index: @xref{slowdown}.
    int8_t slowdownRequested() const {
        return slowdownSel == kSlowdownAuto || slowdownSel >= kSlowdownCount ? -1 : static_cast<int8_t>(slowdownSel - kSlowdown0);
    }

    /// The slowdown the engine runs on this host: the request, or the model's own for auto.
    uint8_t resolvedSlowdown() const {
        return static_cast<uint8_t>(platform::hub75GpioResolvedSlowdown(slowdownRequested()));
    }

    /// The scan the rows wire: rows/2, or rows/4 under Stripe: @xref{scan}.
    uint8_t scanRate() const {
        return platform::hub75GpioScanRate(multiplexingSel, owner_->panelRows);
    }

    char slowdownTrueStr_[4] = "";   ///< The resolved slowdown as one digit, empty while closed
};

// The Pi settings the driver holds start from the PIO seam's defaults (Hub75Settings::kPiDefaults), and the GPIO seam's must match them.
static_assert(platform::Hub75GpioOptions{}.rows == Hub75Settings::kPiDefaults.rows && platform::Hub75GpioOptions{}.cols == Hub75Settings::kPiDefaults.cols &&
                  platform::Hub75GpioOptions{}.panelsPerLane == Hub75Settings::kPiDefaults.panelsPerLane,
              "the two Pi seams default to one wall");

} // namespace mm
