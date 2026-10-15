/// The peripheral half of the parallel driver on the Teensy 4.1.
///
/// QTimer4 paces one eDMA channel through the crossbar, and each request writes the next slot of the frame, widened to a port word, into `GPIO1_DR`.

#include "platform/platform.h"
#include "platform/loopback_verify.h"   // the bit-verify every loopback shares

#include <Arduino.h>
#include <DMAChannel.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm::platform {

extern "C" void xbar_connect(unsigned int input, unsigned int output);   // PJRC's pwm.c

namespace {
// The timer counts the IPG bus clock, F_BUS_ACTUAL, which is 150 MHz at the core's 600 MHz.
constexpr uint32_t kBusHz = 150'000'000;
// 150 MHz / 56 = 2.68 MHz: a 373 ns slot and a 1.12 us WS2812 bit, inside the part's window.
constexpr uint16_t kSlotTicks = 56;
constexpr uint32_t kSlotHz = kBusHz / kSlotTicks;
// eDMA's major loop count is 15 bits, so a longer frame runs as a chain of descriptors.
constexpr size_t kSlotsPerTcd = 32767;
constexpr size_t kMaxTcds = (gpioDmaMaxSlots + kSlotsPerTcd - 1) / kSlotsPerTcd;

// One bus at a time: the timer channel and the crossbar DMA request it uses are single.
struct GpioDmaBus {
    uint8_t* encode = nullptr;    // the driver's 8- or 16-bit slots
    uint32_t* wire = nullptr;     // the same slots as GPIO1_DR words, which the DMA reads
    size_t capacity = 0;
    uint8_t slotBytes = 1;
    uint32_t laneMask = 0;        // the port bits the lanes own
    uint32_t lut[2][256] = {};    // slot byte -> port bits, per byte of the slot
    DMAChannel dma;
    uint32_t startCycles = 0;
    uint32_t lastUs = 0;
    bool busy = false;
};
GpioDmaBus* g_gpioDmaBus = nullptr;
DMASetting g_gpioDmaChain[kMaxTcds];   // in DTCM, which the eDMA reads uncached

void gpioDmaTimerStop() { TMR4_ENBL &= ~1; }

// The timer's output toggles at every compare and the crossbar turns each edge into one DMA request.
void gpioDmaTimerSetup() {
    CCM_CCGR6 |= CCM_CCGR6_QTIMER4(CCM_CCGR_ON);
    CCM_CCGR2 |= CCM_CCGR2_XBAR1(CCM_CCGR_ON);
    gpioDmaTimerStop();
    TMR4_SCTRL0 = TMR_SCTRL_OEN | TMR_SCTRL_FORCE | TMR_SCTRL_MSTR;
    TMR4_CNTR0 = 0;
    TMR4_LOAD0 = 0;
    TMR4_COMP10 = kSlotTicks - 1;
    TMR4_CMPLD10 = kSlotTicks - 1;
    TMR4_CTRL0 = TMR_CTRL_CM(1) | TMR_CTRL_PCS(8) | TMR_CTRL_LENGTH | TMR_CTRL_OUTMODE(3);
    xbar_connect(XBARA1_IN_QTIMER4_TIMER0, XBARA1_OUT_DMA_CH_MUX_REQ30);
    // A level request would stay asserted, so both edges are detected; STS0 is write-one-to-clear.
    uint16_t ctrl = XBARA1_CTRL0 & ~(XBARA_CTRL_EDGE0(3) | XBARA_CTRL_DEN0 | XBARA_CTRL_STS0 | XBARA_CTRL_STS1);
    XBARA1_CTRL0 = ctrl | XBARA_CTRL_EDGE0(3) | XBARA_CTRL_DEN0 | XBARA_CTRL_STS0;
}

// The port ignores 8- and 16-bit DMA writes, measured on the bench, so each slot widens to a port word here.
void gpioDmaWiden(GpioDmaBus& b, size_t slots) {
    if (b.slotBytes == 1) {
        for (size_t i = 0; i < slots; i++) b.wire[i] = b.lut[0][b.encode[i]];
    } else {
        const auto* src = reinterpret_cast<const uint16_t*>(b.encode);
        for (size_t i = 0; i < slots; i++) b.wire[i] = b.lut[0][src[i] & 0xFF] | b.lut[1][src[i] >> 8];
    }
}

// `startTimer` false leaves the timer for the caller, so a capture can start it in the same instant it starts listening.
bool gpioDmaStart(GpioDmaBus& b, size_t bytes, bool startTimer = true) {
    const size_t slots = bytes / b.slotBytes;
    const size_t tcds = (slots + kSlotsPerTcd - 1) / kSlotsPerTcd;
    if (slots == 0 || tcds > kMaxTcds || bytes > b.capacity) return false;
    gpioDmaWiden(b, slots);
    arm_dcache_flush(b.wire, slots * sizeof(uint32_t));
    gpioDmaTimerStop();
    XBARA1_CTRL0 |= XBARA_CTRL_STS0;   // drop a request left from the last frame
    for (size_t t = 0; t < tcds; t++) {
        const size_t first = t * kSlotsPerTcd;
        const size_t count = (slots - first < kSlotsPerTcd) ? slots - first : kSlotsPerTcd;
        DMASetting& d = g_gpioDmaChain[t];
        // The completion flags are only ever ORed in, so a descriptor starts clean or keeps the last frame's.
        d.TCD->CSR = 0;
        d.sourceBuffer(b.wire + first, count * sizeof(uint32_t));
        d.destination(GPIO1_DR);
        if (t + 1 < tcds) d.replaceSettingsOnCompletion(g_gpioDmaChain[t + 1]);
        else d.disableOnCompletion();
    }
    b.dma = g_gpioDmaChain[0];
    b.dma.triggerAtHardwareEvent(DMAMUX_SOURCE_XBAR1_0);
    b.dma.clearComplete();
    b.dma.enable();
    TMR4_CNTR0 = 0;
    asm volatile("dsb" ::: "memory");
    b.startCycles = ARM_DWT_CYCCNT;
    // The timer paces every slot, so the wire time is the slot count; a stamp at the finish would add the render loop's wait.
    b.lastUs = static_cast<uint32_t>(slots * uint64_t{kSlotTicks} * 1'000'000 / kBusHz);
    if (startTimer) TMR4_ENBL |= 1;
    b.busy = true;
    return true;
}

void gpioDmaFinish(GpioDmaBus& b) {
    gpioDmaTimerStop();
    b.dma.disable();
    b.dma.clearComplete();
    b.busy = false;
}
}  // namespace

