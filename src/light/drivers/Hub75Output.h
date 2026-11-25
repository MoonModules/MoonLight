#pragma once

/// The HUB75 driver's two seams: the settings an output reads from its driver, and the output that takes the corrected frame to the panel.

#include <cstddef>
#include <cstdint>
#include "core/module/Control.h"          // ControlList: an output appends its own controls into the driver's list
#include "core/module/MoonModule.h"       // MoonModule::FixedPin: the lines an output reports to the pin map
#include "light/drivers/LedPeripheral.h"  // LedHwBlock: the sibling claim guard's vocabulary
#include "platform/nonblocking.h"        // MM_NONBLOCKING
#include "platform/Hub75Pio.h"           // Hub75PioOptions: the defaults of the Pi output's settings
#include "platform/platform_leds.h"   // Hub75Backend

namespace mm {

class Layer;   // the layout's lights, which the Pi output cuts into panels

/// The color depths an output drives, in bit planes.
struct DepthRange {
    uint8_t min;   ///< the fewest planes
    uint8_t max;   ///< the most planes
};

/// The settings an output reads from its driver, the backend the `peripheral` select names, and the layer the driver draws from.
///
/// The settings live on the driver; the output binds them into the driver's card. The Pi output's panel and clock settings live here too, so a saved card keeps them through a boot where the PIO is not offered.
class Hub75Settings {
public:
    /// The board's wiring: picking one fills the fourteen pins, Custom keeps yours.
    /// @guide A published map hides the pin rows, which stay bound. A pin on flash or PSRAM is refused. Boards and the R1/G1/B1 naming trap: [HUB75 panels](https://moonmodules.org/MoonLight/reference/hardware/hub75-panels.html#which-board).
    uint8_t boardSel = 0;

    int8_t r1 = -1;   ///< Red, upper half-panel.
    int8_t g1 = -1;   ///< Green, upper half-panel.
    int8_t b1 = -1;   ///< Blue, upper half-panel.
    int8_t r2 = -1;   ///< Red, lower half-panel.
    int8_t g2 = -1;   ///< Green, lower half-panel.
    int8_t b2 = -1;   ///< Blue, lower half-panel.
    int8_t addrA = -1;   ///< Row address bit 0.
    int8_t addrB = -1;   ///< Row address bit 1.
    int8_t addrC = -1;   ///< Row address bit 2.
    int8_t addrD = -1;   ///< Row address bit 3.
    int8_t addrE = -1;   ///< Row address bit 4, on 1/32 panels.
    int8_t clk = -1;   ///< The shift clock.
    int8_t lat = -1;   ///< Latch: moves the shift register to the output drivers.
    int8_t oe = -1;    ///< Output enable, active LOW.

    /// The clock edges `clockEdgeSel` picks from, in kEdgeOptions order.
    enum : uint8_t { kEdgeRising, kEdgeFalling, kEdgeCount };
    /// The labels `clockEdgeSel` indexes.
    static constexpr const char* kEdgeOptions[kEdgeCount] = {"rising", "falling"};
    /// The edge the chips sample on; every pixel one column over means the other.
    uint8_t clockEdgeSel = kEdgeRising;
    /// The scan rates `scanSel` picks from, in kScanOptions order.
    static constexpr uint8_t kScanCount = 3;
    /// The labels `scanSel` indexes.
    static constexpr const char* kScanOptions[kScanCount] = {"1/8", "1/16", "1/32"};
    /// The scan rate each label means: 8, 16 or 32.
    static constexpr uint8_t kScanRates[kScanCount] = {8, 16, 32};
    /// The panel's scan rate: 1/8, 1/16 or 1/32, read off the panel.
    /// @guide Two panels of identical dimensions can scan differently, so the size does not give the rate. A 1/32-scan panel needs `e`; on 1/16 panels that pin is free.
    uint8_t scanSel = 1;

    /// Bit planes per frame: color levels against refresh and memory.
    /// @guide Depth costs refresh and memory per plane, more on a layout 16 columns or less at 6-bit. Full white lights about a third of the time at 6-bit.
    uint8_t bitDepth = 6;

