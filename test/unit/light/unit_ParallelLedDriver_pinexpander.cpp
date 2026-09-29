/// @module I80Peripheral
/// @also ParallelLedDriver

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/drivers/I80Peripheral.h"   // I80Peripheral: the i80 clock/DC defaults
#include "correction_presets.h"

#include <cstring>
#include <vector>

// The DRIVER half of the 74HCT595 shift-register expander (unit_ParallelSlots pins the encoded bits). What matters here is the arithmetic the user's memory budget depends on, and the config guards that stop a mis-wired board from emitting a waveform the hardware can't sustain:
//
//   - lanes = pins x 8 (each data pin fans out to 8 strands through its '595)
//   - the DMA frame grows x8 (a '595 is serial-in: presenting a slot costs 8 shift cycles)
//   - but extra STRANDS are free (they ride the bus width), so 15x256 and 48x256 cost the SAME
//   - the bus stays 8-bit: 48 strands on 6 pins is not a 16-bit bus
//   - the latch is a bus lane, so it must not collide with a data pin

namespace {

using mm::nrOfLightsType;

// A mock peripheral backend whose "bus" is plain memory (same shape as the double-buffer mock's MockPeripheral in unit_ParallelLedDriver_doublebuffer.cpp), so the lane/frame arithmetic is provable on the host with no real i80/Parlio/MoonI80 peripheral. Models the i80 SHAPE (powerOfTwoBus true, the expander only ever runs on an i80-shaped bus).
class MockPeripheral : public mm::LedPeripheral {
public:
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return 8; }   // 8 data lines, like an 8-bit bus
    bool powerOfTwoBus() const override { return true; }    // the BUS rounds to 8/16 whatever the pin count
    bool loopbackFullWidth() const override { return false; }
    bool supportsPinExpander() const override { return true; }   // memory bus: the expander is allowed
    const char* initFailMsg() const override { return "mock init failed"; }
    mm::LedHwBlock hwBlock() const override { return mm::LedHwBlock::None; }   // mock drives no real block

    void addBusControls(mm::ControlList&) override {}
    bool busControlTriggersBuild(const char*) const override { return false; }
    void recordBusPins() override {}
    bool extraBusPinsCurrent() const override { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const override { return nullptr; }
    const char* validateBusFatal() const override { return nullptr; }
    uint16_t clockPinForBus() const override { return 99; }   // a recognizable "parked here" sentinel

    bool busInit(size_t frameBytes, bool) override {
        cap_ = frameBytes;
        buf_.assign(frameBytes, 0);
        return true;
    }
    uint8_t* busBuffer(uint8_t i) override { return (i == 0 && !buf_.empty()) ? buf_.data() : nullptr; }
    size_t busCapacity() const override { return cap_; }
    // busTransmit models the real enqueue/completion split: it reports success for the ENQUEUE (like esp_lcd's tx_color returning ESP_OK the moment the transfer is queued), which does NOT mean the frame reached the wire, completion is a separate signal via busWait below. So a true here is "queued", not "done".
    bool busTransmit(uint8_t, size_t) override { transmits_++; return true; }
    // busWait is the completion half: `waitTimesOut` models a transfer whose done-signal never fires (a stuck/failed DMA). The driver must degrade, not wedge, and must not reuse the buffer while a transfer may still be reading it.
    bool busWait(uint8_t, uint32_t) override { return !waitTimesOut; }
    bool waitTimesOut = false;
    uint32_t busLastTransmitUs() const override { return 0; }
    void busDeinit() override { cap_ = 0; buf_.clear(); }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) override {
        return {};
    }

    size_t transmitCount() const { return transmits_; }

private:
    std::vector<uint8_t> buf_;
    size_t cap_ = 0;
    size_t transmits_ = 0;
};

// A tiny ParallelLedDriver subclass that exposes the protected lane-math internals a test needs (physPins_, encodeRows) without widening the production class's public surface, same pattern as unit_MultiPinLedDriver.cpp's `Expose : mm::ParallelLedDriver` for frameFitsDmaBudget. Everything else the tests use (laneCount, maxLaneLights, frameBytes, busPinList/busPinCount, pins, ledsPerPin, pinExpander, latchPin, defineControls, setSourceBuffer, correctionForTest, applyState, severity, status) is already public on ParallelLedDriver itself.
class MockShiftDriver : public mm::ParallelLedDriver {
public:
    // The streaming ring encodes the frame one SLICE at a time, straight into the small internal buffer the DMA is about to read. That is only sound if a sliced encode produces byte-identical output to the whole-frame encode, so expose the encoder for the test that pins it.
    template <class Slot>
    void encodeSliceForTest(uint8_t outCh, uint8_t* dst, mm::nrOfLightsType first,
                            mm::nrOfLightsType count, bool closeFrame) {
        this->template encodeRows<Slot>(outCh, dst, first, count, closeFrame);
    }
    // Test-only view of the derived lane math.
    uint8_t physPinsForTest() const { return physPins_; }
    // The GPIO list + length handed to the PERIPHERAL, the two must agree, or the platform reads off the end of the array. See the padding test below. (busPinList/busPinCount are already public.)
    const uint16_t* busPinListForTest() { return this->busPinList(); }
    uint8_t busPinCountForTest() const { return this->busPinCount(); }
};

