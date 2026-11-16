/// @module ParallelLedDriver
/// @also I80Peripheral, ParlioPeripheral

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"

#include <cstring>
#include <string>   // std::string — clang gets it transitively, GCC does not
#include <vector>

// Host test of the deferred-wait DOUBLE-BUFFER logic in ParallelLedDriver::tick() (Step 1.5). The real LCD/Parlio peripherals are inert on the host (desktop stubs return null), so the alternation/wait/drain invariants can't be exercised through them. Instead a MockPeripheral (a runtime LedPeripheral backend) supplies the bus* hooks against two in-memory buffers and RECORDS the call sequence, so the orchestrator's loop behavior is pinned on the host exactly where the hardware would run it:
//   - double-buffer mode alternates encode target 0,1,0,1,… and waits on a buffer
//     only right before it's REUSED (never after every transmit);
//   - single-buffer mode (mock offers no buffer 1) stays on buffer 0 and waits every
//     frame, the old synchronous behavior, unchanged fps;
//   - a reinit/release drains BOTH buffers' in-flight transfers before freeing.
// The fps win itself is a hardware KPI (proven on the P4); this pins the mechanism.

namespace {

using mm::nrOfLightsType;

// A record of one bus call, so a test can assert the exact ordering the orchestrator emits.
struct Call {
    enum Kind { Transmit, Wait } kind;
    uint8_t buffer;
};

// LedPeripheral mock: two heap buffers, a settable "double-buffer available" flag, and a call log. Everything the orchestrator's tick()/reinit()/deinit() reach is here.
class MockPeripheral : public mm::LedPeripheral {
public:
    // --- test knobs / observation ---
    bool twoBuffers = true;               // false → single-buffer mode (busBuffer(1) == null)
    bool canDoubleBuffer = true;          // false → this peripheral reports supportsDoubleBuffer()==false
    std::vector<Call> calls;              // transmit/wait order, in call sequence

    // --- LedPeripheral hooks (mock, host-only) ---
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return 8; }   // pretend this chip has lanes
    bool powerOfTwoBus() const override { return false; }
    bool loopbackFullWidth() const override { return false; }
    // The mock bus is memory, not a peripheral, so it can host the 74HCT595 expander, which is what lets the shift-register lane/frame arithmetic be pinned on the host (unit_ParallelSlots covers the encoded bits; here it's the driver plumbing).
    bool supportsPinExpander() const override { return true; }
    bool supportsDoubleBuffer() const override { return canDoubleBuffer; }   // MoonI80 reports false
    const char* initFailMsg() const override { return "mock init failed"; }
    mm::LedHwBlock hwBlock() const override { return mm::LedHwBlock::None; }   // mock drives no real block

    // busInit gets `wantSecond` from the orchestrator (= doubleBuffer). The mock allocates the second buffer only when BOTH the flag wants it AND the test's twoBuffers knob allows it (so a test can simulate a memory-tight board that refuses the second buffer even with async on).
    bool busInit(size_t frameBytes, bool wantSecond) override {
        cap_ = frameBytes;
        buf_[0].assign(frameBytes, 0);
        if (wantSecond && twoBuffers) buf_[1].assign(frameBytes, 0);
        else                          buf_[1].clear();
        inited_ = true;
        return true;
    }
    uint8_t* busBuffer(uint8_t i) override {
        if (i >= 2 || buf_[i].empty()) return nullptr;
        return buf_[i].data();
    }
    size_t busCapacity() const override { return cap_; }
    bool busTransmit(uint8_t i, size_t /*bytes*/) override {
        calls.push_back({Call::Transmit, i});
        return true;   // the mock transfer always "starts"
    }
    // Returns whether the transfer completed. `waitTimesOut` makes every wait report a TIMEOUT, so a test can prove the driver refuses to re-encode into a buffer the DMA may still be reading.
    bool busWait(uint8_t i, uint32_t) override {
        calls.push_back({Call::Wait, i});
        return !waitTimesOut;
    }
    bool waitTimesOut = false;
    uint32_t busLastTransmitUs() const override { return lastTransmitUs; }   // mock wire-time KPI
    uint32_t lastTransmitUs = 0;   // a test can set this to check the frameTime string formatting
    void busDeinit() override { cap_ = 0; buf_[0].clear(); buf_[1].clear(); inited_ = false; }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) override {
        return {};
    }

