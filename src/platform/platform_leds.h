#pragma once
/// The platform's LED peripherals: PWM, RMT, the I80 and parallel buses with their streaming ring, GPIO DMA and HUB75.
///
/// One concern of the platform interface ([platform.h](platform.h) gathers them all), so a file includes the concerns it uses and an edit to one rebuilds its users alone.
///
/// @technical
///
/// ## DMA cannot read PSRAM at the expander's clock
///
/// Measured fine at 2.67 MHz and never completing at 26.67 MHz, so a frame above the expander's cap is never materialized; the WS2812 waveform keeps the clock up, and its DMA demand meets PSRAM's contended rate.
///
/// The streaming ring loops a small pool of internal buffers instead, and the CPU encodes the next slice into each as it drains.
///

#include "platform/platform_base.h"   // target flags, MM_NONBLOCKING and the compiler macros

namespace mm::platform {


// PWM output for lights whose channels are each a pin: the chip's LEDC peripheral, one timer per driver.

/// How many PWM channels the chip has across its speed modes; 0 where there is no PWM output.
uint8_t pwmChannelCount() MM_NONBLOCKING;

/// The 80 MHz a PWM timer counts on every built chip (APB, or PLL_F80M on the P4 and S31), so `bits` duty bits pulse at kPwmClockHz >> bits.
constexpr uint32_t kPwmClockHz = 80'000'000;

/// The most duty bits a PWM timer holds; 0 where there is no PWM output.
uint8_t pwmMaxBits() MM_NONBLOCKING;

/// Start a PWM timer with `bits` duty bits, pulsing at kPwmClockHz >> bits; the timer, or -1 when none is free or the chip refuses that resolution.
int pwmStart(uint8_t bits);

/// Drive `pin` from `timer`, its pulse starting `phase` steps into the period; the channel, or -1 when none is free.
int pwmAttach(int timer, uint8_t pin, uint32_t phase);

/// Set a channel's duty in steps of the timer's resolution, a register write that does not block.
void pwmWrite(int channel, uint32_t duty) MM_NONBLOCKING;

/// Release `timer` and every channel attached to it, their pins driven low.
void pwmStop(int timer);

/// The duty last written to `channel`, for the host tests; 0 on a device.
uint32_t pwmDutyForTest(int channel);

/// The pin `channel` drives, for the host tests; -1 when the channel is free.
int pwmPinForTest(int channel);

/// The phase `channel` was attached with, for the host tests; 0 on a device.
uint32_t pwmPhaseForTest(int channel);

// RMT WS2812 output: the driver encodes symbols, the platform owns only the peripheral.

/// One configured RMT TX channel; the driver never inspects `impl`.
struct RmtWs2812Handle { void* impl = nullptr; };

/// Configure one RMT TX channel on `gpio`, where `invert` suits an inverting level-shifter.
bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert);

/// The tick resolution granted, which the driver converts its timings against; 0 before init.
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING;

/// Transmit one frame as the channel-ordered wire bytes the strip expects.
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint8_t* wire, size_t byteCount);

/// Set the symbols a 0 and a 1 bit expand to; the timing control rewrites these between frames.
bool rmtWs2812SetBitTiming(RmtWs2812Handle& h, uint32_t sym0, uint32_t sym1);

/// Block until this channel's transmit completes; false on timeout, the frame still clocking out.
bool rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs);

/// Release the channel.
void rmtWs2812Deinit(RmtWs2812Handle& h);

/// Capture up to `maxSymbols` pulse durations on `gpio`, for the loopback self-test alone.
size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs);