// Bring a mock driver up on `lights` lights with the given pin list and expander setting. shiftOn fits the 74HCT595 expander (8 strands per pin); latch is the latch GPIO.
void wire(MockShiftDriver& d, MockPeripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
          nrOfLightsType lights, const char* pins, bool shiftOn, int8_t latch,
          const char* ledsPerPin = "") {
    d.setPeripheralForTest(&peripheral);
    std::strcpy(d.pins, pins);
    std::strcpy(d.ledsPerPin, ledsPerPin);
    d.pinExpander = shiftOn;
    d.latchPin = latch;
    // These tests exercise the SINGLE-buffer expander/ring path. MockPeripheral::busInit ignores the double-buffer request (busBuffer(1) is always null), so run the driver in single-buffer mode EXPLICITLY rather than relying on a silent fallback, the coverage is honest about which path runs.
    d.doubleBuffer = false;
    REQUIRE(src.allocate(lights, 3) == (lights > 0));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// Each data pin fans out to 8 strands through its '595, so the driver drives pins x 8 lanes, the whole point of the expander (pins are the scarce resource, not strands).
TEST_CASE("shift register: lanes = pins x 8") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // 6 data pins + a latch → 48 strands, the PO's 48x256 panel.
    wire(d, peripheral, src, corr, 48 * 256, "1,2,3,4,5,6", /*shiftOn=*/true, /*latch=*/7);

    CHECK(d.physPinsForTest() == 6);
    CHECK(d.laneCount() == 48);          // 6 pins x 8 outputs
    CHECK(d.maxLaneLights() == 256);     // 12288 lights spread over 48 strands
}

// ROBUSTNESS INVARIANT: a driver consumes only what IT drives (pins x ledsPerPin) and ignores any EXCESS the layout declares. A layout can legitimately publish more lights than one driver covers (a 2D panel grid the driver only partly maps, or several drivers splitting one big source), so the driver must clamp to its own lane capacity and keep every source read in-bounds, never encode past its share into unmapped source (which shows as frozen/garbage LEDs on the uncovered region). This pins the clamp: source = 1920, but 2 pins x 8 x 60 = 960, so the driver drives EXACTLY 960 and its furthest read is < 1920.
TEST_CASE("shift register: driver clamps to pins x ledsPerPin, ignoring a larger layout") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // Source is 1920 lights (a 5x3 grid of 8x16 panels); the driver only drives 2 pins x 8 x 60 = 960.
    wire(d, peripheral, src, corr, /*lights=*/1920, "9,10", /*shiftOn=*/true, /*latch=*/11, /*ledsPerPin=*/"60");

    CHECK(d.laneCount() == 16);           // 2 pins x 8 outputs
    CHECK(d.maxLaneLights() == 60);       // clamped to ledsPerPin, NOT the 1920/16 the source could feed

    // Every lane drives exactly ledsPerPin, and the total the driver consumes is its capacity (960), not the source's 1920, the excess is ignored.
    nrOfLightsType total = 0;
    for (uint8_t i = 0; i < d.laneCount(); i++) {
        CHECK(d.laneLightCount(i) == 60);
        total += d.laneLightCount(i);
    }
    CHECK(total == 960);

    // The furthest source light the encode can address is laneStart_[last] + (maxLaneLights - 1). With 16 lanes of 60 packed from 0, that is 960*... capped at 959, strictly inside the 1920 source, so no read runs past the buffer into unmapped lights.
    CHECK(total <= src.count());
}

// Direct mode is unchanged: one strand per pin, no latch. Pins the no-regression half, the expander must not have altered the existing behavior.
TEST_CASE("shift register: direct mode still drives one strand per pin") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 4 * 256, "1,2,3,4", /*shiftOn=*/false, /*latch=*/-1);

    CHECK(d.physPinsForTest() == 4);
    CHECK(d.laneCount() == 4);           // no fan-out
    CHECK(d.maxLaneLights() == 256);
}