namespace {
// Each lane's port bit; false for no lanes, too many, or a pin off GPIO6, the one port the DMA reaches through its alias.
bool gpioDmaLaneMasks(const uint16_t* dataPins, uint8_t laneCount, uint32_t* masks) {
    if (!dataPins || laneCount == 0 || laneCount > gpioDmaLanes) return false;
    for (uint8_t i = 0; i < laneCount; i++) {
        const uint8_t pin = static_cast<uint8_t>(dataPins[i]);
        if (pin >= CORE_NUM_DIGITAL || portOutputRegister(pin) != &GPIO6_DR) return false;
        masks[i] = digitalPinToBitMask(pin);
    }
    return true;
}

// The bus with its two buffers, or null when either does not fit.
GpioDmaBus* gpioDmaBusNew(size_t bufferBytes, uint8_t slotBytes) {
    auto* b = new (std::nothrow) GpioDmaBus();
    if (!b) return nullptr;
    b->slotBytes = slotBytes;
    b->capacity = bufferBytes;
    const size_t wireBytes = bufferBytes / slotBytes * sizeof(uint32_t);
    b->encode = static_cast<uint8_t*>(aligned_alloc(32, (bufferBytes + 31) & ~size_t{31}));
    b->wire = static_cast<uint32_t*>(aligned_alloc(32, (wireBytes + 31) & ~size_t{31}));
    if (b->encode && b->wire) {
        std::memset(b->encode, 0, bufferBytes);
        return b;
    }
    std::free(b->encode);
    std::free(b->wire);
    delete b;
    return nullptr;
}

// Each slot byte's set bits become the port bits of their lanes: the low byte holds lanes 0 to 7, the high byte 8 to 15.
void gpioDmaBuildLut(GpioDmaBus& b, const uint32_t* masks, uint8_t laneCount) {
    for (uint8_t i = 0; i < laneCount; i++) b.laneMask |= masks[i];
    for (uint16_t v = 0; v < 256; v++)
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (!(v & (1u << bit))) continue;
            if (bit < laneCount) b.lut[0][v] |= masks[bit];
            if (8 + bit < laneCount) b.lut[1][v] |= masks[8 + bit];
        }
}
}  // namespace

