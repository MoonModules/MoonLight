// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/ScratchBuffer.h"   // the lane canvas, accounted to the driver and freed with it
#include "light/drivers/Hub75Canvas.h"   // where a light of a panel-by-panel layout sits on the lane canvas, and the plain-rows walk
#include "light/drivers/Hub75Fit.h"      // whether the layout's panels cable into the lanes, and the way out when they do not
#include "light/drivers/Hub75Output.h"
#include "light/layers/Layer.h"          // the layout's lights, which the fit and the plain-rows walk read
#include "light/layouts/Layouts.h"       // where each light sits, which the plain-rows walk reads

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// What a Raspberry Pi output shares: the panel geometry, the fit of the layout to it, the lane canvas, and the handover of a frame to a seam.
///
/// **The layout sets the geometry.** `panelRows` and `panelCols` are one panel's, `panelsPerLane` the panels in series on one lane, all settings of the driver, and the layout's lights, cut into whole panels, are the panels; the lanes driven at once follow, bounded by the board's three.
///
/// The subclass names its seam: the panel's timing, the call that opens the engine, the one that feeds it, the one that closes it, and the controls only that engine has. While the panels run it shows their rate and failing submits (@xref{running|Running}). The ESP32 bus output has no panel geometry and inherits none of this.
///
/// **The wiring is fixed,** the active 3-channel board's: three lanes of HUB75 data lines on the Pi's header, each lane up to 32 panels in series, so the card carries no pin controls and no wiring row.
///
/// @technical
///
/// ## Opening
///
/// open() is the one sequence the seam follows: close, cut the layout into panels, refuse a geometry that does not cable into the lanes with the way out, open the seam, size the lane canvas, and format the status. The driver clamps the depth into the output's range when the output changes, so open() reads the depth as it is.
///
/// The subclass supplies the steps that differ through protected virtuals: panelSpec() for the panel's timing, openSeam() and seamError() for the engine, seamWarning() for what degrades it, and closeSeam().
///
/// ## The lane canvas
///
/// The seam's canvas is `panelCols * panelsPerLane` wide while the layout's lights run panel by panel, so with several panels per lane each panel row is copied to its place before the push; a layout of another count, or off plain rows, is the warning.
///
/// ## Running
///
/// While the panels run, tick1s() publishes the frames shown in the last second and watches the seam's failed submits: a count that rose in the last second joins the warning, so a dead panel does not read as a healthy one.
class Hub75PiOutput : public Hub75Output {
public:
    /// The lanes the board drives: three, one per HUB75 data path.
    static constexpr uint8_t kParallelMax = static_cast<uint8_t>(platform::kHub75Lanes);

    /// No controls of its own beyond the seam's: the panel's rows, columns and panels per lane are the driver's, shown there while this output runs.
    void addControls(ControlList& controls) override { addSeamControls(controls); }

    /// The wiring is fixed, so the output adds no wiring row above the peripheral select.
    void addWiringControls(ControlList& /*controls*/) override {}

    /// No control changes the card's shape.
    bool onControlChanged(const char* /*name*/) override { return false; }

    /// The planes the engine drives: 2 to the encoder's most.
    DepthRange depthRange() const override { return DepthRange{2, kHub75EncodeMaxBitDepth}; }

    /// A Pi output's canvas size follows its layout, not a frame size the platform sizes.
    size_t frameBytes(uint16_t /*width*/, uint16_t /*height*/) const override { return 0; }

    /// No line has a visible pin control to claim: the wiring is fixed and unlisted.
    uint8_t fixedPins(MoonModule::FixedPin* /*out*/, uint8_t /*max*/) const override { return 0; }