    /// The seam's own defaults, the one home every Pi setting below starts from.
    static constexpr platform::Hub75PioOptions kPiDefaults{};
    /// The rows of one panel on a Raspberry Pi's header.
    uint8_t panelRows = kPiDefaults.rows;
    /// The columns of one panel on a Raspberry Pi's header.
    uint16_t panelCols = kPiDefaults.cols;
    /// Panels in series on one lane of a Pi's header; hzeller's `--led-chain` is the same.
    uint8_t panelsPerLane = kPiDefaults.panelsPerLane;
    /// The Pi's pixel clock in kHz; 10,000 is 10 MHz.
    uint16_t pixelClockKhz = static_cast<uint16_t>(kPiDefaults.pixelClockHz / 1000);
    /// Blank words the Pi's latch holds, 0 for none.
    uint8_t latchBlankWords = kPiDefaults.latchBlankWords;

    /// The backend the `peripheral` select names. Virtual because Hub75BusOutput reads it through this base, in open() and hwBlock() off the frame path, and the driver alone holds the select; drivenLayer() keeps the vtable anyway.
    virtual platform::Hub75Backend selectedBackend() const = 0;
    /// The layer the driver draws from, null while it has none.
    virtual Layer* drivenLayer() const = 0;

protected:
    ~Hub75Settings() = default;   // protected: a driver is deleted through MoonModule's virtual destructor, so this base's stays trivial
};

/// The last step of a HUB75 frame, behind a runtime strategy interface: the driver holds one output.
///
/// The driver owns the lifecycle and the correction into one RGB frame in light order. The output owns what happens to that frame: its controls, geometry, hardware claim, and the call that puts it on the panel.
///
/// The driver reads lights() once, in prepare(), and show() runs once a frame, so the interface costs one dispatch a frame; the output's own calls into its seam come on top.
class Hub75Output {
public:
    /// Virtual, so a concrete output passes `-Wnon-virtual-dtor`, as LedPeripheral's does.
    virtual ~Hub75Output() = default;

    /// Bind the output to its driver, which it reads its settings through.
    void attach(Hub75Settings* owner) { owner_ = owner; }

    /// Append the controls shown above the driver's `peripheral` select.
    virtual void addWiringControls(ControlList& controls) = 0;
    /// Append the controls this output shows below the `peripheral` select.
    virtual void addControls(ControlList& controls) = 0;
    /// Apply a change to control `name`, answering whether the card must be rebuilt.
    virtual bool onControlChanged(const char* name) = 0;
    /// Whether a change to control `name` needs the output reopened.
    virtual bool affectsPrepare(const char* name) const = 0;
    /// The planes the driver's one `bitDepth` takes on this output; the driver clamps the depth into it when the output changes.
    virtual DepthRange depthRange() const = 0;

    /// The bytes a frame for a layout of `width` by `height` takes in the platform's buffer, which decides the backends offered; 0 for an unusable layout.
    virtual size_t frameBytes(uint16_t width, uint16_t height) const = 0;
    /// Bring the panel up for a layout of `width` by `height`; null when it runs, else the reason it refused, which the driver shows as an error.
    virtual const char* open(uint16_t width, uint16_t height) = 0;
    /// The status line while the panel runs.
    virtual const char* describe() const MM_NONBLOCKING = 0;
    /// What degrades the running panel or the layout's fit to it, in full, shown as a warning in place of describe(); null while the output is healthy.
    virtual const char* warning() const MM_NONBLOCKING = 0;
    /// The lights in the layout's box while it runs: the length of the RGB frame show() takes, a size_t since a light type of 16 bits wraps at 65,536. It stays fixed from open() to close(), which the driver relies on to read it once.
    virtual size_t lights() const MM_NONBLOCKING = 0;
    /// Put one corrected RGB frame of lights() lights on the panel, lit at `brightness`.
    virtual void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING = 0;
    /// Refresh the output's measured readouts, once a second while it runs.
    virtual void tick1s() MM_NONBLOCKING = 0;
    /// Stop the panel and release its hardware; safe to call on a stopped panel.
    virtual void close() = 0;

    /// The hardware block this output drives, which the sibling claim guard refuses to double up.
    virtual LedHwBlock hwBlock() const = 0;
    /// Lines the output holds whose pin controls are hidden, for the pin map.
    virtual uint8_t fixedPins(MoonModule::FixedPin* out, uint8_t max) const = 0;

protected:
    Hub75Settings* owner_ = nullptr;   ///< the driver, bound by its constructor
};

} // namespace mm