private:
    std::vector<uint8_t> buf_[2];
    size_t cap_ = 0;
    bool inited_ = false;
};

// Wire a mock driver onto a `lights`-light source buffer + a GRB correction, and drive it ready. `async` sets doubleBuffer (whether the orchestrator requests a second buffer); `canSecond` is the mock's board-fits-a-second-buffer knob (lets a test simulate a memory-tight board that refuses it even with async on). Mirrors the other parallel-driver test helpers.
void wire(mm::ParallelLedDriver& d, MockPeripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          nrOfLightsType lights, bool async, bool canSecond = true) {
    peripheral.twoBuffers = canSecond;
    d.setPeripheralForTest(&peripheral);
    d.doubleBuffer = async;
    std::strcpy(d.pins, "1,2,3,4");
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::rebuildFromPreset(corr, 255, mm::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// Double-buffer mode: the encode target alternates 0,1,0,1,… and a buffer's wait fires only right BEFORE that buffer is reused, never after every transmit. So the first two ticks transmit without a preceding wait (both buffers start idle), and from tick 3 on each tick waits on the buffer it's about to reuse.
TEST_CASE("ParallelLedDriver double-buffer alternates and defers the wait") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/true);

    // Tick 1: buffer 0 idle → no wait, transmit 0, flip to 1.
    d.tick();
    CHECK(d.activeForTest() == 1);
    CHECK(d.inFlightForTest(0) == true);
    // Tick 2: buffer 1 idle → no wait, transmit 1, flip to 0.
    d.tick();
    CHECK(d.activeForTest() == 0);
    CHECK(d.inFlightForTest(1) == true);
    // Tick 3: about to reuse buffer 0 (in flight from tick 1) → wait 0, transmit 0.
    d.tick();
    CHECK(d.activeForTest() == 1);
    // Tick 4: reuse buffer 1 → wait 1, transmit 1.
    d.tick();
    CHECK(d.activeForTest() == 0);

    // The exact call order the orchestrator emitted.
    REQUIRE(peripheral.calls.size() == 6);
    CHECK(peripheral.calls[0].kind == Call::Transmit); CHECK(peripheral.calls[0].buffer == 0);
    CHECK(peripheral.calls[1].kind == Call::Transmit); CHECK(peripheral.calls[1].buffer == 1);
    CHECK(peripheral.calls[2].kind == Call::Wait);     CHECK(peripheral.calls[2].buffer == 0);
    CHECK(peripheral.calls[3].kind == Call::Transmit); CHECK(peripheral.calls[3].buffer == 0);
    CHECK(peripheral.calls[4].kind == Call::Wait);     CHECK(peripheral.calls[4].buffer == 1);
    CHECK(peripheral.calls[5].kind == Call::Transmit); CHECK(peripheral.calls[5].buffer == 1);
}

// Single-buffer mode (no second buffer): the driver stays on buffer 0 and waits on it EVERY frame before re-encoding, the old synchronous wait-after-transmit path, so a memory-tight board keeps its old fps rather than failing to init.
TEST_CASE("ParallelLedDriver single-buffer mode waits every frame on buffer 0") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/false);   // default: no second buffer, synchronous path

    d.tick();   // tickSync: transmit 0, wait 0
    CHECK(d.activeForTest() == 0);
    d.tick();   // transmit 0, wait 0
    CHECK(d.activeForTest() == 0);
    d.tick();   // transmit 0, wait 0

    // tickSync waits RIGHT AFTER each transmit (the original synchronous order): T0,W0 ×3.
    REQUIRE(peripheral.calls.size() == 6);
    CHECK(peripheral.calls[0].kind == Call::Transmit); CHECK(peripheral.calls[0].buffer == 0);
    CHECK(peripheral.calls[1].kind == Call::Wait);     CHECK(peripheral.calls[1].buffer == 0);
    CHECK(peripheral.calls[2].kind == Call::Transmit); CHECK(peripheral.calls[2].buffer == 0);
    CHECK(peripheral.calls[3].kind == Call::Wait);     CHECK(peripheral.calls[3].buffer == 0);
    CHECK(peripheral.calls[4].kind == Call::Transmit); CHECK(peripheral.calls[4].buffer == 0);
    CHECK(peripheral.calls[5].kind == Call::Wait);     CHECK(peripheral.calls[5].buffer == 0);
    // Buffer 1 is never allocated or touched.
    for (const auto& c : peripheral.calls) CHECK(c.buffer == 0);
}