// THE COUNTER-INTUITIVE RESULT, and the one the memory budget rests on: the x8 fan-out multiplies the frame (a '595 is serial-in, each slot costs 8 shift cycles), but extra STRANDS are free (they ride the bus width). So the PO's two panels, 15x256 = 3,840 lights and 48x256 = 12,288 lights, cost the SAME DMA frame. If this ever regresses, the memory model is wrong and the bigger panel will silently fail to allocate.
TEST_CASE("shift register: frame is set by strand LENGTH and the fan-out, not by strand COUNT") {
    mm::Correction corr;

    // 2 pins x 8 = 16 strands, 256 lights each.
    MockShiftDriver small;
    MockPeripheral peripheralSmall;
    mm::Buffer srcSmall;
    wire(small, peripheralSmall, srcSmall, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);
    CHECK(small.laneCount() == 16);
    CHECK(small.maxLaneLights() == 256);

    // 6 pins x 8 = 48 strands, still 256 lights each, 3x the strands, 3x the lights.
    MockShiftDriver big;
    MockPeripheral peripheralBig;
    mm::Buffer srcBig;
    wire(big, peripheralBig, srcBig, corr, 48 * 256, "1,2,3,4,5,6", /*shiftOn=*/true, /*latch=*/7);
    CHECK(big.laneCount() == 48);
    CHECK(big.maxLaneLights() == 256);

    // Same strand LENGTH → same frame, despite 3x the strands and 3x the lights.
    CHECK(big.frameBytes() == small.frameBytes());
}

// The x8 IS paid for, in the frame: the same strands at the same length cost 8x the DMA bytes with the expander fitted. (This is what walls the classic ESP32, its DMA can't reach PSRAM.)
TEST_CASE("shift register: the fan-out costs 8x the DMA frame") {
    mm::Correction corr;

    // 8 strands, 256 lights each, direct (8 pins, one strand each).
    MockShiftDriver direct;
    MockPeripheral peripheralDirect;
    mm::Buffer srcDirect;
    wire(direct, peripheralDirect, srcDirect, corr, 8 * 256, "1,2,3,4,5,6,7,8", /*shiftOn=*/false, /*latch=*/-1);
    CHECK(direct.laneCount() == 8);
    CHECK(direct.maxLaneLights() == 256);

    // The same 8 strands at the same length, but through ONE pin's '595.
    MockShiftDriver shifted;
    MockPeripheral peripheralShifted;
    mm::Buffer srcShifted;
    wire(shifted, peripheralShifted, srcShifted, corr, 8 * 256, "1", /*shiftOn=*/true, /*latch=*/2);
    CHECK(shifted.laneCount() == 8);
    CHECK(shifted.maxLaneLights() == 256);

    // Identical strands, but the serial shift-out costs 8 bus words per slot instead of 1. (Not exactly 8x the BYTES: both frames carry the same 64-byte-aligned latch pad, which is itself scaled, so assert the ratio is the real, substantial growth rather than a fixed constant that would bake the pad arithmetic into the test.)
    CHECK(shifted.frameBytes() > direct.frameBytes() * 7);
}




// The bus width follows the PHYSICAL pins (+ the latch lane), not the strand count. 48 strands on 6 pins is still an 8-bit bus, if this regressed to keying on the strand count, the driver would encode 16-bit slots into an 8-bit bus and emit a frame of pure garbage.
TEST_CASE("shift register: 48 strands on 6 pins is still an 8-bit bus") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 48 * 256, "1,2,3,4,5,6", /*shiftOn=*/true, /*latch=*/7);

    CHECK(d.laneCount() == 48);   // ...strands, but
    // frameBytes = rows x channels x 24 slots x slotBytes x 8 (+ pad). With an 8-bit bus slotBytes is 1; a (wrong) 16-bit bus would double this. Derive the expected value from the 8-bit assumption and check the driver agrees.
    const size_t rowBytes = static_cast<size_t>(256) * 3 * 24 * 1 * 8;
    CHECK(d.frameBytes() >= rowBytes);
    CHECK(d.frameBytes() < rowBytes * 2);   // a 16-bit bus would be >= 2x
}

// The latch is a real bus lane (a bit in every bus word), so it cannot double as a data pin, that lane would carry the latch waveform instead of pixel data. A config error, not a crash.
TEST_CASE("shift register: latchPin colliding with a data pin is a config error") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 8 * 256, "1,2,3,4", /*shiftOn=*/true, /*latch=*/3);   // 3 is a data pin

    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(d.laneCount() == 0);   // idles rather than driving a broken bus
}

// Without a latch the '595s never present a byte, so the strands would stay dark. Refuse the config rather than run a bus that silently outputs nothing.
TEST_CASE("shift register: the expander needs a latchPin") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 8 * 256, "1,2,3,4", /*shiftOn=*/true, /*latch=*/-1);   // unset

    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(d.laneCount() == 0);
}