/// What a loopback self-test observed.
///
/// @technical The verdict says why it failed rather than only that it did: an empty capture is a different fault from a full one that decodes wrong.
/// Without those numbers both collapse into the same "bad bit 0 of 0" and isolate nothing.
struct RmtLoopbackResult {
    bool jumperDetected = false;  ///< the plain-GPIO continuity pre-check passed
    bool pass = false;            ///< every captured bit matched what was sent
    uint8_t sent[3] = {};         ///< the per-light test pattern transmitted
    uint8_t got[3] = {};          ///< the light holding the first mismatch, light 0 when clean
    uint32_t bitsChecked = 0;     ///< WS2812 bits verified, 24 for the short test
    uint32_t firstBadBit = 0;     ///< the first wrong bit, or bitsChecked when all pass
    uint32_t capturedSymbols = 0; ///< symbols captured, which should reach bitsChecked
    int8_t rxIdleLevel = -1;      ///< RX level after the capture window, -1 when unknown
    uint32_t txWallUs = 0;        ///< wall time of the first timed transmit
    uint32_t txExpectUs = 0;      ///< expected wire time; far below the wall time means a stalled transfer
};
/// Drive a known pattern out `txGpio` and verify it on `rxGpio`, which the user jumpers together.
RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio);

/// The same test over a whole real frame, clocked back to back as the render loop does.
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t txGpio, uint8_t rxGpio,
                                         uint16_t lights, uint8_t channels);

// i80 parallel WS2812 output: one pre-encoded frame in a DMA buffer the platform keeps internal: @xref{dma-cannot-read-psram-at-the-expanders-clock}.

/// One configured i80 bus with its one or two DMA frame buffers.
struct I80Ws2812Handle { void* impl = nullptr; };

/// Stands for a line this board does not wire.
constexpr uint16_t kBusPinUnset = 0xFFFF;
/// Create the bus on `dataPins`, plus the `wrGpio` pixel clock and `dcGpio` that WS2812 strands ignore.
bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer, uint8_t clockMultiplier = 1);
/// Why the last init failed when the backend knows more than that it did; null when it did not.
const char* i80Ws2812LastError();
/// Whether the peripheral this backend shares is free to claim right now.
bool i80Ws2812SharedBusFree();

/// The DMA frame buffer the driver encodes into; buffer 1 is null where it did not fit.
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer);
/// The capacity both buffers share, which is the driver's grow-only check; 0 before init.
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h);

/// Start the autonomous transfer of one buffer and return; pair it with a wait on that same buffer.
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes);

/// Block until that buffer's transfer finishes; false on timeout, the DMA perhaps still reading it.
bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

/// How long the last completed transfer took, which is the pure wire time; 0 before the first.
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h);

/// Release the bus and its buffers.
void i80Ws2812Deinit(I80Ws2812Handle& h);

/// Transmit the caller's real frame on a private bus and verify every captured bit.
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits,
                                    uint8_t clockMultiplier = 1);

// MoonI80: our own DMA driver, because esp_lcd re-arms per transaction and caps a frame at one block.

/// One configured MoonI80 bus, in whole-frame or ring mode.
struct MoonI80Ws2812Handle { void* impl = nullptr; };

/// Fill one drained ring buffer: the platform owns the ring, the domain owns the encode.
using MoonI80EncodeFn = void (*)(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                 bool closeFrame, bool needsPrefill);

/// Bring the bus up in whole-frame mode, on i80Ws2812Init's contract.
bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier = 1);

/// Lights per DMA buffer, wall-verified at 256 lights across 16 strands.
constexpr uint8_t kRingRowsDefault = 7;
/// Buffers the DMA circulates, sized to the measured pool knee.
constexpr uint8_t kRingBufsDefault = 16;
/// The per-slice zero-pad ceiling in µs, which stretches the refill deadline without ending the frame.
constexpr uint8_t kRingPadMaxUs = 120;
/// The most bytes one GDMA descriptor node carries.
constexpr size_t kRingNodeMaxBytes = 4095;
/// Buffers the refill keeps clear behind the drain, so it never writes one the DMA may still read.
constexpr uint32_t kRingRefillGap = 2;
/// The ring's lead over the wire, in rows, that ringAuto sizes to. On a 3,840-light expander rig 40 rows lost frames and 45 ran clean, so 45 plus a fifth.
constexpr uint32_t kRingLeadRows = 54;
/// Rows per buffer under ringAuto: finer slices waste less of the lead to the refill's whole-slice steps, and one-row slices interrupt faster than the refill keeps up.
constexpr uint8_t kRingAutoRows = 3;
/// The deepest ring pool, which keeps every slice encoded before arming even at 48 by 256.
constexpr uint8_t kRingBufsMax = 64;
/// The shallowest ring pool.
constexpr uint8_t kRingBufsMin = 2;

