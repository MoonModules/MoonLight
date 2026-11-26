#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/drivers/Hub75Output.h"
#include "platform/Hub75Bus.h"      // Hub75Geometry, kHub75PortMaxBitDepth
#include "platform/Hub75Encode.h"   // hub75Encode: the frame's wire format
#include "platform/platform_leds.h"   // Hub75Pins, hub75Init, hub75LastError
#include "platform/platform_io.h"   // gpioRefusal, gpioCapability

#include <cstdio>
#include <cstring>

namespace mm {

/// The HUB75 driver's output on the ESP32's parallel bus: it packs the corrected frame into the panel's bit planes in the platform's buffer, which LCD_CAM or Parlio clocks out.
///
/// It owns the board table and the measured refresh text; the fourteen pins, the scan rate, the bit depth and the clock edge live on the driver, and the output binds them into the driver's card.
class Hub75BusOutput final : public Hub75Output {
public:
    /// The board select and the fourteen pin controls, above the peripheral select.
    void addWiringControls(ControlList& controls) override {
        buildBoardOptions();
        // Once only, at the first card, so a re-render keeps an edited pin.
        if (!prefilled_) { prefilled_ = true; applyBoard(); }
        controls.addSelect("board", owner_->boardSel, boardOptions_, boardOptionCount_);

        // Hidden on a published map, and hidden stays BOUND: the values still drive the panel.
        const bool editable = pinsEditable();
        const char* const kPinNames[] = {"r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c",
                                         "d", "e", "clk", "lat", "oe"};
        Hub75Settings& settings = *owner_;
        int8_t* const kPinVars[] = {&settings.r1, &settings.g1, &settings.b1, &settings.r2, &settings.g2,
                                    &settings.b2, &settings.addrA, &settings.addrB, &settings.addrC,
                                    &settings.addrD, &settings.addrE, &settings.clk, &settings.lat, &settings.oe};
        for (size_t i = 0; i < sizeof(kPinNames) / sizeof(kPinNames[0]); i++) {
            controls.addPin(kPinNames[i], *kPinVars[i]);
            controls.setHidden(controls.count() - 1, !editable);
        }
    }

    /// The geometry, the clock edge and the measured refresh, below the peripheral select.
    void addControls(ControlList& controls) override {
        controls.addSelect("scanRate", owner_->scanSel, Hub75Settings::kScanOptions, Hub75Settings::kScanCount);
        controls.addControl("bitDepth", owner_->bitDepth, depthRange().min, depthRange().max);
        // The one knob a chip family changes. WLED exposes the same flag as its bus "reversed" box.
        controls.addSelect("clockEdge", owner_->clockEdgeSel, Hub75Settings::kEdgeOptions, Hub75Settings::kEdgeCount);

        /// The measured refresh rate: the number to report when a panel flickers.
        controls.addReadOnly("refresh", refreshStr_, sizeof(refreshStr_));
    }

    /// Picking a board writes its map into the pin controls at once, which the card then shows.
    bool onControlChanged(const char* name) override {
        if (std::strcmp(name, "board") != 0) return false;
        applyBoard();
        return true;
    }

    /// The wire format and the geometry need the bus reopened.
    bool affectsPrepare(const char* name) const override {
        static const char* const kReopen[] = {
            "r1", "g1", "b1", "r2", "g2", "b2", "a", "b", "c", "d", "e",
            "clk", "lat", "oe", "scanRate", "bitDepth", "board", "clockEdge",
        };
        for (const char* n : kReopen) {
            if (std::strcmp(name, n) == 0) return true;
        }
        return false;
    }

    /// The planes the ESP32 port takes, which the control bounds.
    DepthRange depthRange() const override { return DepthRange{2, kHub75PortMaxBitDepth}; }

    /// The bytes the bit planes take for this layout at the selected scan and depth; 0 for an unusable layout.
    size_t frameBytes(uint16_t width, uint16_t height) const override {
        const Hub75Geometry probe = geometry(width, height);
        return probe.valid() ? probe.frameBytes() : 0;
    }