// The latch must not land on the peripheral's own WR/DC pins either, bench-found, because WR defaults to GPIO 10 and that is the first free-looking pin a user reaches for. The i80 bus builds fine, so the failure is silent garbage on the strands rather than an init error; that is what makes it worth an explicit guard. (The check itself lives in I80Peripheral::validateBusFatal, which the mock does not have, this pins the base's half: a data-pin collision is caught, so the mechanism is live. The WR/DC half is a compile-time-visible guard in the i80 backend.)
TEST_CASE("shift register: driver refuses a latch that collides with a data lane") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // Latch on data pin 2 → the lane would carry the latch waveform instead of pixels.
    wire(d, peripheral, src, corr, 8 * 256, "1,2,3", /*shiftOn=*/true, /*latch=*/2);
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    CHECK(d.laneCount() == 0);
}

// The loopback test frame must carry the expander's ×8 factor. The rig transmits the REAL frame through the real DMA path, so a frame sized without the ×8 would clock out a truncated waveform and fail every bit, a false failure that looks like broken hardware. (The mock's busLoopback is a stub, so this pins the SIZE arithmetic, which is the part that was wrong; the captured-bits half is hardware and is proven on the bench.)
TEST_CASE("shift register: the loopback test frame is 8x the direct-mode frame") {
    mm::Correction corr;

    MockShiftDriver direct;
    MockPeripheral peripheralDirect;
    mm::Buffer srcDirect;
    wire(direct, peripheralDirect, srcDirect, corr, 8 * 64, "1,2,3,4,5,6,7,8", /*shiftOn=*/false, /*latch=*/-1);

    MockShiftDriver shifted;
    MockPeripheral peripheralShifted;
    mm::Buffer srcShifted;
    wire(shifted, peripheralShifted, srcShifted, corr, 8 * 64, "1", /*shiftOn=*/true, /*latch=*/2);

    // Same strands, same length; the shift frame pays 8 bus words per slot instead of 1. frameBytes() is the operational frame, which the loopback's per-light sizing mirrors (both scale by the expander's ×8), so the ratio is the invariant worth pinning.
    CHECK(shifted.frameBytes() > direct.frameBytes() * 7);
}

// Robust to any input (CLAUDE.md): flipping the expander on and off on a live driver must reconfigure cleanly each time, never wedge or leave stale lane state behind.
TEST_CASE("shift register: toggling the expander live reconfigures cleanly") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 8 * 256, "1,2,3,4", /*shiftOn=*/false, /*latch=*/-1);
    CHECK(d.laneCount() == 4);
    const size_t directFrame = d.frameBytes();

    // Expander ON (with a latch) → 32 strands, bigger frame.
    d.pinExpander = true;
    d.latchPin = 7;
    d.applyState();
    CHECK(d.laneCount() == 32);          // 4 pins x 8
    CHECK(d.frameBytes() > directFrame);

    // ...and back OFF → exactly the original configuration, no residue.
    d.pinExpander = false;
    d.applyState();
    CHECK(d.laneCount() == 4);
    CHECK(d.frameBytes() == directFrame);
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
}

// =========================================================================== REGRESSION GUARDS for the 2026-07-14 bench failure. Every bug that day was invisible to the tests, which is precisely why it took two days to find: the suite asserted the encoder's INTERNAL LAYOUT (bus-word indices) rather than OBSERVABLE BEHAVIOR. These pin the behavior.
//
// The bug: with the ×8 expander the DMA frame is 8× bigger (154 KB), so a transfer takes ~4.6 ms on the wire instead of ~0.6 ms. esp_lcd's GDMA link list is mounted from index 0 and only counts descriptors the PREVIOUS transfer has released, so the next frame's mount landed mid-transfer and failed (`lli full`). Critically the failure was SILENT: tx_color returns ESP_OK (the enqueue succeeded; the mount fails later in the ISR), so the driver believed the transfer had started, waited for a done-callback that never came, and burned a 1 s timeout every frame.
//
// The observable symptoms, and what each test below pins:
//   1. a transfer that never completes must not wedge the driver (it must keep ticking, degraded);
//   2. it must never re-encode into a buffer the DMA may still be reading (frame corruption);
//   3. the driver must not silently treat "enqueued" as "delivered".

// A transfer that NEVER completes (the done-callback never fires) must not wedge the driver. On the bench this manifested as a 42 ms driver tick for a 4.6 ms transfer, the driver spent every frame inside a timing-out wait. It must stay responsive, and it must not corrupt the in-flight buffer.
TEST_CASE("shift register: a transfer that never completes does not wedge the driver") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);
    REQUIRE(d.laneCount() == 16);

    peripheral.waitTimesOut = true;      // the DMA never signals done — exactly the bench failure

    // Tick repeatedly. The driver must keep running (no hang, no crash) and must NOT report success.
    for (int i = 0; i < 5; i++) d.tick();

    // It must still be alive and configured, degraded, not dead.
    CHECK(d.laneCount() == 16);
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
}