/// Bring the bus up in ring mode, streaming a frame too big to hold; false falls back to whole-frame.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                           uint16_t wrGpio, size_t rowBytes, uint32_t totalRows,
                           uint32_t rowsPerBuf, uint8_t ringBufs, uint8_t padUs,
                           uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user);

/// Start one frame on the ring: prime the buffers, fire the DMA, and let the refill run behind it.
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& h);
/// Set the '595 shift-clock prescale off the 80 MHz bus, taking effect on the next bus build.
void moonI80SetShiftClockDiv(uint8_t div);

/// Whether this handle came up as a ring, which tells the driver which transmit to call.
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& h);

/// Would a whole frame of `bytes` fit internal DMA RAM now, leaving the network reserve?
bool moonI80Ws2812InternalFits(size_t bytes);
/// The DMA frame buffer the driver encodes into.
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer);
/// The capacity both buffers share; 0 before init.
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h);
/// Every byte of DMA memory the bus holds: its frame buffers, or its ring and pad; 0 before init.
size_t moonI80Ws2812HeapBytes(const MoonI80Ws2812Handle& h);
/// Start the autonomous transfer of one whole-frame buffer.
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes);
/// Block until that buffer's transfer finishes; false on timeout, as i80Ws2812Wait describes.
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);
/// How long the last completed transfer took; 0 before the first.
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h);

/// What the ring has been doing, for a read-only control; every field is 0 on a whole-frame handle.
///
/// @technical Best-effort volatile reads without a lock, so this is a diagnostic rather than a contract.
struct MoonI80RingStats {
    bool     isRing = false;     ///< whether this handle runs a ring
    uint32_t nSlices = 0;        ///< slices a frame takes, the light count over rowsPerBuf
    uint32_t ringBufs = 0;       ///< pool size; buffers are reused once nSlices passes it
    uint32_t eofTotal = 0;       ///< lifetime end-of-frame interrupts
    uint32_t doneGiven = 0;      ///< lifetime frame completions
    uint32_t lastDrain = 0;      ///< the drain count the last interrupt saw, which should reach nSlices
    uint32_t numItems = 0;       ///< descriptor pool capacity
    uint32_t consumedItems = 0;  ///< nodes the mount loop used, which equals numItems when sized right
    uint32_t descErr = 0;        ///< descriptor errors, where anything above 0 means the chain was corrupted
    uint32_t maxEncodeUs = 0;    ///< worst refill-encode time, the producer's jitter
    uint32_t avgEncodeUs = 0;    ///< average refill-encode time, which decides whether the ring keeps up
    uint32_t maxIsrGapUs = 0;    ///< worst gap between interrupts within a frame, which is the drain deadline
    int32_t  minLead = -1;       ///< fewest slices the refill was ahead of the drain when an interrupt came, -1 before any
    uint32_t maxGapAtSlice = 0;  ///< the drain position where the worst gap ended, telling a frame-start stall from a random one
    uint8_t  isrCore = 0;        ///< the core the refill interrupt runs on
    uint32_t late = 0;           ///< slices refilled after their drain began, each one stale on the wire
    uint32_t itemsPerBuf = 0;    ///< descriptor nodes a ring buffer takes, 1 since the clamp
    int32_t  termNodeDiag = -1;  ///< the mount-time terminator node, -1 on a lapping chain
    uint32_t cacheOffDefers = 0; ///< interrupts that refilled nothing because the flash cache was off
    uint32_t cacheOffMaxRun = 0; ///< worst run of those, which is how many buffers drained un-refilled
    uint32_t stallAbandons = 0;  ///< frames the wait backstop finalized after the DMA self-terminated
};
/// Read the ring's counters; the worst gap and the fewest lead cover the time since the previous read.
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& h);

