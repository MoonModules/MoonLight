/// @module ParallelLedDriver
/// @also I80Peripheral, ParlioPeripheral

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"   // controlIndex
#include "platform/platform_leds.h"   // RmtLoopbackResult

#include <cstring>
#include <vector>

// Host test of the runtime peripheral swap: applyControlValue writes the select, then rebuildControls(), then onControlChanged("peripheral"), with two owned mock backends registered.

// The bug this pins: rebuildControls() already swaps and binds the new backend, so a second swap in onControlChanged freed the freshly bound backend and left its controls dangling.

namespace {

// The quiesce hook's fire count, and whether it had fired before the last attached backend was freed: the proof the worker stopped before its backend went.
int g_hookFires = 0;
bool g_hookFiredBeforeLastDtor = false;

// A minimal owned backend with a real hwBlock, counting its construction and destruction so a swap shows no leak and no double free.
struct SwapMock : mm::LedPeripheral {
    static inline int live = 0;      // net live instances (ctor++ / dtor--) — 0 at rest, no leak/double-free
    static inline int attachedDtors = 0;   // destructions of a backend the driver selected, the count probe churn cannot hide: the double swap freed one extra per change
    mm::LedHwBlock block;
    explicit SwapMock(mm::LedHwBlock b) : block(b) { live++; }
    // Only a selected backend carries an owner_; freeing one also records whether the quiesce hook had already fired.
    ~SwapMock() override {
        live--;
        if (owner_) { attachedDtors++; g_hookFiredBeforeLastDtor = (g_hookFires > 0); }
    }

    uint8_t lanesAvailable() const MM_NONBLOCKING override { return 8; }
    bool powerOfTwoBus() const override { return true; }
    bool loopbackFullWidth() const override { return false; }
    bool supportsPinExpander() const override { return false; }
    const char* initFailMsg() const override { return "mock init failed"; }
    mm::LedHwBlock hwBlock() const override { return block; }

    void addBusControls(mm::ControlList& controls) override { controls.addPin("clockPin", owner_->clockPin); }   // as both LCD backends declare it
    void addRingControls(mm::ControlList& controls) override { controls.addControl("ringBufs", owner_->ringBufs, 2, 64); }   // as LCD-MM declares it
    bool busControlTriggersBuild(const char*) const override { return false; }
    void recordBusPins() override {}
    bool extraBusPinsCurrent() const override { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const override { return nullptr; }
    const char* validateBusFatal() const override { return nullptr; }
    uint16_t clockPinForBus() const override { return 99; }

    bool busInit(size_t frameBytes, bool) override { cap_ = frameBytes; buf_.assign(frameBytes, 0); return true; }
    uint8_t* busBuffer(uint8_t i) override { return (i == 0 && !buf_.empty()) ? buf_.data() : nullptr; }
    size_t busCapacity() const override { return cap_; }
    bool busTransmit(uint8_t, size_t) override { transmits_++; return true; }
    bool busWait(uint8_t, uint32_t) override { return true; }
    uint32_t busLastTransmitUs() const override { return 0; }
    void busDeinit() override { cap_ = 0; buf_.clear(); }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) override { return {}; }

