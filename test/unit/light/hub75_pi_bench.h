#pragma once
/// What the Pi outputs' tests share: the bench a driver is wired on, each seam's view of what its output opened and handed over, and a rig that runs one driver on a Pi output.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "correction_presets.h"
#include "doctest.h"
#include "hub75_port.h"   // PioPortGuard, PioHost, PioAvailable, optionIndex
#include "light/drivers/Correction.h"
#include "light/drivers/Hub75Driver.h"
#include "light/drivers/Hub75GpioOutput.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/PanelsLayout.h"
#include "platform/Hub75Gpio.h"
#include "platform/Hub75Pio.h"
#include "platform/platform_base.h"   // the target's flags and MM_NONBLOCKING

namespace mm {

/// A Hub75Driver on a layer of the layouts a case adds, wired as production wires it; declared in destruction-safe order, the layouts before what reads them.
struct Hub75Bench {
    mm::Layouts layouts;         ///< the layouts the layer reads
    mm::GridLayout grid;         ///< a raster, for a case that adds it
    mm::PanelsLayout panels;     ///< panels in cabling order, for a case that adds it
    mm::Layer layer;             ///< the layer the driver reads
    mm::Buffer source;           ///< the rendered frame the driver corrects
    mm::Hub75Driver driver;      ///< the driver under test

    /// Release the driver as its owner does before destroying it, while the layer and the frame it reads still stand, so a case that ends without release() leaves nothing allocated.
    ~Hub75Bench() { driver.release(); }

    /// Size the layer from the layouts added, hold `lights` lights of RGB, and bind the driver to both.
    void wire(mm::nrOfLightsType lights) {
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
        REQUIRE(source.allocate(lights, 3));
        driver.setLayer(&layer);
        driver.setSourceBuffer(&source);
    }

    /// Open the panels with a linear, full-brightness correction, built in place, so a frame's values read straight.
    void open() {
        mm::rebuildFromPreset(driver.correctionForTest(), 255, mm::PresetOrder::RGB);
        driver.applyState();
        driver.prepare();
    }

    /// Paint light `i` in the rendered frame.
    void paint(mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
};

/// The text a read-only text control of `m` shows, empty when there is none.
inline std::string textOf(mm::MoonModule& m, const char* name) {
    const int i = mm::controlIndex(m, name);
    if (i < 0) return {};
    const char* text = static_cast<const char*>(m.controls()[static_cast<uint8_t>(i)].ptr);
    return text ? text : "";
}

/// Pick the backend labeled `label` on `driver`'s peripheral select, as the card does, and rebuild the card for it; false when it is not offered.
inline bool pickBackend(mm::Hub75Driver& driver, const char* label) {
    driver.defineControls();   // the card as offered, so the select lists what the host has
    const int i = mm::optionIndex(driver, "peripheral", label);
    if (i < 0) return false;
    driver.peripheralSel_ = static_cast<uint8_t>(i);
    driver.rebuildControls();   // as the scheduler does on a peripheral change, which swaps the output
    return true;
}

/// Make the host answer as `kind` and rebuild `driver`'s card, as a changed machine does: the driver follows the machine to its output.
inline void becomeMachine(mm::Hub75Driver& driver, mm::platform::Machine kind) {
    mm::platform::setTestMachine(kind);
    driver.rebuildControls();
}

/// What the PIO output's tests read from its seam: the output's type and backend, the guard that records, the name the status gives it, and what the seam kept and holds.
struct PioSeam {
    using Output = mm::Hub75PioOutput;                                                       ///< the output under test
    using RecordGuard = mm::PioPortGuard;                                                    ///< the guard that makes the seam record
    static constexpr const char* backendLabel = "Pi PIO";                                    ///< the label the peripheral select offers it under
    static constexpr const char* label = "PIO";                                              ///< how the status and the board's refusals name it
    static const mm::platform::Hub75PioOptions& options() { return mm::platform::hub75PioTestPort().options; }   ///< the options the output opened the engine with
    static void setStalled(bool stalled) { mm::platform::setTestHub75PioStalled(stalled); }                    ///< make the recorder answer a stalled submit, or none
    static void setSubmitErrors(uint32_t count) { mm::platform::setTestHub75PioSubmitErrors(count); }          ///< make the recorder answer `count` failed submits
    static void setFps(uint16_t hz) { mm::platform::setTestHub75PioFps(hz); }                                  ///< make the recorder answer a frame rate
    static int brightness() { return mm::platform::hub75PioTestPort().brightness; }                            ///< the brightness the output last handed the seam
    static uint16_t width() { return mm::platform::hub75PioWidth(); }                         ///< the canvas width the seam holds, 0 when closed
    static uint16_t height() { return mm::platform::hub75PioHeight(); }                       ///< the canvas height the seam holds, 0 when closed
    static int frameCount() { return static_cast<int>(mm::platform::hub75PioTestPort().frames); }   ///< frames the seam took since it opened
    /// Pixel (x, y) of the last frame the seam took, three bytes.
    static const uint8_t* pixel(uint16_t x, uint16_t y) {
        return mm::platform::hub75PioTestFrame() + (static_cast<size_t>(y) * width() + x) * 3;
    }
};

/// What the GPIO output's tests read from its seam, as PioSeam gives the PIO output's: the same members under the GPIO names.
struct GpioSeam {
    using Output = mm::Hub75GpioOutput;                                                      ///< the output under test
    using RecordGuard = mm::GpioPortGuard;                                                   ///< the guard that makes the seam record
    static constexpr const char* backendLabel = "Pi GPIO";                                   ///< the label the peripheral select offers it under
    static constexpr const char* label = "GPIO";                                             ///< how the status and the board's refusals name it
    static const mm::platform::Hub75GpioOptions& options() { return mm::platform::hub75GpioTestPort().options; }   ///< the options the output opened the engine with
    static void setStalled(bool stalled) { mm::platform::setTestHub75GpioStalled(stalled); }                   ///< make the recorder answer a stalled dump, or none
    static void setSubmitErrors(uint32_t count) { mm::platform::setTestHub75GpioSubmitErrors(count); }         ///< make the recorder answer `count` failed dumps
    static void setFps(uint16_t hz) { mm::platform::setTestHub75GpioFps(hz); }                                 ///< make the recorder answer a frame rate
    static int brightness() { return mm::platform::hub75GpioTestPort().brightness; }                           ///< the brightness the output last handed the seam
    static uint16_t width() { return mm::platform::hub75GpioWidth(); }                        ///< the canvas width the seam holds, 0 when closed
    static uint16_t height() { return mm::platform::hub75GpioHeight(); }                      ///< the canvas height the seam holds, 0 when closed
    static int frameCount() { return static_cast<int>(mm::platform::hub75GpioTestPort().frames); }   ///< frames the seam took since it opened
    /// Pixel (x, y) of the last frame the seam took, three bytes.
    static const uint8_t* pixel(uint16_t x, uint16_t y) {
        return mm::platform::hub75GpioTestFrame() + (static_cast<size_t>(y) * width() + x) * 3;
    }
};

/// A Hub75Driver on one raster or on panels in cabling order, on the heap, on the Pi output `Seam` names, picked by its backend label, not by its offered position.
template <class Seam>
struct PiRig : Hub75Bench {
    /// One raster of `width` by `height`, as a single panel's layout is.
    PiRig(uint16_t width, uint16_t height, mm::nrOfLightsType lights) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        finish(lights);
    }