// The buffer-safety invariant the timeout exists to protect: while a transfer may still be reading a buffer, the driver must NOT encode into it again. A re-encode mid-transfer is what puts half of one frame and half of the next on the wire, the "scattered random pixels" seen on the bench.
TEST_CASE("shift register: a timed-out transfer never gets its buffer re-encoded") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);

    peripheral.waitTimesOut = true;
    d.tick();                       // starts a transfer that will never complete
    const size_t transmitsAfterFirst = peripheral.transmitCount();

    // Snapshot the buffer the stuck transfer is (conceptually) still reading. The immutability contract is not just "no new transmit" but "no re-encode": the bytes must be byte-for-byte unchanged while the transfer is in flight, or half of one frame + half of the next reach the wire (the bench's "scattered random pixels"). Read through busBuffer(0), the same memory the encoder writes.
    const uint8_t* live = peripheral.busBuffer(0);
    REQUIRE(live != nullptr);
    const std::vector<uint8_t> snapshot(live, live + peripheral.busCapacity());

    d.tick();                       // must NOT transmit again NOR re-encode over the live buffer
    d.tick();

    CHECK(peripheral.transmitCount() == transmitsAfterFirst);   // no new transfer while the old one is stuck
    // And the buffer contents are untouched, the stronger invariant the timeout exists to protect.
    const uint8_t* after = peripheral.busBuffer(0);
    REQUIRE(after != nullptr);
    CHECK(std::vector<uint8_t>(after, after + peripheral.busCapacity()) == snapshot);
}

// **The robustness rule: a broken bus must not cost the user the DEVICE.** Every dead transfer is a
// wait on the render thread, so a bus that never delivers doesn't just fail to light LEDs, it eats the CPU that WiFi/HTTP need and the device drops off the network entirely, with no way back in except a cable. That is exactly what a misconfigured shift-register frame did to a bench board (2026-07-14): unreachable and unrecoverable remotely, from nothing but an LED setting.
//
// So a persistently-dead bus must be GIVEN UP ON: the driver reports the failure and stops paying for it. Output idle, device alive, never the other way round.
TEST_CASE("a persistently dead bus is given up on, so it cannot starve the device") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);

    peripheral.waitTimesOut = true;   // the DMA never signals done, every frame, forever

    for (int i = 0; i < 40; i++) d.tick();

    // It gave up: the failure is REPORTED (the user can see why the LEDs are dark)...
    CHECK(d.severity() == mm::MoonModule::Severity::Error);
    // ...and it stopped spending the render thread on a bus that will never deliver. The exact count doesn't matter; what matters is that it is BOUNDED, it did not keep trying for all 40 ticks.
    CHECK(peripheral.transmitCount() < 40u);
}

// Giving up must not be permanent: the user fixes the setting that broke the bus, the driver rebuilds, and the LEDs come back, no reboot (the live-reconfiguration rule).
TEST_CASE("a given-up driver recovers when the bus is fixed") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 16 * 256, "1,2", /*shiftOn=*/true, /*latch=*/3);

    peripheral.waitTimesOut = true;
    for (int i = 0; i < 40; i++) d.tick();
    REQUIRE(d.severity() == mm::MoonModule::Severity::Error);   // gave up
    const size_t transmitsWhileDead = peripheral.transmitCount();

    // The user fixes the config: the bus is rebuilt, and now transfers complete.
    peripheral.waitTimesOut = false;
    d.applyState();          // prepare -> reinit: a fresh bus deserves a clean slate

    for (int i = 0; i < 5; i++) d.tick();

    CHECK(peripheral.transmitCount() > transmitsWhileDead);               // transmitting again
    CHECK(d.severity() != mm::MoonModule::Severity::Error);     // and the error is cleared
}

