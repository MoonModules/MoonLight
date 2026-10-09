#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/drivers/DriverBase.h"
#include "light/drivers/Hub75Slots.h"
#include "light/layers/Layer.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Output driver: HUB75 panels driven directly from the board's own pins, with no receiving card.
///
/// The sibling of `PanelCardDriver`, which reaches the same panels over raw Ethernet to a 5A-75B/E card and wins above roughly 16,384 pixels. The bit-plane wire format is Hub75Slots.h, pure data and host-tested.
///
/// Prior art: the HUB75 lineage generally (mrcodetastic/ESP32-HUB75-MatrixPanel-DMA, hzeller/rpi-rgb-led-matrix, ESPHome's hub75 component). The scan and bit-plane structure belongs to the panel rather than to any library; studied, not copied.
///
/// @moreinfo
///
/// **Output is continuous.** A WS2812 strand latches a frame and holds it; a HUB75 panel holds
/// nothing and is lit only while being clocked, so the platform arms one scan and re-sends the same buffer forever. This driver writes the next frame into that buffer between scans, which is why tick() transmits nothing and waits for nothing. `Hub75Slots.h` covers scanning and why brightness is time.
///
/// **The `board` select supplies the pins**, defaulting to MoonHub75 and prefilling all fourteen on
/// first definition, because a soldered line must never be guessed from nothing. Each map is taken from that board's own published source. The maps themselves are on the driver's page. A user wiring a panel reads them beside the rest of the card.
///
/// **Two naming traps, both worth knowing before wiring.** A panel's ribbon numbers its color lines
/// R1/G1/B1 (upper half) and R2/G2/B2 (lower half); some board docs call the same pairs R0/G0/B0 and R1/G1/B1. WLED's pin array ends `...LAT,OE,CLK`, so a map transcribed as `...CLK,LAT,OE` swaps three lines silently. Every map here is already in this driver's control order (clk, lat, oe).
///
class Hub75Driver : public DriverBase {
public:
    // Custom leaves whatever is there, so a hand-wired tweak survives the switch.
    /// Which board's wiring to use; index into boardOptions_, 0 being MoonHub75.
    uint8_t boardSel = 0;

    // Unset by default: a blind guess lands on octal-PSRAM, USB, UART0 or a strapping pin.
    /// Red, green and blue for the upper half-panel.
    int8_t r1 = -1;
    int8_t g1 = -1;   ///< Green, upper half-panel.
    int8_t b1 = -1;   ///< Blue, upper half-panel.
    /// Red, green and blue for the lower half-panel.
    int8_t r2 = -1;
    int8_t g2 = -1;   ///< Green, lower half-panel.
    int8_t b2 = -1;   ///< Blue, lower half-panel.
    /// Row address lines; a 1/16 panel leaves `addrE` unset.
    int8_t addrA = -1;
    int8_t addrB = -1;   ///< Row address bit 1.
    int8_t addrC = -1;   ///< Row address bit 2.
    int8_t addrD = -1;   ///< Row address bit 3.
    int8_t addrE = -1;   ///< Row address bit 4, on 1/32 panels.
    /// The pixel clock.
    int8_t clk = -1;
    int8_t lat = -1;   ///< Latch: moves the shift register to the output drivers.
    int8_t oe = -1;    ///< Output enable, active LOW.

    // NOT derivable from the panel's size: two of one size can scan differently.
    /// The clock edges `clockEdgeSel` picks from, in kEdgeOptions order.
    enum ClockEdge : uint8_t { kEdgeRising, kEdgeFalling, kEdgeCount };
    /// Which clock edge the panel's chips sample on.
    uint8_t clockEdgeSel = kEdgeRising;
    /// The panel's own scan rate; index into kScanOptions.
    uint8_t scanSel = 1;

    // Every plane costs a full scan, so this trades color precision against refresh and memory.
    /// Bit planes per frame, and therefore the refresh tradeoff.
    uint8_t bitDepth = 6;

    // A P4 has one of each, so a board driving strips from one needs the panel on the other.
    /// Which silicon block drives the panel, where the chip offers a choice.
    uint8_t peripheralSel_ = 0;

    /// The card's tag: this driver is two-dimensional.
    const char* tags() const override { return "🟦"; }

    /// A panel shows a level only as lit time, so brightness shortens the planes' windows rather than scaling values.
    bool dimsByTime() const MM_NONBLOCKING override { return true; }