bool gpioDmaWs2812Init(GpioDmaWs2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       size_t bufferBytes) {
    uint32_t masks[16] = {};
    if (h.impl || g_gpioDmaBus || bufferBytes == 0 || !gpioDmaLaneMasks(dataPins, laneCount, masks)) return false;
    GpioDmaBus* b = gpioDmaBusNew(bufferBytes, laneCount > 8 ? 2 : 1);
    if (!b) return false;
    gpioDmaBuildLut(*b, masks, laneCount);
    // The lanes leave the fast GPIO6 for its GPIO1 alias, the one the eDMA reaches; every other pin ignores the DMA's writes.
    for (uint8_t i = 0; i < laneCount; i++) ::pinMode(dataPins[i], OUTPUT);
    GPIO1_DR_CLEAR = b->laneMask;
    GPIO1_GDIR |= b->laneMask;
    IOMUXC_GPR_GPR26 &= ~b->laneMask;
    gpioDmaTimerSetup();
    g_gpioDmaBus = b;
    h.impl = b;
    return true;
}

uint8_t* gpioDmaWs2812Buffer(const GpioDmaWs2812Handle& h) {
    return h.impl ? static_cast<GpioDmaBus*>(h.impl)->encode : nullptr;
}

size_t gpioDmaWs2812BufferCapacity(const GpioDmaWs2812Handle& h) {
    return h.impl ? static_cast<GpioDmaBus*>(h.impl)->capacity : 0;
}

size_t gpioDmaWs2812HeapBytes(const GpioDmaWs2812Handle& h) {
    const auto* b = static_cast<const GpioDmaBus*>(h.impl);
    return b ? b->capacity + b->capacity / b->slotBytes * sizeof(uint32_t) : 0;
}

namespace {
// Block until the transfer finishes, letting the network run meanwhile; false on timeout.
bool gpioDmaAwait(GpioDmaBus& b, uint32_t timeoutMs) {
    if (!b.busy) return true;
    const uint32_t start = ::millis();
    while (!b.dma.complete()) {
        if (::millis() - start >= timeoutMs) { gpioDmaFinish(b); return false; }
        ::yield();
    }
    gpioDmaFinish(b);
    return true;
}
}  // namespace

// The previous frame still clocking out holds the wire copy, so this one waits for it here, after the encode it overlapped.
bool gpioDmaWs2812Transmit(GpioDmaWs2812Handle& h, size_t bytes, uint32_t waitMs) {
    auto* b = static_cast<GpioDmaBus*>(h.impl);
    if (!b || !gpioDmaAwait(*b, waitMs)) return false;
    return gpioDmaStart(*b, bytes);
}

uint32_t gpioDmaWs2812LastTransmitUs(const GpioDmaWs2812Handle& h) {
    return h.impl ? static_cast<GpioDmaBus*>(h.impl)->lastUs : 0;
}