// doubleBuffer is the on/off knob AND drives allocation: OFF (default) allocates ONE buffer and runs the synchronous path; ON requests a second buffer and alternates. Flipping it rebuilds the bus (affectsPrepare) so the second buffer is freed (→off) or allocated (→on), a board that leaves it off never holds the second buffer. This mirrors the live toggle (the A/B knob), which routes through applyState()/prepare() the same way.
TEST_CASE("ParallelLedDriver doubleBuffer toggles allocation and path") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/false);

    // OFF: single buffer only, buffer 1 was never allocated, and tick runs synchronous.
    CHECK(peripheral.busBuffer(1) == nullptr);
    d.tick();
    REQUIRE(peripheral.calls.size() == 2);   // T0, W0 — synchronous
    CHECK(peripheral.calls[0].kind == Call::Transmit);
    CHECK(peripheral.calls[1].kind == Call::Wait);

    // Flip ON and re-prepare (what a live control change does): the second buffer is now allocated.
    d.doubleBuffer = true;
    d.applyState();
    CHECK(peripheral.busBuffer(1) != nullptr);
    peripheral.calls.clear();
    d.tick();   // async: transmit 0, no wait, flip to 1
    d.tick();   // async: transmit 1, no wait, flip to 0
    CHECK(d.activeForTest() == 0);
    // Two transmits, no interleaved wait (both buffers idle at start), the deferred-wait pattern.
    REQUIRE(peripheral.calls.size() == 2);
    CHECK(peripheral.calls[0].kind == Call::Transmit); CHECK(peripheral.calls[0].buffer == 0);
    CHECK(peripheral.calls[1].kind == Call::Transmit); CHECK(peripheral.calls[1].buffer == 1);

    // Flip back OFF and re-prepare: the second buffer is freed, back to synchronous.
    d.doubleBuffer = false;
    d.applyState();
    CHECK(peripheral.busBuffer(1) == nullptr);
}

// Regression (MoonI80 double-buffer freeze): a peripheral that reports supportsDoubleBuffer()==false must run SINGLE-buffer even when the doubleBuffer control is ON, its own-GDMA two-buffer completion handshake races and wedges the bus (the ~200 ms-per-frame freeze). The orchestrator gates the second-buffer request on supportsDoubleBuffer(), so the peripheral never gets a second buffer and the tick stays on the proven synchronous path. The saved doubleBuffer value is preserved (it just doesn't engage here), so switching to a peripheral that DOES support it restores the async behavior.
TEST_CASE("ParallelLedDriver: a peripheral that can't double-buffer stays single-buffer with doubleBuffer ON") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    peripheral.canDoubleBuffer = false;   // the MoonI80 case: supportsDoubleBuffer() == false
    wire(d, peripheral, src, corr, 64, /*async=*/true);   // doubleBuffer ON, but the peripheral refuses async

    // The second buffer is NEVER requested (busInit got wantSecond=false), so it isn't allocated...
    CHECK(peripheral.busBuffer(1) == nullptr);
    // ...and the saved control value is untouched (survives a round-trip through this peripheral).
    CHECK(d.doubleBuffer == true);

    // The tick runs the SYNCHRONOUS single-buffer path: transmit 0 then wait 0, every frame, on buffer 0.
    d.tick();
    d.tick();
    for (const auto& c : peripheral.calls) CHECK(c.buffer == 0);   // buffer 1 never touched
    REQUIRE(peripheral.calls.size() == 4);
    CHECK(peripheral.calls[0].kind == Call::Transmit); CHECK(peripheral.calls[1].kind == Call::Wait);
    CHECK(peripheral.calls[2].kind == Call::Transmit); CHECK(peripheral.calls[3].kind == Call::Wait);
    CHECK(d.activeForTest() == 0);   // never flips — single-buffer stays on 0
}

// A board that WANTS async but can't fit the second buffer (memory-tight) degrades to single-buffer synchronous, never fails to init. doubleBuffer is on, but the mock refuses the second buffer.
TEST_CASE("ParallelLedDriver async degrades to synchronous when second buffer won't fit") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/true, /*canSecond=*/false);
    CHECK(peripheral.busBuffer(1) == nullptr);   // requested but didn't fit
    d.tick();
    REQUIRE(peripheral.calls.size() == 2);       // synchronous path (T0, W0)
    CHECK(peripheral.calls[0].kind == Call::Transmit);
    CHECK(peripheral.calls[1].kind == Call::Wait);
}