    // Reported only while the bus is up, so a failed init cannot phantom-claim the block.
    /// The peripheral block this driver holds, for the sibling claim guard.
    LedHwBlock hwBlock() const override {
        if (!running_) return LedHwBlock::None;
        // WHICH block, not a guess: claiming the wrong one frees a live block and holds a free one.
        return backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0] ==
                       platform::Hub75Backend::Parlio
                   ? LedHwBlock::Parlio
                   : LedHwBlock::LcdCam;
    }

    /// Bind the rendered frame this driver encodes from.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Publish the board select, the fourteen pin controls, the geometry and the measured refresh.
    void defineDriverControls() override {
        buildBoardOptions();
        // Once only, and not in the constructor: declaration order would overwrite it.
        if (!prefilled_) { prefilled_ = true; applyBoard(); }
        controls_.addSelect("board", boardSel, boardOptions_, boardOptionCount_);

        // Hidden on a published map, and hidden stays BOUND: the values still drive the panel.
        const bool editable = pinsEditable();
        const char* const kPinNames[] = {"r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c",
                                         "d", "e", "clk", "lat", "oe"};
        int8_t* const kPinVars[] = {&r1, &g1, &b1, &r2, &g2, &b2, &addrA, &addrB, &addrC,
                                    &addrD, &addrE, &clk, &lat, &oe};
        for (size_t i = 0; i < sizeof(kPinNames) / sizeof(kPinNames[0]); i++) {
            controls_.addPin(kPinNames[i], *kPinVars[i]);
            controls_.setHidden(controls_.count() - 1, !editable);
        }

        buildBackendOptions();
        controls_.addSelect("peripheral", peripheralSel_, backendOptions_, backendOptionCount_);
        controls_.addSelect("scanRate", scanSel, kScanOptions, kScanCount);
        controls_.addControl("bitDepth", bitDepth, 2, 6);
        // The one knob a chip family changes. WLED exposes the same flag as its bus "reversed" box.
        controls_.addSelect("clockEdge", clockEdgeSel, kEdgeOptions, kEdgeCount);

        // The MEASURED refresh: what turns "it flickers" into a number someone can act on.
        controls_.addReadOnly("refresh", refreshStr_, sizeof(refreshStr_));
    }

    // Written here rather than at the next rebuild, so the card's fields update at once.
    /// Apply a control change; picking a board writes its map into the pin controls.
    void onControlChanged(const char* name) override {
        if (name && std::strcmp(name, "board") == 0) {
            applyBoard();
            // rebuildControls CLEARS first; a bare defineControls appended fourteen more rows.
            rebuildControls();
        }
        DriverBase::onControlChanged(name);
    }

    // The rest, such as a pin typo being corrected, are handled by prepare() re-initializing.
    /// Whether changing `name` forces a rebuild: the wire format and the geometry do.
    bool affectsPrepare(const char* name) const override {
        static const char* const kRebuild[] = {
            "r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c", "d", "e",
            "clk", "lat", "oe", "scanRate", "bitDepth", "peripheral", "board", "clockEdge",
        };
        for (const char* n : kRebuild) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    /// Stop the scan and release the panel's peripheral.
    void release() override {
        platform::hub75Deinit(hub_);
        running_ = false;
    }

    /// Build the geometry, claim the peripheral, and arm the continuous scan.
    void prepare() override {
        release();
        if (!layer_) {
            setStatus("no layer", Severity::Warning);
            return;
        }
        geo_.width = static_cast<uint16_t>(layer_->physicalWidth());
        geo_.height = static_cast<uint16_t>(layer_->physicalHeight());
        geo_.scanRate = kScanRates[scanSel < kScanCount ? scanSel : 0];
        geo_.bitDepth = bitDepth;

        if (!geo_.valid()) {
            // Name WHICH constraint failed: the scan rate is nearly always the wrong one.
            std::snprintf(statusBuf_, sizeof(statusBuf_),
                          "%ux%u does not divide into 1/%u scan row pairs",
                          geo_.width, geo_.height, geo_.scanRate);
            setStatus(statusBuf_, Severity::Error);
            return;
        }

        // Routing I/O onto a flash or PSRAM pin resets the chip with no panic naming anything.
        {
            const char* role = nullptr;
            const char* why = nullptr;
            const int8_t bad = unusableLine(role, why);
            if (bad >= 0) {
                std::snprintf(statusBuf_, sizeof(statusBuf_), "%s on GPIO %d %s",
                              role, static_cast<int>(bad), why);
                setStatus(statusBuf_, Severity::Error);
                return;
            }
        }

        platform::Hub75Pins pins;
        pins.r1 = toPin(r1); pins.g1 = toPin(g1); pins.b1 = toPin(b1);
        pins.r2 = toPin(r2); pins.g2 = toPin(g2); pins.b2 = toPin(b2);
        pins.a = toPin(addrA); pins.b = toPin(addrB); pins.c = toPin(addrC);
        pins.d = toPin(addrD); pins.e = toPin(addrE);
        pins.clk = toPin(clk); pins.lat = toPin(lat); pins.oe = toPin(oe);
        pins.clkFalling = clockEdgeSel == kEdgeFalling;

        const platform::Hub75Backend backend =
            backendIndex_[peripheralSel_ < backendOptionCount_ ? peripheralSel_ : 0];
        if (!platform::hub75Init(hub_, backend, pins, geo_.width, geo_.height,
                                 geo_.scanRate, geo_.bitDepth)) {
            const char* why = platform::hub75LastError();
            setStatus(why ? why : "HUB75 init failed", Severity::Error);
            return;
        }
        if (!platform::hub75Start(hub_)) {
            const char* why = platform::hub75LastError();
            setStatus(why ? why : "the panel scan would not start", Severity::Error);
            platform::hub75Deinit(hub_);
            return;
        }
        // One whole frame of packed RGB: the encoder needs the frame, not one light at a time.
        ensureWire(static_cast<size_t>(geo_.width) * geo_.height * 3);
        if (!wire_) {
            setStatus("not enough memory for the correction buffer", Severity::Error);
            platform::hub75Deinit(hub_);
            return;
        }

        running_ = true;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%ux%u, 1/%u scan, %u-bit (%s)",
                      geo_.width, geo_.height, geo_.scanRate, geo_.bitDepth,
                      platform::hub75Backend(hub_));
        setStatus(statusBuf_, Severity::Status);
    }

    void tick() MM_NONBLOCKING override {
        if (!running_ || !sourceBuffer_ || !sourceBuffer_->data()) return;
        uint8_t* out = platform::hub75Buffer(hub_);
        if (!out) return;

        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t outCh = correction_.outChannels;
        const nrOfLightsType lights =
            static_cast<nrOfLightsType>(geo_.width) * geo_.height;
        if (srcCh == 0 || outCh < 3 || lights == 0) return;
        if (sourceBuffer_->count() < lights) return;     // frame smaller than the panel
        if (!wire_ || wireCap_ < static_cast<size_t>(lights) * 3) return;   // not ready

        // A plane reads (x, r) and the pixel half a panel below in one pass, so correct first.
        const uint8_t* src = sourceBuffer_->data();
        if (outCh == 3) {
            for (nrOfLightsType i = 0; i < lights; i++) {
                correction_.apply(src + static_cast<size_t>(i) * srcCh,
                                  wire_ + static_cast<size_t>(i) * 3, srcCh);
            }
        } else {
            uint8_t scratch[8];   // outChannels is bounded by the wiring; 8 covers RGBWW and up
            const uint8_t n = outCh <= sizeof(scratch) ? outCh : static_cast<uint8_t>(sizeof(scratch));
            for (nrOfLightsType i = 0; i < lights; i++) {
                correction_.apply(src + static_cast<size_t>(i) * srcCh, scratch, srcCh);
                uint8_t* d = wire_ + static_cast<size_t>(i) * 3;
                d[0] = scratch[0]; d[1] = scratch[1]; d[2] = n > 2 ? scratch[2] : 0;
            }
        }

        // Brightness is the planes' lit time, read each frame so the slider applies on the next one.
        geo_.brightness = effectiveBrightness();
        // No double buffer: a one-pass tear beats a second 256 KB frame, invisible at 300+ Hz.
        hub75Encode(wire_, out, geo_);
    }

    // An override because the geometry can change under it: resize before the next tick reads.
    /// Resize the correction scratch when the wiring changes.
    void onCorrectionChanged() override {
        if (!running_) return;
        ensureWire(static_cast<size_t>(geo_.width) * geo_.height * 3);
    }

    // MM_NONBLOCKING STAYS: dropping it widens the base's contract and the compiler refuses.
    /// Publish the measured refresh rate.
    void tick1s() MM_NONBLOCKING override {
        if (!running_) return;
        const uint16_t hz = platform::hub75RefreshHz(hub_);
        if (hz == lastRefresh_) return;      // never re-serialize an unchanged value on the 1 Hz tick
        lastRefresh_ = hz;
        mm::formatTo(refreshStr_, sizeof(refreshStr_), "%u Hz", hz);
    }