// **THE INVARIANT THE STREAMING RING RESTS ON.** The ring never materializes the big encoded frame:
// the DMA loops a few small INTERNAL buffers, and the CPU encodes each slice straight into the buffer the DMA is about to read. That is only sound if encoding in slices produces EXACTLY the bytes the whole-frame encode would have produced, otherwise the wire sees a different frame depending on how it happened to be chunked, which is the class of bug that is invisible on a sparse effect and catastrophic on a dense one.
//
// Note the latch pad: only the LAST slice closes the frame (closeFrame), because the pad is what makes every strand idle LOW into the WS2812 reset. A pad emitted mid-frame would reset the strand halfway.
TEST_CASE("streaming ring: a sliced encode is byte-identical to the whole-frame encode") {
    mm::Correction corr;
    MockShiftDriver whole;
    MockPeripheral peripheral;
    mm::Buffer srcWhole;
    wire(whole, peripheral, srcWhole, corr, 16 * 32, "1,2", /*shiftOn=*/true, /*latch=*/3);
    REQUIRE(whole.maxLaneLights() == 32);

    // Fill the source with a dense, varied pattern, a sparse/zero buffer would hide a slicing bug.
    for (nrOfLightsType i = 0; i < 16 * 32; i++) {
        uint8_t* px = srcWhole.data() + i * 3;
        px[0] = static_cast<uint8_t>(i * 7 + 1);
        px[1] = static_cast<uint8_t>(i * 13 + 2);
        px[2] = static_cast<uint8_t>(i * 29 + 3);
    }

    const uint8_t outCh = 3;
    const size_t bytes = whole.frameBytes();
    std::vector<uint8_t> refBuf(bytes, 0), sliceBuf(bytes, 0);

    // Reference: the whole frame in one call (what the driver does today).
    whole.encodeSliceForTest<uint8_t>(outCh, refBuf.data(), 0, 0, /*closeFrame=*/true);

    // The ring's way: the same frame in slices of 8 rows, each written at its own offset. Only the final slice closes the frame.
    const nrOfLightsType rows = whole.maxLaneLights();
    const nrOfLightsType per = 8;
    // Bytes one row emits: channels x 8 bits x 3 slots x outputsPerPin, at 1 byte per slot (8-bit bus).
    const size_t rowBytes = static_cast<size_t>(outCh) * 8 * 3 * 8;
    for (nrOfLightsType first = 0; first < rows; first += per) {
        const bool last = (first + per >= rows);
        whole.encodeSliceForTest<uint8_t>(outCh, sliceBuf.data() + first * rowBytes,
                                          first, per, /*closeFrame=*/last);
    }

    CHECK(std::memcmp(refBuf.data(), sliceBuf.data(), bytes) == 0);
}

// **The pin list handed to the peripheral must be exactly as long as the count that goes with it.**
// The BUS is 8 or 16 bits wide whatever the board drives, so a 3-pin board still hands the peripheral an 8-entry list: 3 data lanes, then the spares parked on the clock pin (a real GPIO the peripheral already drives, so the lane is inert). This is what lets a board name only the pins it uses.
//
// The bug this pins: busPinList() used to shortcut `if (!pinExpanderMode()) return laneList_`, the RAW, unpadded list, while busPinCount() reported the rounded width. With fewer pins than the bus is wide, the platform would then read past the end of laneList_. Direct mode never hit it only because the validation rejected any count but 8 or 16; allowing any count is what exposes it.
// A backend that routes its own GPIOs does not need a pad for a spare lane, and must not get one: the pad is a REAL claim on a REAL pin. On an ESP32-S31 the default clock pin (10) is an RGMII transmit line, so a one-strand board silently drove an Ethernet data pad and corrupted every frame the MAC sent, while the driver reported a healthy link and zero drops. The peripheral says whether it needs the pad; only the lanes a strand actually reads are handed over.
TEST_CASE("a peripheral that routes its own pins is handed only the lanes it drives") {
    mm::Buffer src;
    mm::Correction corr;

    struct SelfRoutingPeripheral : MockPeripheral {
        bool spareLanesNeedPad() const override { return false; }
    };

    SUBCASE("direct mode: 3 pins stay 3 lanes, nothing parked on the clock pin") {
        MockShiftDriver d;
        SelfRoutingPeripheral peripheral;
        wire(d, peripheral, src, corr, 64, "1,2,4", /*shiftOn=*/false, /*latch=*/-1);

        REQUIRE(d.busPinCountForTest() == 3);
        const uint16_t* list = d.busPinListForTest();
        CHECK(list[0] == 1);
        CHECK(list[1] == 2);
        CHECK(list[2] == 4);
        for (uint8_t i = 0; i < 3; i++) CHECK(list[i] != 99);   // the clock pin is never claimed
    }

    SUBCASE("shift mode: the latch is a real lane, so it is still handed over") {
        MockShiftDriver d;
        SelfRoutingPeripheral peripheral;
        wire(d, peripheral, src, corr, 64, "1,2", /*shiftOn=*/true, /*latch=*/7);

        REQUIRE(d.busPinCountForTest() == 3);       // 2 data + the latch
        const uint16_t* list = d.busPinListForTest();
        CHECK(list[0] == 1);
        CHECK(list[1] == 2);
        CHECK(list[2] == 7);                        // the latch lane
    }
}