// The frameTime KPI: tick1s() pulls the platform's measured wire time via busLastTransmitUs(). The string formatting + the actual DMA timing are verified on hardware (the metric's whole point is a real wire measurement); here we just pin that tick1s reads the seam without crashing pre-first-frame.
TEST_CASE("ParallelLedDriver frameTime tick1s is safe before the first transfer") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/true);
    d.tick1s();                 // lastTransmitUs == 0 → placeholder path, must not divide by zero
    peripheral.lastTransmitUs = 7680;    // the 256-light WS2812 floor
    d.tick1s();                 // real path (1e6/7680 = 130 fps) — must not crash
    CHECK(peripheral.busLastTransmitUs() == 7680);
}

// Robustness + no-caps (regression for a live bootloop): a correction can carry ANY channel count (RGB=3, RGBW=4, RGBCCT=5, an N-channel fixture). The per-row encode scratch (wire_) is heap-sized to kMaxLanes × outChannels off the hot path, so a >4-channel correction lays out without overrun, the old fixed-4-byte-stride array overflowed and corrupted memory (the SE16 bootloop, 2026-07-13). The driver must DRIVE it (size the frame + encode), not idle and not crash.
TEST_CASE("ParallelLedDriver drives an N-channel (>4) correction without overflow") {
    using R = mm::ChannelRole;
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    d.setPeripheralForTest(&peripheral);
    mm::Buffer src;
    mm::Correction corr;
    d.doubleBuffer = true;
    std::strcpy(d.pins, "1,2,3,4");
    REQUIRE(src.allocate(64, 3) == true);
    // An 8-channel fixture-style correction (RGBW + 4 fixture roles), well over the old 4-byte slot.
    R roles[] = {R::Red, R::Green, R::Blue, R::White, R::Pan, R::Tilt, R::Dimmer, R::Zoom};
    corr.rebuild(255, roles, 8);
    CHECK(corr.outChannels == 8);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();

    // No error, the driver accepts the multi-channel correction and sizes a real frame (outCh=8 → frameBytes scales with 8). The encode runs and transmits; the ASan/valgrind-clean run (and the hardware regression on the SE16) is the overflow proof, a 4-byte stride would have corrupted memory here.
    CHECK(d.severity() != mm::ParallelLedDriver::Severity::Error);
    CHECK(d.frameBytes() > 0);
    // The status reports the total channel count for a multi-channel fixture (lights × channels), the DMX-universe footprint the user sizes against, not just the light count.
    CHECK(std::string(d.status()).find('(') != std::string::npos);   // "... (N channels)"
    CHECK(std::string(d.status()).find("channels") != std::string::npos);
    peripheral.calls.clear();
    d.tick();
    REQUIRE(peripheral.calls.size() >= 1);
    CHECK(peripheral.calls[0].kind == Call::Transmit);   // it actually drove the fixture
}

// A reinit (grid resize / pin edit) must drain BOTH buffers' in-flight transfers before freeing them, a live DMA reading a buffer about to be freed is a use-after-free. After two ticks both buffers are in flight; the resize's reinit waits on both before rebuilding. (async on → two buffers.)
TEST_CASE("ParallelLedDriver reinit drains both in-flight buffers") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/true);

    d.tick();   // transmit 0 (in flight)
    d.tick();   // transmit 1 (in flight)
    CHECK(d.inFlightForTest(0) == true);
    CHECK(d.inFlightForTest(1) == true);
    const size_t before = peripheral.calls.size();

    // Force a rebuild by growing the grid, which changes frameBytes → reinit().
    REQUIRE(src.allocate(256, 3) == true);
    d.setSourceBuffer(&src);
    d.applyState();   // re-parses + reinits; must drain both first

    // Both buffers were waited on during the drain (order-independent, assert both present).
    bool waited0 = false, waited1 = false;
    for (size_t i = before; i < peripheral.calls.size(); i++) {
        if (peripheral.calls[i].kind == Call::Wait && peripheral.calls[i].buffer == 0) waited0 = true;
        if (peripheral.calls[i].kind == Call::Wait && peripheral.calls[i].buffer == 1) waited1 = true;
    }
    CHECK(waited0);
    CHECK(waited1);
    // After the drain both flags are clear (the rebuild starts fresh on buffer 0).
    CHECK(d.inFlightForTest(0) == false);
    CHECK(d.inFlightForTest(1) == false);
    CHECK(d.activeForTest() == 0);
}