/// Release the bus and its buffers.
void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h);
/// Verify a real frame on a private bus, optionally riding the ring the render path uses.
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier = 1,
                                        uint32_t ringRows = 0, uint32_t ringBufs = 0,
                                        bool useRing = false);

/// Verify what the live pipeline is already clocking, building no bus and leaving it running.
RmtLoopbackResult ws2812LoopbackRide(uint16_t rxGpio, const uint8_t* sent, uint8_t sentLen,
                                     size_t dataBytes, uint8_t rowBits, uint8_t clockMultiplier);

// Parlio WS2812 output: the data GPIOs directly, any lane count, and the clock generated internally.

/// One configured Parlio TX unit with its one or two DMA frame buffers.
struct ParlioWs2812Handle { void* impl = nullptr; };

/// Create a Parlio TX unit on `dataPins` clocked at the WS2812 slot rate.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes,
                      bool wantSecondBuffer);

/// The DMA frame buffer the driver encodes into; buffer 1 is null where it did not fit.
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer);
/// The capacity both buffers share; 0 before init.
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h);

/// The most bytes Parlio sends in one transfer: a hardware ceiling, so it needs no handle.
size_t parlioMaxTransferBytes();

/// Start the autonomous transfer of one buffer; pair it with a wait on that same buffer.
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes);

/// Block until that buffer's transfer finishes; false on timeout, as i80Ws2812Wait describes.
bool parlioWs2812Wait(ParlioWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

/// How long the last completed transfer took, the pure wire time; 0 before the first.
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& h);

/// Release the unit and its buffers.
void parlioWs2812Deinit(ParlioWs2812Handle& h);

/// Verify a real frame on a private Parlio unit, on i80Ws2812Loopback's contract.
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits);

// GPIO-DMA WS2812 output: one DMA channel writes each slot into a GPIO port's data register, paced by a timer, so any pins of that port are lanes.

/// One configured GPIO-DMA bus with its frame buffer.
struct GpioDmaWs2812Handle { void* impl = nullptr; };

/// Claim `dataPins` as lanes, in order, and size the frame buffer the driver encodes 8-bit slots into (16-bit above 8 lanes).
bool gpioDmaWs2812Init(GpioDmaWs2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       size_t bufferBytes);
/// The frame buffer the driver encodes into; null before init.
uint8_t* gpioDmaWs2812Buffer(const GpioDmaWs2812Handle& h);
/// The frame buffer's capacity; 0 before init.
size_t gpioDmaWs2812BufferCapacity(const GpioDmaWs2812Handle& h);
/// Every byte the bus holds, the frame buffer and any copy the transfer reads; 0 before init.
size_t gpioDmaWs2812HeapBytes(const GpioDmaWs2812Handle& h);
/// Wait up to `waitMs` for the previous frame to leave the wire, then start sending the first `bytes` of the frame buffer.
bool gpioDmaWs2812Transmit(GpioDmaWs2812Handle& h, size_t bytes, uint32_t waitMs);
/// How long the last completed transfer took, the pure wire time; 0 before the first.
uint32_t gpioDmaWs2812LastTransmitUs(const GpioDmaWs2812Handle& h);
/// Release the bus, returning its pins to plain GPIO.
void gpioDmaWs2812Deinit(GpioDmaWs2812Handle& h);
/// Verify a real frame of 8-bit slots on a private bus, on i80Ws2812Loopback's contract.
RmtLoopbackResult gpioDmaWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t rxGpio, const uint8_t* frame,
                                        size_t frameBytes, size_t dataBytes, uint8_t rowBits);

// HUB75 panel output: a panel is scanned rather than addressed, one bit plane at a time.

