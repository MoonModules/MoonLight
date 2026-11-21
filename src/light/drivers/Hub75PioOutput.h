// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "light/drivers/Hub75PiOutput.h"   // the panel geometry, the fit and the lane canvas
#include "platform/Hub75Pio.h"

#include <cstdint>

namespace mm {

/// The HUB75 driver's output on a Raspberry Pi 5's header, through the RP1's PIO: it hands the corrected frame to the PIO seam, behind which a state machine clocks the panels; the engine dims by lit time (@xref{brightness-and-the-curve|brightness and the curve}).
///
/// Its pins are the active 3-channel board's (hub75_pio_pins.h). The defaults are a 256x192 wall: 128x64 panels, 2 to a lane, 3 lanes in parallel, stacked to 256x192 at 1/32 scan and 6-bit, clocked at 10 MHz.
///
/// The scan derives from half the panel's rows (@xref{scan-rate|scan rate}), every rate the card quotes is a computed wire rate (@xref{the-refresh|the refresh}), and 8-bit color is opt-in.
///
/// @attribution pico-examples, pimoroni-pico
///
/// @technical
///
/// ## The refresh
///
/// The frame is `bitDepth * scanRows * laneWidth` pixel clocks. Every rate this card quotes is that computed wire rate: 203.5 Hz on the wall at 10 MHz, where `shown` measured 169 fps on kernel 6.18 and 198 on 6.12, as the DMA feed falls behind the wire by a margin that differs by kernel. The rates at other clocks are in docs/reference/hardware/hub75-bench.md.
///
/// The card offers 2 to 8 planes, starting at 6; the cost and encoding of the 8-bit setting are in docs/reference/hardware/hub75-panels.md.
///
/// A pixel clock above the benched 20 MHz warns, failed submits show as a warning while they rise, a submit that has not returned shows as a stall, and a close the kernel left unfinished says why.
///
/// ## Scan rate
///
/// The scan is rows / 2, so the card carries no `scanRate` control and a 64-row wall is 1/32 on its own; a 1/16 scan on that wall stripes it (bench 2026-11-13).
///
/// That shuts out a 32-row panel scanned 1/8 and a 64-row panel scanned 1/16, which the engine could drive. Only 16, 32 and 64 rows open; any other count is refused at open with the status line naming the rows that work.
///
/// ## Brightness and the curve
///
/// The engine dims by lit time, so the correction keeps full values and bakes the curve itself; the seam's brightness is the global level alone.
class Hub75PioOutput final : public Hub75PiOutput {
public:
    /// The lane canvas is `owner`'s memory, counted in its readout and freed on its release.
    explicit Hub75PioOutput(MoonModule& owner) : Hub75PiOutput(owner, "PIO") {}

    /// The depth, the clock edge and the frames shown each second, below the peripheral select; the panel's rows, columns, panels per lane, clock and latch blank are the driver's.
    void addSeamControls(ControlList& controls) override {
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        controls.addSelect("clockEdge", owner_->clockEdgeSel, Hub75Settings::kEdgeOptions, Hub75Settings::kEdgeCount);
        controls.addReadOnly("shown", shownStr_, sizeof(shownStr_));
    }

    /// Every PIO option reopens the panels; the read-only rate does not.
    bool affectsPrepare(const char* name) const override {
        static const char* const kReopen[] = {
            "rows", "cols", "panelsPerLane", "bitDepth", "clockEdge", "pixelClock", "latchBlank",
        };
        return isOneOf(name, kReopen);
    }

    /// The options as the platform takes them.
    platform::Hub75PioOptions options() const {
        platform::Hub75PioOptions o;
        fillOptions(o);
        o.scanRate = scanRate();
        o.pixelClockHz = static_cast<uint32_t>(owner_->pixelClockKhz) * 1000;
        o.latchBlankWords = owner_->latchBlankWords;
        return o;
    }

protected:
    /// The rows that give a scan the engine drives, 8, 16 or 32 at rows / 2: 16, 32 and 64.
    const char* optionsRefusal() const override {
        const uint8_t rows = owner_->panelRows;
        if (rows == 16 || rows == 32 || rows == 64) return nullptr;
        return "the scan is half the panel's rows, so rows must be 16, 32 or 64";
    }

    /// The panel as the rate counts it: address steps of rows / 2, each one shift of the lane, at the pixel clock.
    PanelSpec panelSpec(uint8_t bits) const override {
        // The shift is one pixel clock a pixel, the latch rides the last one, and no lit time outlasts a row's shift.
        const uint8_t rows = owner_->panelRows;
        const uint16_t scanRows = rows > 1 ? static_cast<uint16_t>(rows / 2) : 1;
        const uint16_t shiftTenths = owner_->pixelClockKhz ? static_cast<uint16_t>(10'000'000u / owner_->pixelClockKhz) : 10000;
        PanelSpec panel{rows, owner_->panelCols, scanRows, bits, 0, shiftTenths, 0};
        panel.pairs = 1;   // a scan of rows / 2 pairs one row of the upper half with one of the lower
        panel.encoderHold = true;   // the frame is the encoder's: a narrow row's hold words count as shift
        return panel;
    }

    bool openSeam(uint16_t& width, uint16_t& height) override {
        if (!platform::hub75PioOpen(options())) return false;
        width = platform::hub75PioWidth();
        height = platform::hub75PioHeight();
        return true;
    }
    const char* seamError() const override { return platform::hub75PioLastError(); }
    const char* seamWarning() const override { return platform::hub75PioWarning(); }
    const char* seamCloseError() const override { return platform::hub75PioCloseError(); }
    void closeSeam() override { platform::hub75PioClose(); }
    uint16_t framesShown() const MM_NONBLOCKING override { return platform::hub75PioFps(); }
    uint32_t submitErrors() const MM_NONBLOCKING override { return platform::hub75PioSubmitErrors(); }
    bool seamStalled() const MM_NONBLOCKING override { return platform::hub75PioStalled(); }
    /// The engine dims by lit time, so the seam's brightness is the global level alone.
    void applyBrightness(uint8_t brightness) MM_NONBLOCKING override { platform::hub75PioCorrection(brightness); }
    void push(const uint8_t* rgb) MM_NONBLOCKING override { platform::hub75PioPush(rgb); }

private:
    /// The scan rate the panel's rows imply, rows / 2: 64 rows is 1/32, 32 rows 1/16, 16 rows 1/8 (@xref{scan-rate|scan rate}).
    uint8_t scanRate() const {
        return owner_->panelRows > 1 ? static_cast<uint8_t>(owner_->panelRows / 2) : 1;
    }
};

} // namespace mm