    /// Build the geometry, refuse an unusable line, then init the bus and arm the continuous scan.
    const char* open(uint16_t width, uint16_t height) override {
        geo_ = geometry(width, height);
        if (!geo_.valid()) {
            // Name WHICH constraint failed: the scan rate is nearly always the wrong one.
            std::snprintf(status_, sizeof(status_), "%ux%u does not divide into 1/%u scan row pairs",
                          geo_.width, geo_.height, geo_.scanRate);
            return status_;
        }

        // Routing I/O onto a flash or PSRAM pin resets the chip with no panic naming anything.
        {
            const char* role = nullptr;
            const char* why = nullptr;
            const int8_t bad = unusableLine(role, why);
            if (bad >= 0) {
                std::snprintf(status_, sizeof(status_), "%s on GPIO %d %s",
                              role, static_cast<int>(bad), why);
                return status_;
            }
        }

        const Hub75Settings& settings = *owner_;
        platform::Hub75Pins pins;
        pins.r1 = toPin(settings.r1); pins.g1 = toPin(settings.g1); pins.b1 = toPin(settings.b1);
        pins.r2 = toPin(settings.r2); pins.g2 = toPin(settings.g2); pins.b2 = toPin(settings.b2);
        pins.a = toPin(settings.addrA); pins.b = toPin(settings.addrB); pins.c = toPin(settings.addrC);
        pins.d = toPin(settings.addrD); pins.e = toPin(settings.addrE);
        pins.clk = toPin(settings.clk); pins.lat = toPin(settings.lat); pins.oe = toPin(settings.oe);
        pins.clkFalling = settings.clockEdgeSel == Hub75Settings::kEdgeFalling;

        if (!platform::hub75Init(hub_, settings.selectedBackend(), pins, geo_.width, geo_.height,
                                 geo_.scanRate, geo_.bitDepth)) {
            const char* why = platform::hub75LastError();
            return why ? why : "HUB75 init failed";
        }
        if (!platform::hub75Start(hub_)) {
            const char* why = platform::hub75LastError();
            platform::hub75Deinit(hub_);
            return why ? why : "the panel scan would not start";
        }
        std::snprintf(status_, sizeof(status_), "%ux%u, 1/%u scan, %u-bit (%s)",
                      geo_.width, geo_.height, geo_.scanRate, geo_.bitDepth,
                      platform::hub75Backend(hub_));
        return nullptr;
    }

    /// The geometry and the backend, as open() formatted them.
    const char* describe() const MM_NONBLOCKING override { return status_; }

    /// The bus raises each refusal as an error at open, so a running bus has no warning to give.
    const char* warning() const MM_NONBLOCKING override { return nullptr; }

    /// The layout's box, one light per panel pixel.
    size_t lights() const MM_NONBLOCKING override {
        return static_cast<size_t>(geo_.width) * geo_.height;
    }

    /// Pack the frame into the bit planes, brightness being the planes' lit time.
    void show(const uint8_t* rgb, uint8_t brightness) MM_NONBLOCKING override {
        geo_.brightness = brightness;
        // One buffer: a one-pass tear beats a second 256 KB frame, invisible at 300+ Hz.
        hub75Encode(rgb, platform::hub75Buffer(hub_), geo_);   // a null buffer, as with no bus open, makes hub75Encode return 0
    }

    /// Publish the measured refresh rate.
    void tick1s() MM_NONBLOCKING override {
        const uint16_t hz = platform::hub75RefreshHz(hub_);
        if (hz == lastRefresh_) return;      // a changed value alone re-serializes on the 1 Hz tick
        lastRefresh_ = hz;
        mm::formatTo(refreshStr_, sizeof(refreshStr_), "%u Hz", hz);
    }

    /// Stop the scan and release the panel's peripheral.
    void close() override { platform::hub75Deinit(hub_); }

    /// The block the selected backend drives, as the select names it, since claiming the wrong one frees a live block and holds a free one.
    LedHwBlock hwBlock() const override {
        return owner_->selectedBackend() == platform::Hub75Backend::Parlio ? LedHwBlock::Parlio
                                                                           : LedHwBlock::LcdCam;
    }