    /// Open the panels; the canvas they form is the frame, whatever the layout's box, and warnFit() compares the light counts (@xref{opening}).
    const char* open(uint16_t /*width*/, uint16_t /*height*/) override {
        close();
        if (const char* why = optionsRefusal()) return why;
        const uint8_t bits = owner_->bitDepth;

        // The layout sets the geometry: its lights, cut into whole panels, are the panels, and the card's panels per lane divide them into lanes.
        const uint32_t perPanel = static_cast<uint32_t>(owner_->panelRows) * owner_->panelCols;
        const uint32_t layoutLights = layer() ? layer()->physicalLightCount() : 0;
        const uint32_t panels = perPanel ? layoutLights / perPanel : 0;
        char board[16];
        std::snprintf(board, sizeof(board), "Pi %s", label_);
        const CardGeometry geometry{panelSpec(bits), owner_->panelsPerLane, kParallelMax, board};
        const GeometryFit fit = checkGeometry(panels, owner_->panelsPerLane, kParallelMax);
        message_[0] = '\0';
        if (fit != GeometryFit::Ok) {
            geometryMessage(fit, panels, geometry, message_, sizeof(message_));
            return message_;
        }
        derivedParallel_ = static_cast<uint8_t>(panels / owner_->panelsPerLane);

        if (!openSeam(canvasW_, canvasH_)) {
            // Copied: the seam's text lives in a string its next open rewrites, and the status keeps the pointer.
            const char* why = seamError();
            std::snprintf(message_, sizeof(message_), "%s", why ? why : "the panels would not open");
            return message_;
        }
        open_ = true;
        lastErrors_ = shownErrors_ = 0;   // the seam counts from this open
        stalled_ = false;   // a stall still there at the first tick shows, as the tick compares against this open's state
        placed_ = {owner_->panelRows, owner_->panelCols, owner_->panelsPerLane};
        // A lane of several panels is not a raster: the layout's lights run panel by panel, so the push reads a second frame they are placed on.
        if (owner_->panelsPerLane > 1 && !canvas_.resize(lights() * 3)) {
            close();
            return "not enough memory for the lane canvas";
        }
        std::snprintf(status_, sizeof(status_), "%ux%u, %u-bit, %s", static_cast<unsigned>(canvasW_), static_cast<unsigned>(canvasH_),
                      static_cast<unsigned>(bits), label_);
        warnFit(layoutLights);
        return nullptr;
    }

    /// The canvas and the depth, as open() formatted them.
    const char* describe() const MM_NONBLOCKING override { return status_; }

    /// What degrades the running panels, in full, none hiding another: the seam's warning, a light count other than the canvas's, the walk's panel, then failing submits; after a close that did not finish, why.
    const char* warning() const MM_NONBLOCKING override { return (open_ || closeStuck_) && message_[0] ? message_ : nullptr; }

    /// The panels' canvas, one light per pixel.
    size_t lights() const MM_NONBLOCKING override { return open_ ? canvasLights() : 0; }

    /// Hand the frame over, at the global brightness the engine dims by.
    void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING override {
        applyBrightness(brightness);
        if (canvas_.bytes() == 0) {
            push(rgb);
            return;
        }
        placeOnCanvas(rgb);
        push(canvas_.data());
    }

    /// Publish the frames shown in the last second, and fold failing submits into the warning.
    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        const uint16_t fps = framesShown();
        if (fps != lastShown_) {   // a changed value alone re-serializes on the 1 Hz tick
            lastShown_ = fps;
            mm::formatTo(shownStr_, sizeof(shownStr_), "%u fps", fps);
        }
        watchSubmits();
    }

    /// Stop the refresh and darken the panels; a close the seam could not finish leaves its reason as the warning.
    void close() override {
        if (open_) closeSeam();
        open_ = false;
        canvasW_ = canvasH_ = 0;
        canvas_.resize(0);
        shownStr_[0] = '\0';   // a closed panel shows an empty rate
        lastShown_ = 0xFFFF;
        message_[0] = '\0';
        const char* stuck = seamCloseError();
        closeStuck_ = stuck != nullptr;
        if (stuck) std::snprintf(message_, sizeof(message_), "%s", stuck);
        baseLen_ = std::strlen(message_);
    }

    /// The Pi's header, which one HUB75 driver per Pi drives; every Pi output claims this block, so one drives the header at a time.
    LedHwBlock hwBlock() const override { return LedHwBlock::PiHeader; }