    size_t transmits_ = 0;
private:
    std::vector<uint8_t> buf_;
    size_t cap_ = 0;
};

// The registry is static: on host every real backend fills it, so a swap test saves it, installs its two mocks, and restores it on scope exit.
struct RegistryScope {
    mm::ParallelLedDriver::PeripheralEntry saved[mm::ParallelLedDriver::kMaxPeripherals];
    uint8_t savedCount;
    RegistryScope() {
        savedCount = mm::ParallelLedDriver::peripheralRegistryCount_;
        for (uint8_t i = 0; i < savedCount; i++) saved[i] = mm::ParallelLedDriver::peripheralRegistry_[i];
        mm::ParallelLedDriver::peripheralRegistryCount_ = 0;
        // "swapA" (Parlio block) and "swapB" (LcdCam block) model two distinct peripherals.
        mm::ParallelLedDriver::registerPeripheral("swapA", []() -> mm::LedPeripheral* {
            return new SwapMock(mm::LedHwBlock::Parlio);
        });
        mm::ParallelLedDriver::registerPeripheral("swapB", []() -> mm::LedPeripheral* {
            return new SwapMock(mm::LedHwBlock::LcdCam);
        });
    }
    ~RegistryScope() {
        mm::ParallelLedDriver::peripheralRegistryCount_ = savedCount;
        for (uint8_t i = 0; i < savedCount; i++) mm::ParallelLedDriver::peripheralRegistry_[i] = saved[i];
    }
};

// The Scheduler's control-change sequence for `peripheral`: write the index through the control, rebuild (the one swap), then onControlChanged, which keeps the backend in place.
void changePeripheralTo(mm::ParallelLedDriver& d, uint8_t index) {
    int i = mm::controlIndex(d, "peripheral");
    REQUIRE(i >= 0);
    *static_cast<uint8_t*>(d.controls()[static_cast<uint8_t>(i)].ptr) = index;
    d.rebuildControls();
    d.onControlChanged("peripheral");
}

// Wire a working driver on pins 1 and 2. A fresh driver seeds the first registered backend, swapA.
void wireDriver(mm::ParallelLedDriver& d, mm::Buffer& src, mm::Correction& corr) {
    std::strcpy(d.pins, "1,2");
    REQUIRE(src.allocate(64, 3) == true);
    mm::rebuildFromPreset(corr, 255, mm::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

// The `peripheral` Select's index for a backend's label, or -1: the board-filtered option list in registry order, which a test does not hardcode. A Select keeps its options in aux and their count in max.
int peripheralIndex(mm::ParallelLedDriver& d, const char* label) {
    const int sel = mm::controlIndex(d, "peripheral");
    if (sel < 0) return -1;
    const auto& c = d.controls()[static_cast<uint8_t>(sel)];
    const char* const* opts = reinterpret_cast<const char* const*>(c.aux);
    for (int k = 0; k < c.max; k++)
        if (opts[k] && std::strcmp(opts[k], label) == 0) return k;
    return -1;
}

} // namespace

TEST_CASE("ParallelLedDriver: a peripheral swap does not double-free or dangle the control list") {
    RegistryScope registry;   // install swapA/swapB, restore the real set on scope exit
    const int liveBefore = SwapMock::live;

    {
        mm::ParallelLedDriver d;
        mm::Buffer src;
        mm::Correction corr;
        wireDriver(d, src, corr);

        // Exactly ONE backend is live for a driver holding a single peripheral.
        CHECK(SwapMock::live == liveBefore + 1);

        const int aIdx = peripheralIndex(d, "swapA");
        const int bIdx = peripheralIndex(d, "swapB");
        REQUIRE(aIdx >= 0);
        REQUIRE(bIdx >= 0);

        // Swap A to B: the fixed path builds exactly one backend for one change, where the double swap built two; the built count is the discriminator, live counts net one either way.
        int dtorsBefore = SwapMock::attachedDtors;
        changePeripheralTo(d, static_cast<uint8_t>(bIdx));
        // A to B destroys exactly one attached backend, the outgoing A; the double swap destroyed two.
        CHECK(SwapMock::attachedDtors - dtorsBefore == 1);
        CHECK(SwapMock::live == liveBefore + 1);
        // The bus comes up, so hwBlock() reads B's block (LcdCam): the live backend is B, not a freed A.
        CHECK(d.hwBlock() == mm::LedHwBlock::LcdCam);

        // A tick after the swap transmits through the live backend; a dangling control list would be a use-after-free.
        d.tick20ms();
        d.tick();   // one render tick — reaches the live backend's busTransmit

        // Swap back B → A, then A → B again: repeated swaps must stay balanced (no accumulation), and hwBlock() tracks the live backend each time (A = Parlio, B = LcdCam).
        changePeripheralTo(d, static_cast<uint8_t>(aIdx));
        CHECK(SwapMock::live == liveBefore + 1);
        CHECK(d.hwBlock() == mm::LedHwBlock::Parlio);
        changePeripheralTo(d, static_cast<uint8_t>(bIdx));
        CHECK(SwapMock::live == liveBefore + 1);
        CHECK(d.hwBlock() == mm::LedHwBlock::LcdCam);
    }

    // Driver destroyed → its owned backend freed → back to the starting live count (no leak).
    CHECK(SwapMock::live == liveBefore);
}

// A live swap stops the encode worker (the quiesce hook) before freeing the backend the worker reads: SwapMock's destructor records whether the hook fired first, which a swap that freed first leaves false.
TEST_CASE("ParallelLedDriver: a live peripheral swap quiesces the render worker before freeing the backend") {
    RegistryScope registry;
    g_hookFires = 0;
    g_hookFiredBeforeLastDtor = false;

    // The hook records each fire; SwapMock's destructor records whether it had fired before an attached backend was freed, the ordering proof.
    mm::MoonModule::setQuiesceRenderHook([] { g_hookFires++; });

    {
        mm::ParallelLedDriver d;
        mm::Buffer src;
        mm::Correction corr;
        wireDriver(d, src, corr);
        const int bIdx = peripheralIndex(d, "swapB");
        REQUIRE(bIdx >= 0);

        // The fire count resets right before the swap, so the destructor's snapshot reflects this swap alone.
        g_hookFires = 0;
        g_hookFiredBeforeLastDtor = false;
        changePeripheralTo(d, static_cast<uint8_t>(bIdx));   // the live swap: frees the old backend (A)
        // The swap fired the quiesce hook at all...
        CHECK(g_hookFires > 0);
        // And it fired before the outgoing backend was freed.
        CHECK(g_hookFiredBeforeLastDtor);
    }

    mm::MoonModule::setQuiesceRenderHook(nullptr);
}

// The wiring is the board's and the ring's settings the user's, so both survive the peripheral they are declared through. Each backend once held its own copy at a default, so a switch there and back put WR on a data pin and reset the ring.
TEST_CASE("ParallelLedDriver: switching the peripheral and back keeps the clockPin and the ring's settings") {
    RegistryScope registry;
    mm::ParallelLedDriver d;
    mm::Buffer src;
    mm::Correction corr;
    wireDriver(d, src, corr);
    const int aIdx = peripheralIndex(d, "swapA");
    const int bIdx = peripheralIndex(d, "swapB");
    REQUIRE(aIdx >= 0);
    REQUIRE(bIdx >= 0);
    changePeripheralTo(d, static_cast<uint8_t>(aIdx));
    mm::setControlValue<int8_t>(d, "clockPin", 3);
    mm::setControlValue<uint8_t>(d, "ringBufs", 20);

    changePeripheralTo(d, static_cast<uint8_t>(bIdx));
    changePeripheralTo(d, static_cast<uint8_t>(aIdx));

    const int idx = mm::controlIndex(d, "clockPin");
    REQUIRE(idx >= 0);
    CHECK(*static_cast<const int8_t*>(d.controls()[static_cast<uint8_t>(idx)].ptr) == 3);
    CHECK(d.clockPin == 3);
    const int ring = mm::controlIndex(d, "ringBufs");
    REQUIRE(ring >= 0);
    CHECK(*static_cast<const uint8_t*>(d.controls()[static_cast<uint8_t>(ring)].ptr) == 20);
    CHECK(d.ringBufs == 20);
}

// The reset button's default comes from a fresh driver, which on an S3 starts on LCD-IDF: settings a ring-capable peripheral declared had none there.
TEST_CASE("ParallelLedDriver: the ring's settings are declared, hidden, without a ring-capable peripheral, so their defaults are known") {
    RegistryScope registry;
    mm::ParallelLedDriver d;
    d.defineControls();
    for (const char* name : {"shiftOverclock", "useRing", "ringSnapshot", "ringAuto", "ringRows", "ringBufs", "ringPadUs"}) {
        CAPTURE(name);
        const int idx = mm::controlIndex(d, name);
        REQUIRE(idx >= 0);
        CHECK(d.controls()[static_cast<uint8_t>(idx)].hidden);
    }
}