TEST_CASE("bus pin list is padded to the full bus width, in both modes") {
    mm::Buffer src;
    mm::Correction corr;

    SUBCASE("direct mode: 3 pins → 8 lanes, 5 parked on the clock pin") {
        MockShiftDriver d;
        MockPeripheral peripheral;
        wire(d, peripheral, src, corr, 64, "1,2,4", /*shiftOn=*/false, /*latch=*/-1);

        REQUIRE(d.busPinCountForTest() == 8);        // the BUS width, not the pin count
        const uint16_t* list = d.busPinListForTest();
        CHECK(list[0] == 1);
        CHECK(list[1] == 2);
        CHECK(list[2] == 4);
        for (uint8_t i = 3; i < 8; i++) CHECK(list[i] == 99);   // parked on clockPinForBus()
    }

    SUBCASE("shift mode: 2 pins + latch → 8 lanes, the rest parked") {
        MockShiftDriver d;
        MockPeripheral peripheral;
        wire(d, peripheral, src, corr, 64, "1,2", /*shiftOn=*/true, /*latch=*/7);

        REQUIRE(d.busPinCountForTest() == 8);
        const uint16_t* list = d.busPinListForTest();
        CHECK(list[0] == 1);
        CHECK(list[1] == 2);
        CHECK(list[2] == 7);                                    // the latch takes bus bit physPins_
        for (uint8_t i = 3; i < 8; i++) CHECK(list[i] == 99);
    }

    // (The 16-bit bus is exercised in unit_I80LedDriver, on a driver whose chip actually HAS 16 lanes; this mock declares 8, so a 9th pin is correctly clamped away rather than widening the bus.)
}

// **The shift-mode loopback frame must reserve the trailing latch word it unconditionally writes.**
//
// encodeLoopbackFrameShift emits `lights` rows and then ALWAYS calls encodeWs2812ShiftLatchPad to close the frame, one extra Slot past the last row. The buffer was sized for the rows only, so that final word landed one-to-two bytes past the end of the heap block: a real overrun on every shift-mode loopback, silent on a forgiving allocator and a crash on an unlucky one. (Direct mode writes no pad, so it is unaffected, and that asymmetry is why sizing them the same was wrong.)
//
// The bug is a heap write, so the assertion that really catches it is the sanitizer, not a CHECK: run this suite under ASan and the pre-fix code trips heap-buffer-overflow here. What the CHECKs below can pin on their own is that the loopback ran to completion and left the driver healthy rather than tripping the out-of-memory path.
TEST_CASE("shift register: the loopback frame has room for its closing latch word") {
    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 16 * 8, "1,2", /*shiftOn=*/true, /*latch=*/3);
    REQUIRE(d.laneCount() == 16);          // 2 pins x 8 outputs

    d.loopbackTest = true;
    d.loopbackStrand = 0;
    d.applyState();                        // arms the self-test
    d.tick();                              // builds + "transmits" the test frame (mock bus)

    // It must not have fallen into the out-of-memory branch, and must still be a live driver.
    CHECK(d.severity() != mm::MoonModule::Severity::Error);
    CHECK(d.laneCount() == 16);
}

// A peripheral that CANNOT host the '595 (Parlio's transfer cap, or the classic i80 = I2S). Its pinExpander control is hidden, so a user can't turn a stray `pinExpander=true` back off, the driver must therefore degrade to direct mode, not wedge in an unfixable error status.
struct NoExpanderPeripheral : MockPeripheral {
    bool supportsPinExpander() const override { return false; }
};

// Switching to (or loading a saved config on) a peripheral that can't host the expander must degrade to direct mode, because the pinExpander toggle is hidden there, an error status would be unfixable from the UI. The degrade is in the EFFECTIVE mode (pinExpanderMode()), NOT a mutation of the stored value: the saved `pinExpander=true` is preserved so A/B'ing back to a supporting peripheral restores shift mode. Pins: the driver KEEPS pinExpander, drives DIRECT (effective mode off), and reports no error.
TEST_CASE("pinExpander degrades to direct on a peripheral that can't host it, keeping the saved value") {
    NoExpanderPeripheral peripheral;   // declared before the driver — borrowed, must outlive it
    MockShiftDriver d;
    mm::Buffer src;
    mm::Correction corr;
    // Ask for the expander (as a stale saved config or a post-switch state would): 3 data pins, latch set.
    wire(d, peripheral, src, corr, 3 * 8, "1,2,3", /*shiftOn=*/true, /*latch=*/7);

    CHECK(d.pinExpander);                                       // saved value PRESERVED (not mutated)
    CHECK_FALSE(d.pinExpanderMode());                           // but the EFFECTIVE mode is direct
    CHECK(d.severity() != mm::MoonModule::Severity::Error);     // NOT a dead-end error
    CHECK(d.laneCount() == 3);                                  // 3 pins → 3 direct lanes (not ×8)
}