    /// `across` by `down` panels of `panelW` by `panelH`, chained row by row, each walked `wiring` (0 rows, 1 columns) with `snake` inside: off is what a chained card takes.
    PiRig(uint16_t across, uint16_t down, uint16_t panelW, uint16_t panelH, uint8_t wiring, bool snake) {
        panels.horizontalPanels = across;
        panels.verticalPanels = down;
        panels.panelWidth = panelW;
        panels.panelHeight = panelH;
        panels.wiringOrder = wiring;
        panels.snake = snake;
        layouts.addChild(&panels);
        finish(static_cast<mm::nrOfLightsType>(across) * down * panelW * panelH);
        driver.panelRows = static_cast<uint8_t>(panelH);
        driver.panelCols = panelW;
    }

    /// A raster of `width` by `height` holding `lights` lights, or as many as the raster has when `lights` is 0.
    static std::unique_ptr<PiRig> raster(uint16_t width, uint16_t height, mm::nrOfLightsType lights = 0) {
        return std::make_unique<PiRig>(width, height, lights ? lights : static_cast<mm::nrOfLightsType>(width) * height);
    }

    /// A Panels layout in cabling order, each panel in plain rows unless `snake`.
    static std::unique_ptr<PiRig> cabled(uint16_t across, uint16_t down, uint16_t panelW, uint16_t panelH, bool snake = false) {
        return std::make_unique<PiRig>(across, down, panelW, panelH, uint8_t{0}, snake);
    }

    /// The output the driver selected.
    typename Seam::Output& out() { return *static_cast<typename Seam::Output*>(driver.outputForTest()); }

private:
    void finish(mm::nrOfLightsType lights) {
        wire(lights);
        REQUIRE(pickBackend(driver, Seam::backendLabel));   // by label, not by position
        REQUIRE(driver.outputForTest()->hwBlock() == mm::LedHwBlock::PiHeader);
        driver.panelRows = 32;   // panels forming a 16x32 canvas, unless a case changes them
        driver.panelCols = 16;
        driver.panelsPerLane = 1;   // one panel per lane, unless a case puts several on one: the card's own default is 2, the wall's
    }
};

/// Open `r` on a linear curve, correct to `brightness`, and show a gray pixel of `value` as the first light: what a case reads from its seam after.
template <class Rig>
void showGrayAt(Rig& r, uint8_t brightness, uint8_t value) {
    r.open();
    REQUIRE(mm::setControlValue(r.driver, "curve", static_cast<uint8_t>(mm::Correction::Curve::Linear)));
    r.driver.rebuildCorrection(brightness);
    r.paint(0, value, value, value);
    r.driver.tick();
}

/// A light corrected through a profile of `n` channels, `roles` placing its color, shows its red, green and blue on the panel's first pixel.
template <class Seam>
void showsColorsThrough(const mm::ChannelRole* roles, uint8_t n) {
    auto r = PiRig<Seam>::raster(16, 32);
    r->open();
    r->driver.correctionForTest().rebuild(255, roles, n);
    r->driver.onCorrectionChanged();   // as the container calls it when the wiring changes
    r->paint(0, 10, 20, 30);
    r->driver.tick();
    REQUIRE(Seam::frameCount() == 1);
    CHECK(Seam::pixel(0, 0)[0] == 10); CHECK(Seam::pixel(0, 0)[1] == 20); CHECK(Seam::pixel(0, 0)[2] == 30);
}

}  // namespace mm

TYPE_TO_STRING_AS("Pi PIO", mm::PioSeam);
TYPE_TO_STRING_AS("Pi GPIO", mm::GpioSeam);