/// The pins one HUB75 port needs; `e` serves 1/32-scan panels alone.
///
/// @technical Every pin defaults to unset because a soldered line must never be guessed.
/// A default would pick the user's wiring, and on an S3 could land on PSRAM or a strapping pin.
struct Hub75Pins {
    uint16_t r1 = 0xFFFF, g1 = 0xFFFF, b1 = 0xFFFF;   ///< upper half-panel color
    uint16_t r2 = 0xFFFF, g2 = 0xFFFF, b2 = 0xFFFF;   ///< lower half-panel color
    uint16_t a = 0xFFFF, b = 0xFFFF, c = 0xFFFF;      ///< row address, 1/8 scan
    uint16_t d = 0xFFFF;                              ///< and 1/16 scan
    uint16_t e = 0xFFFF;                              ///< and 1/32 scan
    uint16_t clk = 0xFFFF, lat = 0xFFFF, oe = 0xFFFF; ///< shift clock, latch, blank
    /// The panel's shift registers sample on the falling clock edge rather than the rising one. Some chips do, and driven on the wrong edge every pixel lands one column over.
    bool clkFalling = false;
};

/// One running HUB75 port.
struct Hub75Handle { void* impl = nullptr; };

/// What drives a HUB75 port: an ESP32's LCD_CAM or Parlio block clocking bus words, a Pi 5's RP1 PIO through the `hub75Pio*` seam, or a Pi 3/4's GPIO through the `hub75Gpio*` seam.
enum class Hub75Backend : uint8_t { LcdCam = 0, Parlio = 1, Pio = 2, Gpio = 3 };

/// Is this backend present on this silicon and able to carry a frame of `frameBytes`?
bool hub75BackendAvailable(Hub75Backend backend, size_t frameBytes);

/// The label this backend shows in the UI, the same on every build so a saved choice finds it.
inline const char* hub75BackendLabel(Hub75Backend backend) {
    return backend == Hub75Backend::Gpio ? "Pi GPIO" : backend == Hub75Backend::Pio ? "Pi PIO" : backend == Hub75Backend::Parlio ? "Parlio" : "LCD_CAM";
}

/// Bring up a HUB75 port on `backend` and allocate its frame buffer.
bool hub75Init(Hub75Handle& h, Hub75Backend backend, const Hub75Pins& pins,
               uint16_t width, uint16_t height, uint8_t scanRate, uint8_t bitDepth);

/// Why the last hub75Init failed, or null when it did not.
const char* hub75LastError();

/// The DMA frame buffer the driver encodes into; null before a successful init.
uint8_t* hub75Buffer(const Hub75Handle& h) MM_NONBLOCKING;

/// Arm the scan, which then runs forever from the same buffer.
bool hub75Start(Hub75Handle& h);

/// Measured refresh in Hz from the completed-scan counter; 0 before the first scan.
uint16_t hub75RefreshHz(const Hub75Handle& h) MM_NONBLOCKING;

/// Which backend this handle runs, for the status line; null before a successful init.
const char* hub75Backend(const Hub75Handle& h);

/// Release the port and its buffer.
void hub75Deinit(Hub75Handle& h);
#ifndef ESP_PLATFORM   // host-test seams, which no ESP32 code calls

/// Open the host's HUB75 port on LCD_CAM, and on Parlio too when `parlio`, with a frame of the geometry's size; off, the port refuses as on a chip without the silicon. Each call resets what hub75TestPort() holds and the two answers below.
void setTestHub75(bool on, bool parlio = false);
/// What hub75RefreshHz answers while the test port is open.
void setTestHub75RefreshHz(uint16_t hz);
/// Make hub75Start refuse while the test port is open, as a bus that will not arm.
void setTestHub75StartFails(bool fails);
/// What the test port was handed, which a test reads.
struct Hub75TestPort {
    Hub75Backend backend = Hub75Backend::LcdCam;   ///< the backend the last hub75Init asked for
    Hub75Pins pins;                                ///< the pins the last hub75Init was handed, the clock edge among them
    size_t offeredBytes = 0;                       ///< the frame bytes the last hub75BackendAvailable asked about
    uint32_t deinits = 0;                          ///< the hub75Deinit calls so far
};
/// The test port's record of what the driver handed it.
const Hub75TestPort& hub75TestPort();
/// The frame the driver last packed while the test port is open, `bytes` long; null while closed.
const uint8_t* hub75TestFrame(size_t* bytes);
#endif

}  // namespace mm::platform