protected:
    /// The frames the panels showed in the last second.
    char shownStr_[16] = "";

    /// The lane canvas is `owner`'s memory, counted in its readout and freed on its release; `label` names the seam in the status and its warnings ("PIO", "GPIO").
    Hub75PiOutput(MoonModule& owner, const char* label) : label_(label), canvas_(owner) {}

    /// Whether `name` is in `list`, for the controls that reopen the panels.
    template <size_t N>
    static bool isOneOf(const char* name, const char* const (&list)[N]) {
        for (const char* n : list) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    /// Fill the options the seam takes: the panel, the lanes the layout decides in open(), the depth and the clock edge.
    template <class Options>
    void fillOptions(Options& o) const {
        o.rows = owner_->panelRows;
        o.cols = owner_->panelCols;
        o.panelsPerLane = owner_->panelsPerLane;
        o.parallel = parallel();
        o.bitDepth = owner_->bitDepth;
        o.clockFallingEdge = owner_->clockEdgeSel == Hub75Settings::kEdgeFalling;
    }

    /// The lanes the layout's panels divide into, set by open(); one before it runs.
    uint8_t parallel() const { return derivedParallel_ ? derivedParallel_ : 1; }

    /// Why the settings cannot open the seam at all, in the settings' own words, or null.
    virtual const char* optionsRefusal() const { return nullptr; }
    /// Add the seam's controls: the depth, the edge and the `shown` rate last.
    virtual void addSeamControls(ControlList& controls) = 0;
    /// The panel as the seam clocks it, for the arrangements' rate and the message of a geometry that does not fit.
    virtual PanelSpec panelSpec(uint8_t bits) const = 0;
    /// Open the seam with the current options and answer the canvas it forms; false when it would not open.
    virtual bool openSeam(uint16_t& width, uint16_t& height) = 0;
    /// Why the seam did not open, or null.
    virtual const char* seamError() const = 0;
    /// What degrades the running seam, or null.
    virtual const char* seamWarning() const = 0;
    /// Why the seam's last close did not finish, or null.
    virtual const char* seamCloseError() const = 0;
    /// Stop the seam and release its engine.
    virtual void closeSeam() = 0;
    /// The frames the seam showed in the last second.
    virtual uint16_t framesShown() const MM_NONBLOCKING = 0;
    /// The submits the seam counted as failed since it opened.
    virtual uint32_t submitErrors() const MM_NONBLOCKING = 0;
    /// Whether a submit has been in flight for longer than the seam allows, so the panels hold their last frame.
    virtual bool seamStalled() const MM_NONBLOCKING = 0;
    /// Hand the seam the global brightness.
    virtual void applyBrightness(uint8_t brightness) MM_NONBLOCKING = 0;
    /// Hand the seam one frame of the canvas.
    virtual void push(const uint8_t* rgb) MM_NONBLOCKING = 0;

private:
    /// The layer the driver draws from.
    Layer* layer() const { return owner_->drivenLayer(); }

    /// The canvas's pixels, one light each, whether or not the panels are open.
    size_t canvasLights() const MM_NONBLOCKING { return static_cast<size_t>(canvasW_) * canvasH_; }

    /// Build the warning, each part whole and joined with "; ": the seam's, a light count other than the canvas's, then the walk's panel.
    void warnFit(uint32_t layoutLights) {
        detail::Text t{message_, sizeof(message_)};
        if (const char* seam = seamWarning()) t.add("%s", seam);
        if (layoutLights != lights())
            t.add("%slayout %u lights, panels %u", t.n ? "; " : "", static_cast<unsigned>(layoutLights), static_cast<unsigned>(lights()));
        const uint32_t off = offPanel();
        if (off != PanelWalk::kNone)
            t.add("%sthe layout's panel %u does not run in plain rows of %u: lay the panels out in cabling order, each in plain rows with snake off",
                  t.n ? "; " : "", static_cast<unsigned>(off + 1), static_cast<unsigned>(placed_.cols));
        baseLen_ = t.n < sizeof(message_) ? t.n : sizeof(message_) - 1;
    }

    /// A stalled submit, or failed submits that rose in the last second, join the warning after its other parts, and leave it when they stop; a stall, the graver, shows alone.
    void watchSubmits() MM_NONBLOCKING {
        const uint32_t errors = submitErrors();
        const bool stalled = seamStalled();
        const bool failing = errors != lastErrors_;
        const bool changed = stalled != stalled_ || failing != failing_ || (failing && !stalled && errors != shownErrors_);
        lastErrors_ = errors;
        if (!changed) return;
        stalled_ = stalled;
        failing_ = failing;
        shownErrors_ = errors;
        message_[baseLen_] = '\0';
        const char* sep = baseLen_ ? "; " : "";
        char* at = message_ + baseLen_;
        const size_t room = sizeof(message_) - baseLen_;
        if (stalled) mm::formatTo(at, room, "%sthe %s is stalled: a submit has not returned, and the panels hold their last frame", sep, label_);
        else if (failing) mm::formatTo(at, room, "%sthe %s is failing: %u submits failed since it opened, and the panels may be dark", sep, label_, static_cast<unsigned>(errors));
    }

    /// The first driven panel, counted from 0, whose lights leave plain rows, or PanelWalk::kNone; lights past the last whole panel are the count's.
    uint32_t offPanel() const {
        Layouts* layouts = layer() ? layer()->layouts() : nullptr;
        if (!layouts) return PanelWalk::kNone;
        struct Driven { PanelWalk walk; size_t lights; };
        Driven driven{PanelWalk(placed_.rows, placed_.cols), lights()};
        layouts->placeLights(CoordSink{[](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType) {
                                           auto* d = static_cast<Driven*>(c);
                                           if (i < d->lights) d->walk.light(i, x, y);
                                       },
                                       nullptr, &driven});
        return driven.walk.firstOffPanel();
    }

    /// Copy the corrected frame onto the canvas a panel row at a time: a panel's row is contiguous in both, so each row is one blit (@xref{the-lane-canvas}).
    void placeOnCanvas(const uint8_t* rgb) MM_NONBLOCKING {
        const size_t width = static_cast<size_t>(placed_.cols) * placed_.panelsPerLane;
        const size_t rowBytes = static_cast<size_t>(placed_.cols) * 3;
        const size_t n = canvasLights();   // show() runs while the panels are open, so this is lights() read directly
        for (size_t i = 0; i + placed_.cols <= n; i += placed_.cols) {
            const CanvasPos p = laneCanvasPos(static_cast<uint32_t>(i), placed_.rows, placed_.cols, placed_.panelsPerLane);
            const size_t to = (static_cast<size_t>(p.y) * width + p.x) * 3;
            if (to + rowBytes > canvas_.bytes()) continue;
            std::memcpy(canvas_.data() + to, rgb + i * 3, rowBytes);
        }
    }

    /// One panel and its panels per lane as the panels were opened, so a control changed before the reopen moves no row.
    struct Placed {
        uint16_t rows = 0;
        uint16_t cols = 0;
        uint8_t  panelsPerLane = 0;
    };

    const char* label_;   ///< the seam's name in the status line
    bool     open_ = false;
    bool     closeStuck_ = false;   ///< the seam's last close did not finish, and message_ says why
    bool     failing_ = false;   ///< whether submits failed in the last second, and the warning says so
    bool     stalled_ = false;   ///< whether a submit is stalled, and the warning says so
    uint8_t  derivedParallel_ = 0;   ///< the outputs the layout's panels divide into, set by open()
    char     message_[kGeometryMessageMax] = "";   ///< the way out of a geometry that does not fit, or the running panels' warnings, whole
    size_t   baseLen_ = 0;   ///< the length of the warning's fixed parts, which the failing-submit part follows
    uint16_t canvasW_ = 0;
    uint16_t canvasH_ = 0;
    uint16_t lastShown_ = 0xFFFF;   // 0xFFFF, so the first real 0 still publishes
    uint32_t lastErrors_ = 0;   ///< the failed submits counted at the last tick
    uint32_t shownErrors_ = 0;   ///< the count the warning shows
    char     status_[32] = {};   ///< the canvas and the depth, at most "65535x65535, 8-bit, GPIO"
    Placed   placed_;
    ScratchBuffer<uint8_t> canvas_;   ///< the placed frame of lanes of several panels, empty for one panel per lane, whose lights are already a raster
};

} // namespace mm