    /// A published board's lines for the pin map, since their hidden controls read as free there.
    uint8_t fixedPins(MoonModule::FixedPin* out, uint8_t max) const override {
        // Hidden means "not in use" everywhere else; here the lines are soldered and driven.
        if (!out || pinsEditable()) return 0;
        Line wired[kLineCount];
        uint8_t n = 0;
        for (uint8_t i = 0, count = lines(wired); i < count && n < max; i++) {
            out[n++] = MoonModule::FixedPin{static_cast<uint8_t>(wired[i].gpio), wired[i].role};
        }
        return n;
    }

private:
    /// The bit-plane geometry for a layout of `width` by `height` at the selected scan and depth.
    Hub75Geometry geometry(uint16_t width, uint16_t height) const {
        Hub75Geometry g;
        g.width = width;
        g.height = height;
        g.scanRate = Hub75Settings::kScanRates[owner_->scanSel < Hub75Settings::kScanCount ? owner_->scanSel : 0];
        g.bitDepth = owner_->bitDepth;
        return g;
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

    /// Whether this chip is an S3, as the platform names it; likewise for P4 and S31, since inferring the chip from GPIO counts is a guess dressed as a test.
    static bool chipIsS3()  { return platform::isEsp32S3; }
    static bool chipIsP4()  { return platform::isEsp32P4; }
    static bool chipIsS31() { return platform::isEsp32S31; }

    /// The boards this output knows, and the per-chip generic sets, which are editable free pins from gpio-usage.md rather than boards.
    static constexpr BoardPins kBoards[] = {
        // MoonHub75 PCB (Lilygo T7-S3). MOONHUB75/README.md names the upper half R0/G0/B0.
        {"MoonHub75",   1,  5,  6,   7, 13,  9,  16, 48, 47, 21, 38,  18,  8,  4, chipIsS3, false},
        // Adafruit MatrixPortal S3, from the board's own CircuitPython pins.c.
        {"MatrixPortal S3", 42, 41, 40,  38, 39, 37,  45, 36, 48, 35, 21,   2, 47, 14, chipIsS3, false},
        // Waveshare SKU 34422: WLED's array ends LAT,OE,CLK, so reading ...CLK,LAT,OE swaps three.
        {"Waveshare RGB Matrix", 4,  5,  6,   7, 15, 16,  18,  8,  3, 42,  9,  41, 40,  2, chipIsS3, false},
        // Generic sets: the first fourteen of each chip's free GPIOs, in ribbon order.
        {"S3 generic",  4,  5,  6,   7,  8,  9,  10, 11, 12, 13, 14,  15, 16, 17, chipIsS3, true},
        // The P4's and the S31's free sets are board-specific, so they ship unset rather than invented: the P4-NANO's 39-42 are the TF slot's on the WIFI6-DEV-KIT, off its header.
        {"P4 generic", -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, chipIsP4, true},
        {"S31 generic", -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, chipIsS31, true},
        // Waveshare ESP32-P4-NANO: the first fourteen of its free GPIOs (gpio-usage.md), in ribbon order.
        {"Waveshare P4-NANO", 20, 21, 22,  23, 24, 25,  26, 27, 32, 33, 39,  40, 41, 42, chipIsP4, true},
        // Custom keeps whatever is in the fields: the escape hatch for a hand-wired panel.
        {"Custom",     -1, -1, -1,  -1, -1, -1,  -1, -1, -1, -1, -1,  -1, -1, -1, nullptr, true},
    };
    static constexpr uint8_t kBoardCount = sizeof(kBoards) / sizeof(kBoards[0]);

    /// Offer the boards that make sense on this chip: a P4-generic row on an S3 would be fourteen pins that chip lacks.
    void buildBoardOptions() {
        uint8_t& boardSel = owner_->boardSel;
        const char* current = (boardSel < boardOptionCount_) ? boardOptions_[boardSel] : nullptr;
        boardOptionCount_ = 0;
        for (uint8_t i = 0; i < kBoardCount; i++) {
            // A host offers every row; silicon filters to its own chip's.
            constexpr bool anyKnownChip =
                platform::isEsp32S3 || platform::isEsp32P4 || platform::isEsp32S31;
            if constexpr (anyKnownChip) {
                if (kBoards[i].onThisChip && !kBoards[i].onThisChip()) continue;
            }
            boardOptions_[boardOptionCount_] = kBoards[i].label;
            boardIndex_[boardOptionCount_] = i;
            boardOptionCount_++;
        }
        // Re-point by LABEL, so a filtered list keeps the picked board while it is offered, and falls to the first row when it is not.
        boardSel = offeredIndexOf(current);
    }

    /// The offered row carrying `label`, or the first row when none does.
    uint8_t offeredIndexOf(const char* label) const {
        if (!label) return 0;
        for (uint8_t k = 0; k < boardOptionCount_; k++)
            if (std::strcmp(boardOptions_[k], label) == 0) return k;
        return 0;
    }

    // One HUB75 line as wired: the GPIO and the ribbon name it carries.
    struct Line { int8_t gpio; const char* role; };
    static constexpr uint8_t kLineCount = 14;

    /// The set lines in ribbon order: the one home for which GPIOs this output holds.
    uint8_t lines(Line* out) const {
        const Hub75Settings& settings = *owner_;
        const Line all[kLineCount] = {
            {settings.r1, "r1"}, {settings.g1, "g1"}, {settings.b1, "b1"},
            {settings.r2, "r2"}, {settings.g2, "g2"}, {settings.b2, "b2"},
            {settings.addrA, "a"}, {settings.addrB, "b"}, {settings.addrC, "c"}, {settings.addrD, "d"},
            {settings.addrE, "e"},
            {settings.clk, "clk"}, {settings.lat, "lat"}, {settings.oe, "oe"}};
        uint8_t n = 0;
        for (const Line& l : all) if (l.gpio >= 0) out[n++] = l;
        return n;
    }

    /// Are the pin controls the user's to edit, for the selected board?
    bool pinsEditable() const {
        const uint8_t boardSel = owner_->boardSel;
        if (boardSel >= boardOptionCount_) return true;   // no board resolved: show every pin
        return kBoards[boardIndex_[boardSel]].editable;
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

    /// Write the chosen board's map into the pin controls; Custom leaves the pins as they are, so switching to it keeps an edit.
    void applyBoard() {
        Hub75Settings& settings = *owner_;
        if (settings.boardSel >= boardOptionCount_) return;
        const BoardPins& b = kBoards[boardIndex_[settings.boardSel]];
        if (std::strcmp(b.label, "Custom") == 0) return;
        settings.r1 = b.r1; settings.g1 = b.g1; settings.b1 = b.b1;
        settings.r2 = b.r2; settings.g2 = b.g2; settings.b2 = b.b2;
        settings.addrA = b.a; settings.addrB = b.b; settings.addrC = b.c;
        settings.addrD = b.d; settings.addrE = b.e;
        settings.clk = b.clk; settings.lat = b.lat; settings.oe = b.oe;
    }

    const char* boardOptions_[kBoardCount] = {};
    uint8_t boardIndex_[kBoardCount] = {};
    uint8_t boardOptionCount_ = 0;
    bool prefilled_ = false;   // the board map is written once, at the first card

    /// Convert a Pin control's value to the platform seam's pin type; -1 for unset becomes 0xFFFF, since a P4 GPIO number can exceed int8_t's range.
    static uint16_t toPin(int8_t p) {
        return p < 0 ? 0xFFFF : static_cast<uint16_t>(p);
    }

    platform::Hub75Handle hub_{};
    Hub75Geometry geo_{};
    uint16_t lastRefresh_ = 0xFFFF;      // 0xFFFF, so the first real 0 still publishes
    char status_[64] = {};
    char refreshStr_[16] = {};
};

} // namespace mm