void gpioDmaWs2812Deinit(GpioDmaWs2812Handle& h) {
    auto* b = static_cast<GpioDmaBus*>(h.impl);
    if (!b) return;
    if (b->busy) gpioDmaFinish(*b);
    IOMUXC_GPR_GPR26 |= b->laneMask;   // back to the fast GPIO6
    // Released pins listen rather than drive, so a jumpered or rewired neighbor never fights them.
    for (uint8_t pin = 0; pin < CORE_NUM_DIGITAL; pin++)
        if (portOutputRegister(pin) == &GPIO6_DR && (b->laneMask & digitalPinToBitMask(pin))) ::pinMode(pin, INPUT_PULLDOWN);
    std::free(b->encode);
    std::free(b->wire);
    delete b;
    g_gpioDmaBus = nullptr;
    h.impl = nullptr;
}

// The live bus is the only transmitter and no free-running capture rides beside it, so the self-test builds its own bus instead.
RmtLoopbackResult ws2812LoopbackRide(uint16_t, const uint8_t*, uint8_t, size_t, uint8_t, uint8_t) { return {}; }

namespace {
// A low longer than this ends the frame: the trailing zero slots are the 324 us latch.
constexpr uint32_t kLatchGapUs = 50;

// From RAM and touching only the pin and the cycle counter, so no pass outlasts a 373 ns slot.
__attribute__((section(".fastrun"), noinline))
size_t captureHighCycles(const volatile uint32_t* psr, uint32_t mask, uint32_t* out, size_t max,
                         uint32_t timeoutCycles, uint32_t gapCycles) {
    TMR4_ENBL |= 1;   // the transfer starts here, as the capture does
    const uint32_t t0 = ARM_DWT_CYCCNT;
    uint32_t riseAt = t0, fallAt = t0;
    bool prev = false;
    size_t got = 0;
    while (got < max) {
        const bool level = (*psr & mask) != 0;
        const uint32_t now = ARM_DWT_CYCCNT;
        if (level != prev) {
            if (level) riseAt = now;
            else { out[got++] = now - riseAt; fallAt = now; }
            prev = level;
        } else if (!level && got > 0 && now - fallAt > gapCycles) {
            break;
        }
        if (now - t0 > timeoutCycles) break;
    }
    return got;
}

// High times in 40 MHz ticks, the resolution the shared verify's threshold is written in.
constexpr uint32_t kCapHz = 40'000'000;
constexpr uint16_t kSlotCapTicks = static_cast<uint16_t>(kCapHz / kSlotHz);
constexpr uint16_t kThreshTicks = static_cast<uint16_t>(kSlotCapTicks + kSlotCapTicks / 2);

// A driven level that reaches the receive pin both ways is a jumper rather than a floating input.
bool loopbackJumperOk(uint8_t tx, uint8_t rx) {
    ::pinMode(tx, OUTPUT);
    ::pinMode(rx, INPUT_PULLDOWN);
    ::digitalWrite(tx, HIGH);
    ::delay(2);
    const int hi = ::digitalRead(rx);
    ::digitalWrite(tx, LOW);
    ::delay(2);
    const int lo = ::digitalRead(rx);
    std::printf("loopback: continuity tx=%u -> rx=%u: hi=%d lo=%d (want hi=1 lo=0)\n", tx, rx, hi, lo);
    return hi == 1 && lo == 0;
}

// Interrupts stay off for the test frame, about 7 ms, so the edge timestamps carry no gaps.
size_t loopbackCapture(GpioDmaBus& b, size_t frameBytes, uint8_t rx, uint32_t* symbols, size_t max,
                       RmtLoopbackResult& r) {
    const uint32_t cyclesPerUs = F_CPU_ACTUAL / 1000000;
    const uint32_t timeoutCycles = (r.txExpectUs + 2000) * cyclesPerUs;
    __disable_irq();
    gpioDmaStart(b, frameBytes, false);
    const size_t got = captureHighCycles(portInputRegister(rx), digitalPinToBitMask(rx), symbols, max,
                                         timeoutCycles, kLatchGapUs * cyclesPerUs);
    const uint32_t t0 = b.startCycles;
    while (!b.dma.complete() && ARM_DWT_CYCCNT - t0 < timeoutCycles) {}
    // Measured rather than counted, so a stalled transfer shows against txExpectUs.
    r.txWallUs = (ARM_DWT_CYCCNT - t0) / cyclesPerUs;
    gpioDmaFinish(b);
    __enable_irq();
    for (size_t i = 0; i < got; i++) symbols[i] /= F_CPU_ACTUAL / kCapHz;
    return got;
}

void loopbackVerify(const uint32_t* symbols, size_t bits, size_t got, uint8_t rowBits, RmtLoopbackResult& r) {
    const detail::LoopbackBitStats st = detail::verifyLoopbackBits(symbols, bits, got, rowBits, kThreshTicks, false, r);
    std::printf("loopback: high ticks 0-bits %u..%u, 1-bits %u..%u (25 ns/tick, threshold %u), %s\n",
                st.minHigh[0], st.maxHigh[0], st.minHigh[1], st.maxHigh[1], kThreshTicks, r.pass ? "PASS" : "FAIL");
    if (!r.pass)
        std::printf("loopback: first bad bit %u (light %u), %u/%u bits bad; got %02x%02x%02x\n",
                    static_cast<unsigned>(st.mismatch), static_cast<unsigned>(st.mismatch / rowBits),
                    static_cast<unsigned>(st.mismatchCount), static_cast<unsigned>(bits), r.got[0], r.got[1], r.got[2]);
}
}  // namespace