// A wait that TIMES OUT means the DMA may still be reading that buffer. Encoding into it anyway would hand a live transfer a half-rewritten frame, the exact corruption the timeout exists to prevent. (The seam used to return void, so the driver couldn't tell a completion from a timeout and cleared inFlight_ either way; 🐇 CodeRabbit caught it.) The contract now: on timeout the buffer STAYS in-flight, the frame is skipped, and the driver re-waits next tick, self-healing, never corrupting.
TEST_CASE("ParallelLedDriver: a timed-out wait never re-encodes into the live buffer") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/true);

    d.tick();   // encode+transmit buffer 0 → in flight
    d.tick();   // encode+transmit buffer 1 → in flight; next tick must reuse buffer 0
    REQUIRE(d.inFlightForTest(0) == true);

    peripheral.waitTimesOut = true;          // buffer 0's transfer is wedged (the DMA never completes)
    const size_t before = peripheral.calls.size();
    d.tick();                       // must NOT encode/transmit into buffer 0

    // It waited on 0 and then gave up, no Encode, no Transmit followed.
    bool transmitted = false;
    for (size_t i = before; i < peripheral.calls.size(); i++)
        if (peripheral.calls[i].kind == Call::Transmit) transmitted = true;
    CHECK_FALSE(transmitted);            // the live buffer was NOT reused
    CHECK(d.inFlightForTest(0) == true); // still marked in flight, so the next tick re-waits

    // The DMA completes: the very next tick proceeds normally, it self-heals, no reinit needed.
    peripheral.waitTimesOut = false;
    d.tick();
    bool transmittedNow = false;
    for (size_t i = before; i < peripheral.calls.size(); i++)
        if (peripheral.calls[i].kind == Call::Transmit) transmittedNow = true;
    CHECK(transmittedNow);
}

// After ENOUGH consecutive dead frames the driver GIVES UP (stops spending the render thread on a bus that won't deliver, a misconfigured bus must not starve the network). But give-up is not permanent: a TRANSIENT stall (the streaming ring's refill missing one deadline under a burst of HTTP load) must self-recover WITHOUT a reinit, or a momentary hiccup leaves the LEDs dark until the user touches a control. So once given up, the driver periodically lets one frame through; if the bus is alive again, output resumes on its own. This pins that retry-recovery.
TEST_CASE("ParallelLedDriver: give-up self-recovers on a periodic retry, no reinit needed") {
    mm::ParallelLedDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, /*async=*/true);

    // Wedge the bus and tick until it gives up: every wait times out, so each frame is a dead strike.
    peripheral.waitTimesOut = true;
    for (int i = 0; i < 40; i++) d.tick();   // well past kDeadFramesBeforeGiveUp
    CHECK(d.severity() == mm::DriverBase::Severity::Error);   // reported "output stalled"

    // While given up, a tick must NOT transmit every frame (that would keep spending the render thread).
    size_t mark = peripheral.calls.size();
    d.tick();
    bool transmittedWhileGivenUp = false;
    for (size_t i = mark; i < peripheral.calls.size(); i++)
        if (peripheral.calls[i].kind == Call::Transmit) transmittedWhileGivenUp = true;
    CHECK_FALSE(transmittedWhileGivenUp);   // this tick was inside the quiet window, not a retry

    // The bus comes back to life. Within one retry window the driver lets a frame through, it completes, and output resumes, no config change, no reinit. Recovery means the driver actually LEFT the give-up state (its Error status cleared), not merely that one retry Transmit happened: a retry that transmits but whose wait still fails would keep the driver given-up, and that must NOT count.
    peripheral.waitTimesOut = false;
    bool recovered = false;
    for (int i = 0; i < 200 && !recovered; i++) {   // several retry windows' worth of margin
        d.tick();
        if (d.severity() != mm::DriverBase::Severity::Error) recovered = true;
    }
    CHECK(recovered);   // the give-up Error state cleared — the driver is transmitting normally again

    // And it keeps transmitting on the following ticks (steady-state, not a one-off retry blip).
    const size_t after = peripheral.calls.size();
    d.tick();
    bool stillTransmitting = false;
    for (size_t j = after; j < peripheral.calls.size(); j++)
        if (peripheral.calls[j].kind == Call::Transmit) stillTransmitting = true;
    CHECK(stillTransmitting);
}