private:
    // Re-points by LABEL, so a geometry change that drops a backend cannot select another.
    /// Offer the backends this silicon has and that can carry this geometry.
    void buildBackendOptions() {
        const char* current = (peripheralSel_ < backendOptionCount_)
                                  ? backendOptions_[peripheralSel_] : nullptr;
        // Zero while the layer is unknown, which asks only whether the silicon exists.
        size_t bytes = 0;
        if (layer_) {
            Hub75Geometry probe;
            probe.width = static_cast<uint16_t>(layer_->physicalWidth());
            probe.height = static_cast<uint16_t>(layer_->physicalHeight());
            probe.scanRate = kScanRates[scanSel < kScanCount ? scanSel : 0];
            probe.bitDepth = bitDepth;
            if (probe.valid()) bytes = probe.frameBytes();
        }

        backendOptionCount_ = 0;
        for (uint8_t i = 0; i < kBackendCount; i++) {
            const auto b = static_cast<platform::Hub75Backend>(i);
            if (!platform::hub75BackendAvailable(b, bytes)) continue;
            backendOptions_[backendOptionCount_] = platform::hub75BackendLabel(b);
            backendIndex_[backendOptionCount_] = b;
            backendOptionCount_++;
        }
        if (backendOptionCount_ == 0) {      // no usable backend (desktop, classic ESP32)
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

    // One board's HUB75 wiring in ribbon order; -1 leaves that line to the user.
    struct BoardPins {
        const char* label;
        int8_t r1, g1, b1, r2, g2, b2;
        int8_t a, b, c, d, e;
        int8_t clk, lat, oe;
        bool (*onThisChip)();   // null = every chip
        bool editable;   // false on a published map: those lines are soldered
    };

    // The platform names the chip; inferring it from GPIO counts was a guess dressed as a test.
    static bool chipIsS3()  { return platform::isEsp32S3; }
    static bool chipIsP4()  { return platform::isEsp32P4; }
    static bool chipIsS31() { return platform::isEsp32S31; }

    /// The boards this driver knows, and the per-chip generic sets.
    // The `-generic` rows are NOT boards: free pins from gpio-usage.md, and editable.
    static constexpr BoardPins kBoards[] = {
        // MoonHub75 PCB (Lilygo T7-S3). MOONHUB75/README.md names the upper half R0/G0/B0.
        {"MoonHub75",   1,  5,  6,   7, 13,  9,  16, 48, 47, 21, 38,  18,  8,  4, chipIsS3, false},
        // Adafruit MatrixPortal S3, from the board's own CircuitPython pins.c.
        {"MatrixPortal S3", 42, 41, 40,  38, 39, 37,  45, 36, 48, 35, 21,   2, 47, 14, chipIsS3, false},
        // Waveshare SKU 34422: WLED's array ends LAT,OE,CLK, so reading ...CLK,LAT,OE swaps three.
        {"Waveshare RGB Matrix", 4,  5,  6,   7, 15, 16,  18,  8,  3, 42,  9,  41, 40,  2, chipIsS3, false},
        // Generic sets: the first fourteen of each chip's free GPIOs, in ribbon order.
        {"S3 generic",  4,  5,  6,   7,  8,  9,  10, 11, 12, 13, 14,  15, 16, 17, chipIsS3, true},
        {"P4 generic", 20, 21, 22,  23, 24, 25,  26, 27, 32, 33, 39,  40, 41, 42, chipIsP4, true},
        // The S31's free set is board-specific, so it ships unset rather than invented.
        {"S31 generic", -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, chipIsS31, true},
        // Custom keeps whatever is in the fields: the escape hatch for a hand-wired panel.
        {"Custom",     -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, nullptr, true},
    };
    static constexpr uint8_t kBoardCount = sizeof(kBoards) / sizeof(kBoards[0]);

    // A P4-generic row on an S3 would be fourteen pins that chip does not have.
    /// Offer the boards that make sense on this chip.
    void buildBoardOptions() {
        const char* current = (boardSel < boardOptionCount_) ? boardOptions_[boardSel] : nullptr;
        boardOptionCount_ = 0;
        for (uint8_t i = 0; i < kBoardCount; i++) {
            // A desktop is none of the three chips and offers every row; silicon sees its own.
            constexpr bool anyKnownChip =
                platform::isEsp32S3 || platform::isEsp32P4 || platform::isEsp32S31;
            if constexpr (anyKnownChip) {
                if (kBoards[i].onThisChip && !kBoards[i].onThisChip()) continue;
            }
            boardOptions_[boardOptionCount_] = kBoards[i].label;
            boardIndex_[boardOptionCount_] = i;
            boardOptionCount_++;
        }
        // Re-point by LABEL, so a filtered list cannot silently select a different board.
        uint8_t sel = 0;
        if (current) {
            for (uint8_t k = 0; k < boardOptionCount_; k++) {
                if (std::strcmp(boardOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        boardSel = sel;
    }

    // One HUB75 line as wired: the GPIO and the ribbon name it carries.
    struct Line { int8_t gpio; const char* role; };
    static constexpr uint8_t kLineCount = 14;

    /// The set lines in ribbon order: the one home for which GPIOs this driver holds.
    uint8_t lines(Line* out) const {
        const Line all[kLineCount] = {
            {r1, "r1"}, {g1, "g1"}, {b1, "b1"}, {r2, "r2"}, {g2, "g2"}, {b2, "b2"},
            {addrA, "a"}, {addrB, "b"}, {addrC, "c"}, {addrD, "d"}, {addrE, "e"},
            {clk, "clk"}, {lat, "lat"}, {oe, "oe"}};
        uint8_t n = 0;
        for (const Line& l : all) if (l.gpio >= 0) out[n++] = l;
        return n;
    }

    /// Are the pin controls the user's to edit, for the board currently selected?
    bool pinsEditable() const {
        if (boardSel >= boardOptionCount_) return true;   // no board resolved: never hide
        return kBoards[boardIndex_[boardSel]].editable;
    }

    /// A published board's lines for the pin map, since their hidden controls read as free there.
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        // Hidden means "not in use" everywhere else; here the lines are soldered and driven.
        if (!out || pinsEditable()) return 0;
        Line wired[kLineCount];
        uint8_t n = 0;
        for (uint8_t i = 0, count = lines(wired); i < count && n < max; i++) {
            out[n++] = FixedPin{static_cast<uint8_t>(wired[i].gpio), wired[i].role};
        }
        return n;
    }

    /// The first line on a pin the chip refuses, with its role and why, or -1 when all are usable.
    int8_t unusableLine(const char*& role, const char*& why) const {
        Line wired[kLineCount];
        for (uint8_t i = 0, count = lines(wired); i < count; i++) {
            const uint8_t gpio = static_cast<uint8_t>(wired[i].gpio);
            const char* refusal = platform::gpioRefusal(gpio);
            // Every line is driven, so an input-only pad is as unusable as a reserved one.
            if (!refusal && !platform::gpioCapability(gpio).outputCapable)
                refusal = "is an input-only pin on this chip";
            if (!refusal) continue;
            role = wired[i].role;
            why = refusal;
            return wired[i].gpio;
        }
        return -1;
    }

    // Custom writes nothing, so switching to it after an edit keeps the edit.
    /// Write the chosen board's map into the pin controls.
    void applyBoard() {
        if (boardSel >= boardOptionCount_) return;
        const BoardPins& b = kBoards[boardIndex_[boardSel]];
        if (std::strcmp(b.label, "Custom") == 0) return;
        r1 = b.r1; g1 = b.g1; b1 = b.b1;
        r2 = b.r2; g2 = b.g2; b2 = b.b2;
        addrA = b.a; addrB = b.b; addrC = b.c; addrD = b.d; addrE = b.e;
        clk = b.clk; lat = b.lat; oe = b.oe;
    }

    const char* boardOptions_[kBoardCount] = {};
    uint8_t boardIndex_[kBoardCount] = {};
    uint8_t boardOptionCount_ = 0;
    bool prefilled_ = false;   // the board map is written once, not on every re-render

    static constexpr uint8_t kBackendCount = 2;
    const char* backendOptions_[kBackendCount + 1] = {};
    platform::Hub75Backend backendIndex_[kBackendCount + 1] = {};
    uint8_t backendOptionCount_ = 0;

    static constexpr const char* kEdgeOptions[kEdgeCount] = {"rising", "falling"};
    static constexpr uint8_t kScanCount = 3;
    static constexpr const char* kScanOptions[kScanCount] = {"1/8", "1/16", "1/32"};
    static constexpr uint8_t kScanRates[kScanCount] = {8, 16, 32};

    // -1 for unset becomes 0xFFFF: a P4 GPIO number can exceed int8_t's range.
    /// Convert a Pin control's value to the platform seam's pin type.
    static uint16_t toPin(int8_t p) {
        return p < 0 ? 0xFFFF : static_cast<uint16_t>(p);
    }

    platform::Hub75Handle hub_{};
    Hub75Geometry geo_{};
    Buffer* sourceBuffer_ = nullptr;
    bool running_ = false;
    uint16_t lastRefresh_ = 0xFFFF;      // not 0: the first real 0 must still publish
    char statusBuf_[64] = {};
    char refreshStr_[16] = {};
};

}  // namespace mm