RmtLoopbackResult gpioDmaWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t rxGpio, const uint8_t* frame,
                                        size_t frameBytes, size_t dataBytes, uint8_t rowBits) {
    RmtLoopbackResult r = detail::loopbackResult();
    if (!detail::loopbackFrameValid(dataPins, laneCount, frame, frameBytes, dataBytes, rowBits) || rxGpio >= CORE_NUM_DIGITAL)
        return r;
    const uint8_t tx = static_cast<uint8_t>(dataPins[0]), rx = static_cast<uint8_t>(rxGpio);
    // Only a lane pin transmits, and never the receive pin: two outputs on one jumper fight each other.
    if (tx >= CORE_NUM_DIGITAL || tx == rx || portOutputRegister(tx) != &GPIO6_DR) {
        std::printf("loopback: transmit pin %u is not a lane pin (0, 1, 14-27, 38-41)\n", tx);
        return r;
    }
    r.jumperDetected = loopbackJumperOk(tx, rx);
    if (!r.jumperDetected) return r;

    GpioDmaWs2812Handle h;
    if (!gpioDmaWs2812Init(h, dataPins, 1, frameBytes)) { std::printf("loopback: bus init failed\n"); return r; }
    std::memcpy(gpioDmaWs2812Buffer(h), frame, frameBytes);
    const size_t bits = dataBytes / 3;
    auto* symbols = static_cast<uint32_t*>(std::malloc((bits + 16) * sizeof(uint32_t)));
    if (!symbols) { gpioDmaWs2812Deinit(h); return r; }

    r.txExpectUs = static_cast<uint32_t>(bits * 3ull * 1000000ull / kSlotHz);
    const size_t got = loopbackCapture(*static_cast<GpioDmaBus*>(h.impl), frameBytes, rx, symbols, bits + 16, r);
    r.capturedSymbols = static_cast<uint32_t>(got);
    r.rxIdleLevel = static_cast<int8_t>(::digitalRead(rx));
    std::printf("loopback: %u bytes in %lu us (expect ~%lu us), captured %u pulses (need %u), idle rx=%d\n",
                static_cast<unsigned>(frameBytes), static_cast<unsigned long>(r.txWallUs),
                static_cast<unsigned long>(r.txExpectUs), static_cast<unsigned>(got),
                static_cast<unsigned>(bits), r.rxIdleLevel);
    if (got >= bits) loopbackVerify(symbols, bits, got, rowBits, r);
    std::free(symbols);
    gpioDmaWs2812Deinit(h);
    return r;
}

}  // namespace mm::platform