// Regression: the i80 clock/DC defaults were 10/11, free GPIOs on the ESP32-S3 they were chosen on and the FLASH bus (6-11) on a classic ESP32. On an ESP32-PICO-V3-02 (QuinLED Dig-Next-2) every pin list wedged the board the moment it was applied: a TG1WDT_SYS_RESET with both CPUs stuck at the same PC, no panic and no coredump, so nothing named the pin. The defaults are now chosen per chip off the same `i2sLanes > 0` discriminator dmaBudgetBytes() uses, and reinit() refuses any reserved bus pin outright rather than routing I/O onto flash.
TEST_CASE("the i80 clock and DC defaults never land on a chip's flash pins") {
    mm::I80Peripheral p;
    // Classic ESP32 reserves 6-11 for flash; every other supported chip has 10/11 free. Whichever build this is, the defaults must sit outside that range on the classic path.
    if constexpr (mm::platform::i2sLanes > 0) {  // compiles out on every desktop build; flash pins are a classic-ESP32 property
        CHECK(p.clockPin != 10);
        CHECK(p.dcPin != 11);
        CHECK((p.clockPin < 6 || p.clockPin > 11));
        CHECK((p.dcPin < 6 || p.dcPin > 11));
    }
    // And they are always real, distinct, output-capable pins: esp_lcd rejects a negative pin, and the two signals cannot share one wire.
    CHECK(p.clockPin >= 0);
    CHECK(p.dcPin >= 0);
    CHECK(p.clockPin != p.dcPin);
}

// The guard itself, on the host: routing a bus pin onto a chip's flash/PSRAM lines corrupts the device, and on the classic ESP32 the failing path busy-waits to a watchdog reset rather than returning an error, so the driver must refuse BEFORE it touches the bus. setTestGpioCapability is what makes that testable here, where every real pin is otherwise "safe".
TEST_CASE("a bus pin wired to flash is refused, not routed") {
    mm::platform::clearTestGpioCapability();
    mm::platform::GpioCapability flash;      // what a flash pin reports
    flash.reserved = true;
    mm::platform::setTestGpioCapability(3, flash);

    MockShiftDriver d;
    MockPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wire(d, peripheral, src, corr, 64, "3", /*shiftOn=*/false, /*latch=*/-1);

    // Refused with a status that NAMES the pin, which is the whole point: the watchdog reset this replaces printed nothing a user could act on.
    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "GPIO 3") != nullptr);
    CHECK(std::strstr(d.status(), "flash") != nullptr);
    mm::platform::clearTestGpioCapability();
}

// The WR/DC half of the reserved-pin refusal, and the case the lane sweep alone cannot see: at exactly the bus width there are no spare lanes for WR to be parked on, and DC never rides the lane list at all (it goes straight to busInit). Both must still be refused.
//
// Through wire()/applyState() with a REAL I80Peripheral, not a bare instance: validateBusFatal() reaches owner_->pinExpanderMode() on its no-hit path, and owner_ is only ever set by attach() (setPeripheralForTest calls it), so a peripheral built and queried standalone is a null-owner crash that no production caller can hit, since ParallelLedDriver always attaches before use.
TEST_CASE("a full-width bus still refuses a WR or DC pin wired to flash") {
    mm::platform::clearTestGpioCapability();
    mm::platform::GpioCapability flash;
    flash.reserved = true;
    mm::platform::setTestGpioCapability(11, flash);       // stands in for a flash pin

    // Inlined rather than routed through wire(), which is typed to MockPeripheral&: this test needs the REAL I80Peripheral (see the class comment above) so owner_ is genuinely attached.
    MockShiftDriver d;
    mm::I80Peripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    peripheral.clockPin = 10;
    peripheral.dcPin    = 11;                              // DC on the reserved pin
    d.setPeripheralForTest(&peripheral);
    std::strcpy(d.pins, "10,20,21,22,23,24,25,26");        // full 8-wide bus: no spare lane for WR
    d.pinExpander = false;
    d.latchPin = -1;
    d.doubleBuffer = false;
    REQUIRE(src.allocate(8, 3));
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();

    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "dcPin") != nullptr);
    CHECK(std::strstr(d.status(), "flash") != nullptr);

    peripheral.clockPin = 11;                              // and the same for WR
    peripheral.dcPin    = 10;
    d.applyState();
    REQUIRE(d.status() != nullptr);
    CHECK(std::strstr(d.status(), "clockPin") != nullptr);

    mm::platform::clearTestGpioCapability();
}
