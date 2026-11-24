/// Where a light sits on a HUB75 lane canvas, the ways to cable a set of panels with the refresh each predicts, and the plain-rows walk that tells whether a layout's lights run panel by panel in plain rows.
///
/// @technical
///
/// ## Predicted refresh
///
/// The defaults are the page's worked example, 130 ns for the LSB, 60.4 ns a pixel and 0.5 us to latch; a driver with its own pixel clock names its shift and its latch instead, so its arrangements quote its own rate. The arithmetic runs in tenths of a nanosecond, so the page's 60.4 ns a pixel stays an integer.
///
/// Where the frame is the in-tree encoder's (`encoderHold`), a narrow row leads with hold words (Hub75Geometry::holdWords), which the shift counts: the pass, not the row, is what the wire clocks.
///
/// The hold is 0 where the frame is not the encoder's or the row needs no hold. A PanelSpec row is one encoder group, so the hold adds whole, and `pairs` counts the row's shift words (laneWidth * pairs) for the hold's trigger.

#pragma once

#include "platform/Hub75Bus.h"   // hub75HoldWords: the hold words a narrow encoder row leads with
#include "platform/Hub75Pio.h"   // kHub75PanelsPerLaneMax: the engine's limit, which the card takes

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mm {

/// A cell of a HUB75 lane canvas, the raster an encoder reads.
struct CanvasPos {
    uint16_t x = 0;   ///< the column
    uint16_t y = 0;   ///< the row
};

/// Where light `i` of a layout running panel by panel, plain rows inside each, lands: panel `k` is place `k % panelsPerLane` of lane `k / panelsPerLane`, the lanes stacked.
inline CanvasPos laneCanvasPos(uint32_t i, uint16_t rows, uint16_t cols, uint16_t panelsPerLane) {
    const uint32_t perPanel = static_cast<uint32_t>(rows) * cols;
    if (perPanel == 0 || panelsPerLane == 0) return {};
    const uint32_t panel = i / perPanel;
    const uint32_t inPanel = i % perPanel;
    CanvasPos p;
    p.x = static_cast<uint16_t>((panel % panelsPerLane) * cols + inPanel % cols);
    p.y = static_cast<uint16_t>((panel / panelsPerLane) * rows + inPanel / cols);
    return p;
}

/// One panel and how it is clocked: what a refresh rate and an arrangement depend on.
struct PanelSpec {
    uint16_t rows = 0;       ///< the panel's rows
    uint16_t cols = 0;       ///< the panel's columns
    uint16_t scanRows = 0;   ///< row addresses, 32 for a 64-row panel at 1/32 scan
    uint8_t  bits = 8;       ///< bit planes
    uint16_t lsbNs = 130;    ///< the shortest plane's lit time: the page's 130 ns, or 0 where no lit time outlasts a row's shift
    uint16_t shiftTenthsPerPx = 604;   ///< the shift per pixel in tenths of a nanosecond: the page's 60.4 ns, or a pixel clock's own (1000 at 10 MHz)
    uint16_t latchTenthsNs = 5000;     ///< the latch per row in tenths of a nanosecond: the page's 0.5 us, or 0 where the latch rides the last pixel clock
    uint8_t  pairs = 1;                ///< color passes per address step the encoder shifts together (rowsPerScan / 2): 1 where a row is one shift; the hold's trigger counts laneWidth * pairs
    bool     encoderHold = false;      ///< the frame is the in-tree encoder's, whose hold words stretch a narrow row, and this output clocks them as shift: the rate counts them
    uint16_t rateCols = 0;             ///< the columns the rate counts per panel, 0 meaning cols: Stripe counts the driven double width, while the canvas stays the logical one
};

/// A `laneWidth` lane clocked as `panel` says: its predicted refresh in Hz by docs/reference/hardware/hub75-refresh.md, per plane the longer of one row's shift and its lit time, times the scan rows (@xref{predicted-refresh|predicted refresh}).
inline uint16_t hub75PredictedHz(uint32_t laneWidth, const PanelSpec& panel) {
    if (laneWidth == 0 || panel.scanRows == 0 || panel.bits == 0 || panel.bits > 16) return 0;
    // The hold words per row group in tenths of a nanosecond, added whole to the shift (@xref{predicted-refresh|predicted refresh}).
    const uint64_t holdTenths = panel.encoderHold
        ? static_cast<uint64_t>(hub75HoldWords(static_cast<size_t>(laneWidth) * (panel.pairs ? panel.pairs : 1), panel.bits)) * panel.shiftTenthsPerPx
        : 0;
    const uint64_t shift = static_cast<uint64_t>(laneWidth) * panel.shiftTenthsPerPx + panel.latchTenthsNs + holdTenths;
    uint64_t frame = 0;
    for (uint8_t i = 0; i < panel.bits; i++) {
        const uint64_t on = (static_cast<uint64_t>(panel.lsbNs) * 10) << i;   // each plane is lit twice as long as the one before
        frame += panel.scanRows * (on > shift ? on : shift);
    }
    frame = std::max<uint64_t>(frame, 1);   // a spec with no shift, latch or lit time has a frame of no length, which saturates the rate
    const uint64_t hz = (20000000000ull + frame) / (2 * frame);   // 1e10 / frame, rounded to the nearest
    return hz >= 65535 ? 65535 : static_cast<uint16_t>(hz);
}

/// The most panels in series on one lane a card takes, so no arrangement offers more.
constexpr uint16_t kPanelsPerLaneMax = platform::kHub75PanelsPerLaneMax;

/// One way to cable a set of panels: `panelsPerLane` panels in series on each of `parallel` lanes.
struct Arrangement {
    uint16_t panelsPerLane = 0;      ///< panels in series on each lane
    uint16_t parallel = 0;   ///< lanes, one strip each
    uint32_t width = 0;      ///< the canvas's width
    uint32_t height = 0;     ///< the canvas's height
    uint16_t hz = 0;         ///< the predicted refresh
};

/// The arrangement of `panelsPerLane` panels on each of `parallel` lanes, with its canvas and rate. The canvas is the logical one; the rate counts the driven width (`rateCols` where set), since a Stripe panel shifts twice the columns a row.
inline Arrangement makeArrangement(uint16_t panelsPerLane, uint16_t parallel, const PanelSpec& panel) {
    Arrangement a;
    a.panelsPerLane = panelsPerLane;
    a.parallel = parallel;
    a.width = static_cast<uint32_t>(panel.cols) * panelsPerLane;
    a.height = static_cast<uint32_t>(panel.rows) * parallel;
    const uint32_t rateWidth = static_cast<uint32_t>(panel.rateCols ? panel.rateCols : panel.cols) * panelsPerLane;
    a.hz = hub75PredictedHz(rateWidth, panel);
    return a;
}

/// Every arrangement using all `panels` on at most `maxLanes` lanes and kPanelsPerLaneMax panels a lane, most lanes first since parallel lanes shift together; the count.
inline size_t panelArrangements(uint32_t panels, const PanelSpec& panel, uint8_t maxLanes, Arrangement* out, size_t cap) {
    if (!out || panels == 0) return 0;
    size_t n = 0;
    for (uint32_t parallel = maxLanes; parallel >= 1 && n < cap; parallel--) {
        if (panels % parallel != 0 || panels / parallel > kPanelsPerLaneMax) continue;
        out[n++] = makeArrangement(static_cast<uint16_t>(panels / parallel), static_cast<uint16_t>(parallel), panel);
    }
    return n;
}

/// What keeps the lanes already built: fewer panels with the rest spare, or more panels up to the next full lane. A zero `panelsPerLane` means none fits.
struct KeepLanes {
    Arrangement use;       ///< whole lanes of the panels there are, at most the board's lanes
    uint32_t spare = 0;    ///< the panels `use` leaves over
    Arrangement add;       ///< the next full set of lanes
    uint32_t added = 0;    ///< the panels `add` needs beyond those there are
};

/// The two ways to keep lanes of `panelsPerLane` panels on at most `maxLanes` lanes: fewer panels, or more.
inline KeepLanes keepLanes(uint32_t panels, const PanelSpec& panel, uint16_t panelsPerLane, uint8_t maxLanes) {
    KeepLanes k;
    if (panelsPerLane == 0) return k;
    // Whole lanes only, and no more of them than the board has lanes for: the rest of the panels stay spare.
    const uint32_t wholeLanes = panels / panelsPerLane;
    const uint32_t fewer = (wholeLanes < maxLanes ? wholeLanes : maxLanes) * panelsPerLane;
    if (fewer > 0) {   // a whole number of lanes, capped at the board's lanes above
        k.use = makeArrangement(panelsPerLane, static_cast<uint16_t>(fewer / panelsPerLane), panel);
        k.spare = panels - fewer;
    }
    const uint32_t more = ((panels + panelsPerLane - 1) / panelsPerLane) * panelsPerLane;
    if (more != panels && more / panelsPerLane <= maxLanes) {
        k.add = makeArrangement(panelsPerLane, static_cast<uint16_t>(more / panelsPerLane), panel);
        k.added = more - panels;
    }
    return k;
}

/// Whether a layout's lights, cut in index order into runs of one panel, walk each panel in plain rows, in any of its orientations.
///
/// Every run lies on a grid of unit steps, along a row then one step to the next row; a raster across several panels, or a snaked panel, breaks it.
///
/// Fed one light at a time in index order, as a layout emits them, so it holds no table.
class PanelWalk {
public:
    /// The answer while no panel breaks the rule.
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    /// A walk for panels of `rows` by `cols` lights.
    PanelWalk(uint16_t rows, uint16_t cols) : cols_(cols), perPanel_(static_cast<uint32_t>(rows) * cols) {}

    /// The light at index `i` sits at (x, y): a panel's first light is its origin, its second and the first of its second row its steps, and every other must sit where they put it.
    void light(uint32_t i, int32_t x, int32_t y) {
        if (cols_ < 2 || perPanel_ == 0 || offPanel_ != kNone) return;
        const uint32_t panel = i / perPanel_, j = i % perPanel_;
        if (j == 0) return start(panel, x, y);
        if (panel != panel_) return;   // a panel whose first light is missing has no origin to measure from
        if (!fits(j, x - ox_, y - oy_)) offPanel_ = panel;
    }

    /// The first panel, counted from 0, whose lights leave plain rows, or kNone.
    uint32_t firstOffPanel() const { return offPanel_; }

private:
    /// A panel's first light: its origin, and both steps still to learn.
    void start(uint32_t panel, int32_t x, int32_t y) {
        panel_ = panel;
        ox_ = x;
        oy_ = y;
        along_ = down_ = false;
    }

    /// Whether the held panel's light `j`, at (dx, dy) from its origin, keeps plain rows; light 1 sets the step along, light `cols_` the step down.
    bool fits(uint32_t j, int32_t dx, int32_t dy) {
        if (j == 1) {
            ux_ = dx;
            uy_ = dy;
            return along_ = unitStep(dx, dy);
        }
        if (j == cols_) {
            vx_ = dx;
            vy_ = dy;
            return down_ = unitStep(dx, dy) && dx * ux_ + dy * uy_ == 0;
        }
        const int32_t col = static_cast<int32_t>(j % cols_), row = static_cast<int32_t>(j / cols_);
        if (!along_ || (row > 0 && !down_)) return true;
        return dx == col * ux_ + row * vx_ && dy == col * uy_ + row * vy_;
    }

    /// One step along an axis, either way.
    static bool unitStep(int32_t dx, int32_t dy) { return (dx == 0) != (dy == 0) && dx * dx + dy * dy == 1; }

    uint16_t cols_;
    uint32_t perPanel_;
    uint32_t offPanel_ = kNone;
    uint32_t panel_ = kNone;   ///< the panel whose origin is held
    int32_t  ox_ = 0, oy_ = 0;   ///< that panel's first light
    int32_t  ux_ = 0, uy_ = 0;   ///< the step along its rows
    int32_t  vx_ = 0, vy_ = 0;   ///< the step down to its next row
    bool     along_ = false, down_ = false;
};

} // namespace mm
