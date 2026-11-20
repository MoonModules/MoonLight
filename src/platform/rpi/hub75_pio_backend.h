// SPDX-License-Identifier: GPL-3.0-or-later

/// The seam between the PIO engine and RP1 PIO: the `PioBackend` interface the engine talks through, and the RP1 clock and per-pixel-clock constants the refresh rate derives from.
///
/// Two implementations exist: PiolibBackend (hub75_pio_backend_piolib.cpp) is the real one, using Raspberry Pi's piolib (BSD-3-Clause) over /dev/pio0, built only when piolib is found (see the root CMakeLists.txt).
///
/// StubBackend (hub75_pio_backend_stub.cpp) has no hardware: submitFrame() takes the time the frame would take on the wire, or only the wait for a free buffer, so the refresh thread, the swap handshake and the engine run, and can be tested, on a machine with no Pi attached; it is always built.
///
/// The interface is deliberately narrow: open, start (@xref{start|start}), submitFrame (the DMA transfer of one whole frame, @xref{submit-semantics|submit semantics}), submitBlank, stop (@xref{stop|stop}) and close. Everything PIO-specific (pin directions, shift config, FIFO join, clock divider) lives in the backend, not the engine.
///
/// @technical
///
/// ## Submit semantics
///
/// submitFrame() is piolib's pio_sm_xfer_data(): it copies the buffer into kernel DMA bounce buffers (at most `xferBufCount` of `xferBufSize` bytes) and submits the DMA. What a return means depends on the kernel line, read from the driver source and the released packages (2026-11-18 and 19).
///
/// On rpi-6.6.y, rpi-6.12.y, rpi-6.19.y, rpi-7.0.y and rpi-6.18.y up to 6.18.34 a submit returns when every chunk has been delivered to the SM's TX FIFO, and each wait times out after 1 s with -ETIMEDOUT. The 1 s is per wait, so a frame larger than the ring takes up to 2 s, and the waits ignore signals.
///
/// A timeout leaves the ring out of step on those kernels, from the driver source and not run: descriptors stay queued, the semaphore keeps its count, and the engine's retry copies into buffers still queued, until a new pio_sm_config_xfer resets them.
///
/// The pinned piolib (third_party/rpi-utils, whose `rp1_ioctl` returns every code) hands -ETIMEDOUT to the caller. Stock piolib from commit bb3e2622 (2026-11-13) calls pio_panic() there and ends the process, and every earlier or packaged libpio0 returns -1 for any failure.
///
/// From 6.18.39 (commit ee9c2a40) and on rpi-7.1.y and later a submit returns once the last chunk is queued, and blocks only while all `xferBufCount` buffers are busy, with no timeout. A frame is therefore not delivered on return.
///
/// That wait is interruptible. On 6.18.39 to 6.18.52 a signal ends it and the call returns 0 with the rest of the frame unsent. From 6.18.53 the call returns -EINTR when the handler lacks SA_RESTART and resumes where it has it. The refresh thread blocks SIGINT, SIGTERM and SIGHUP.
///
/// A submit blocks for good when the state machine stops draining, or when a DMA channel will not stop (raspberrypi/linux issue #7642, 6.18.48 to 6.18.52). The join deadline reports it (hub75_pio_engine.h, Shutdown), and the thread stays in the kernel until the kernel lets it go or the process exits.
///
/// A failed submit returns a negative errno-style code, which the engine counts and retries after a pause: -EIO, -EINVAL and -EREMOTEIO anywhere, -ETIMEDOUT where a submit waits for delivery, -EINTR from 6.18.53. Against a packaged libpio0 every code reads -1.
///
/// A return leaves the pins' state open, so the blank is as deep as the whole ring. The FIFO holds at most 8 words per direction, and the sources leave the joined TX depth unstated. rpi-6.12.y has no PIO IRQ path, so the engine does not use IRQs.
///
/// The stub models both kinds: the blocking model returns after the frame's wire time, the ring model once queued.
///
/// ## Start
///
/// start() loads the program, configures the SM (pins, shift, FIFO join, clock divider), configures the DMA transfer ring, clears the FIFOs and enables the SM.
///
/// The `PioStartConfig` carries `programInstructions` and `programLength` from the pioasm-generated hub75.pio.h (the rising- or falling-edge variant, as chosen by the engine), and `wrapTarget` and `wrap`, the program's wrap bounds.
///
/// Its `clkdivInt` and `clkdivFrac` set the SM clock divider (divider = int + frac/256, int >= 1), which `pioClockDivider` rounds from the pixel clock in integer arithmetic. `xferBufSize` and `xferBufCount` size the kernel DMA bounce buffers (count <= 4, the kernel limit).
///
/// ## Real-backend gate
///
/// A test must stay clear of the PIO of the Pi it runs on. The gate sits where every open of the real backend converges, createPioBackend(false), so no path around the seam, such as an engine opened directly, can reach /dev/pio0 from a test binary. The seam refuses earlier, with a readable reason.
///
/// The test binary's main counts the real backends created and fails the run if any was, unless the run opted in with `MM_TEST_REAL_PIO=1`, which makes those opens deliberate (it then prints a note).
///
/// The test binary closes the gate before every case, and a case may reopen it. The real backend then opens only when the environment holds `MM_TEST_REAL_PIO=1`, the one switch that lets a test binary reach /dev/pio0. CI and ordinary runs stay clear of the PIO on every build, a Pi 5 included, and a run on a Pi 5 with the PIO free sets it deliberately. The refusal text names the variable.
///
/// ## Stop
///
/// The refresh thread submits the blank itself as it leaves its loop (submitBlank), as deep as the whole DMA ring, so on a kernel that queues its submit returns only after every real chunk has gone out. It skips the blank when its last frame submit failed. The join deadline then covers the one call that can block for good.
///
/// The closing thread makes no submit: on 6.18.39 and later a submit blocks with no timeout when the SM stops draining, and a second submitter shares the ring and the semaphore with no lock.
///
/// stop() is firmware-message calls only, each bounded at 1 s by the kernel (read on 6.12 and 6.18): it disables the SM, sets the OUT pins to `kBlankWideWord` with the call start() uses, clears the FIFOs and removes the program. It runs from the closing thread while the refresh thread may be inside the kernel.
///
/// With `drain` set, because the refresh thread came back and its blank went out, stop() first waits out the blank's wire time (the ring's words over the pixel clock, doubled for the feed lag), then polls the TX FIFO for empty within 100 ms. On 6.18.39 and later an empty FIFO right after the submit leaves a ring of blank chunks queued.
///
/// The wait lets the DMA channel go idle before the SM stops, which matters on kernels that lack the channel abort (rpi-6.12.y and 6.18.39 to 6.18.52). From 6.18.53 a stop with chunks queued costs a 50 ms abort at the next configure or close.
///
/// With `drain` clear, because the join deadline passed or the thread skipped its blank, stop() skips the wait. After a deadline the panel goes dark while the thread is still inside the kernel, and the engine reports wedged and keeps the backend and the buffers, which the kernel reads while that thread is inside.
///
/// The dark word stays on the pins after stop(). Measured on 6.18.60, 2026-11-19: with the SM disabled, setting the dark word turned OE high; set pins and clear FIFOs each returned in under 0.04 ms while another thread was held in a submit. The disable was issued before that thread was held, so a disable with a thread already blocked in a submit is not measured. 6.12 is unchecked.
///
/// A step that fails leaves the others running and puts its failure in `error`. The shim reports the disable and the pins, whose piolib calls return void (hub75_pio_c_api.h, Silent calls); the FIFO clear and the program removal are not checked. stop() returns whether the panel is dark: the disable and the pin write both succeeded, in this stop or an earlier one.
///
/// A stop that returned false repeats the disable, the pins and the FIFO clear on the next call, with a wait of zero, until both have succeeded once, so a wedged engine's later shutdown retries it. A stop after a true finds the SM stopped already, and a submit after any stop answers -ENODEV. A FIFO that did not drain is a note on a dark panel.
///
/// The stop relies on non-DMA ioctls staying independent of a blocked transfer (true on 6.6, 6.12, 6.18 and 7.x; PR #7771, open, would add a lock per DMA channel that only configure, transfer and release take), on the 1 s message timeout, and on the firmware setting pins of a disabled SM.

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace mm::platform {

/// RP1 PIO system clock in Hz: clk_sys "typically runs at 200MHz" (RP1 peripherals datasheet) and piolib's rp1_clock_get_hz(clk_sys) returns 200 MHz; the pixel clock and refresh rate derive from it.
constexpr uint32_t kRp1PioClockHz = 200'000'000;

/// The highest pixel clock the bench ran: above it the seam warns, since the DMA feed took about 1.8 times a frame's wire time at 20 MHz and the picture was not checked past it (hub75_pio_engine.h, Feed).
constexpr uint32_t kPioBenchedPixelClockHz = 20'000'000;

/// PIO cycles per pixel clock for the hub75_wide program: pull (1) + out [1] (2) + nop [1] (2), see hub75.pio.
constexpr int kCyclesPerPixelClock = 5;

/// One 32-bit FIFO word per pixel clock (the wide word).
constexpr int kBytesPerPixelClock = 4;

/// The SM clock divider for a pixel clock, as the RP1 register holds it: a 16-bit integer part and an 8-bit fraction, and the pixel clock the rounded divider yields.
struct PioClockDivider {
    uint16_t intPart;     ///< divider integer part, 1..65535
    uint8_t fracPart;     ///< divider fraction in 1/256ths
    uint32_t actualHz;    ///< the pixel clock after rounding, 200 MHz / (5 x divider)
};

/// Rounds `pixelClockHz` to the nearest 1/256 of a divider, in integer arithmetic (ties up), floored at divider 1 (a 40 MHz pixel clock) and capped at 65535 + 255/256.
constexpr PioClockDivider pioClockDivider(uint32_t pixelClockHz) {
    constexpr uint64_t kPixelRateHz = kRp1PioClockHz / kCyclesPerPixelClock;
    constexpr uint64_t kScale = 2 * kPixelRateHz * 256;
    const uint64_t pixelClock = pixelClockHz ? pixelClockHz : 1;
    uint64_t divider256 = (kScale + pixelClock) / (2 * pixelClock);
    if (divider256 < 256) divider256 = 256;
    const uint64_t whole = std::min<uint64_t>(divider256 >> 8, 65535);
    const uint64_t rounded256 = whole << 8 | (divider256 & 255);
    return {static_cast<uint16_t>(whole), static_cast<uint8_t>(divider256 & 255),
            static_cast<uint32_t>((kScale + rounded256) / (2 * rounded256))};
}

/// The doubling a blank's wire time gets for the feed lag: the bench measured a frame taking about 1.2 times its wire time at 10 MHz and about 1.8 times at 20 MHz (hub75_pio_engine.h, Feed).
constexpr uint32_t kPioFeedLagFactor = 2;

/// The margin on the wire time the shutdown allows, in percent: half again over the feed-lag doubling, for the scheduler and the frame the thread was in.
constexpr uint32_t kPioShutdownMarginPercent = 50;

/// The microseconds `words` wide words take on the wire at the SM clock divider `clkdivInt` + `clkdivFrac`/256, rounded up, with no feed lag: 5 PIO cycles of the 200 MHz clock a word.
constexpr uint64_t pioWireMicros(uint64_t words, uint16_t clkdivInt, uint8_t clkdivFrac) {
    constexpr uint64_t kCyclesPerMicro = kRp1PioClockHz / 1'000'000;
    const uint64_t divider256 = static_cast<uint64_t>(clkdivInt) << 8 | clkdivFrac;
    const uint64_t cycles256 = words * kCyclesPerPixelClock * divider256;
    return (cycles256 + kCyclesPerMicro * 256 - 1) / (kCyclesPerMicro * 256);
}

/// The milliseconds the shutdown needs on the wire: the frame the thread may be inside plus the ring-deep blank (`frameWords`, `ringWords`), doubled for the feed lag with a margin; the join deadline follows it.
constexpr uint64_t pioShutdownWireMs(uint64_t frameWords, uint64_t ringWords, uint16_t clkdivInt, uint8_t clkdivFrac) {
    const uint64_t micros = (pioWireMicros(frameWords, clkdivInt, clkdivFrac) + pioWireMicros(ringWords, clkdivInt, clkdivFrac)) *
                            kPioFeedLagFactor * (100 + kPioShutdownMarginPercent) / 100;
    return (micros + 999) / 1000;
}

/// How long stop() waits for a blank of `ringWords` words to go out, in microseconds: its wire time doubled for the feed lag. It is always under `pioShutdownWireMs`, which adds the frame and a margin.
constexpr uint64_t pioBlankWaitMicros(uint64_t ringWords, uint16_t clkdivInt, uint8_t clkdivFrac) {
    return pioWireMicros(ringWords, clkdivInt, clkdivFrac) * kPioFeedLagFactor;
}

/// /dev/pio0 as stat() reads it, for the permission message.
struct PioNode {
    bool characterDevice = false;   ///< the node is a character device
    unsigned mode = 0;              ///< its permission bits, as st_mode & 07777
    std::string owner;              ///< the owning user's name, or its number
    std::string group;              ///< the owning group's name, or its number
};

/// The node's mode as `ls -l` prints it, such as `crw-rw----`.
std::string pioModeString(const PioNode& node);

/// What start() needs to know to load the program and run the SM: fixed for the run, so the engine fills it once.
struct PioStartConfig {
    const uint16_t* programInstructions = nullptr;   ///< the pioasm-generated program, rising- or falling-edge variant
    int programLength = 0;       ///< program length in words
    int wrapTarget = 0;          ///< the program's wrap target
    int wrap = 0;                ///< the program's wrap
    uint16_t clkdivInt = 1;      ///< SM clock divider integer part
    uint8_t clkdivFrac = 0;      ///< SM clock divider fraction in 1/256ths
    uint32_t xferBufSize = 0;    ///< kernel DMA bounce buffer size in bytes
    uint32_t xferBufCount = 0;   ///< kernel DMA bounce buffers, at most 4
};

/// The hardware interface the engine drives: open the device, start the state machine, submit frames, stop with the panel dark.
class PioBackend {
  public:
    /// Lets the engine delete a backend through the interface.
    virtual ~PioBackend() = default;

    /// Opens the PIO device and claims one state machine; fills `error` and returns false on failure.
    virtual bool open(std::string& error) = 0;

    /// Loads the program, configures the SM and the DMA transfer ring, and enables the SM; fills `error` and returns false on failure (@xref{start|start}).
    virtual bool start(const PioStartConfig& config, std::string& error) = 0;

    /// Submits one whole frame for DMA into the SM's TX FIFO; returns 0, or a negative errno-style code on failure; what a return means depends on the kernel (@xref{submit-semantics|submit semantics}).
    virtual int submitFrame(const void* data, uint32_t bytes) = 0;

    /// Submits the blank the refresh thread sends as it leaves its loop, as deep as the whole DMA ring; the same transfer as submitFrame, so it blocks the same ways (@xref{stop|stop}).
    virtual int submitBlank(const void* data, uint32_t bytes) { return submitFrame(data, bytes); }

    /// Stops the SM with firmware-message calls only, while another thread may be inside a submit, and returns whether the panel is dark; `drain` waits out the blank first, and `error` holds the failures or a note (@xref{stop|stop}).
    virtual bool stop(bool drain, std::string& error) = 0;

    /// Releases the SM and closes the device; safe to call after a failed open or start (a no-op for anything not acquired).
    virtual void close() = 0;

    /// Returns the backend's display name.
    virtual const char* name() const = 0;
};

/// What the piolib backend says when `pio_open(0)` fails with the positive errno `err`, `node` being /dev/pio0 as stat() reads it, or null. Pure text, so a host test reads it.
std::string pioOpenFailure(int err, const PioNode* node);

/// What the piolib backend says when claiming a state machine returns the negative errno `rc`.
std::string pioClaimFailure(int rc);

/// What the piolib backend says when configuring the DMA ring returns the negative errno `rc`: -EBUSY means another client holds the DMA.
std::string pioXferConfigFailure(int rc, uint32_t bufSize, uint32_t bufCount);

/// What the piolib backend says when loading the program returns the negative errno `rc`.
std::string pioProgramFailure(int rc, int programLength);

/// Returns the stub backend when `preferStub` is set, else the piolib backend, or null when the build has none or a test binary leaves MM_TEST_REAL_PIO=1 unset (@xref{real-backend-gate|the real-backend gate}); the caller owns it.
PioBackend* createPioBackend(bool preferStub);

/// The refusal a test binary gives for the real backend: it opens when the run holds `MM_TEST_REAL_PIO=1`.
inline constexpr const char* kPioTestRefusal = "the real PIO is opened from a test binary only when MM_TEST_REAL_PIO=1 is set";

/// Why createPioBackend(false) returned null: a test binary leaves MM_TEST_REAL_PIO=1 unset, or this build has no piolib.
const char* pioBackendRefusal();

/// Where the real-backend gate stands: a program's gate is open for good, the test binary closes it before every case, and a case may reopen it.
enum class PioBackendGate : uint8_t { Program, Closed, Reopened };

/// Whether the real backend is refused at `gate` in a run that did or did not opt in (`optedIn`): a closed gate always refuses, a reopened one unless the run opted in, and a program's gate always admits it.
constexpr bool pioBackendGateRefuses(PioBackendGate gate, bool optedIn) {
    return gate == PioBackendGate::Closed || (gate == PioBackendGate::Reopened && !optedIn);
}

/// Whether the gate is open: true in a program, false in the test binary before every case. An open gate is no permission for the real backend: the run's environment decides (`pioBackendRealOptedIn`).
bool pioBackendGateOpen();
/// Closes the gate (`open` false), or reopens it for a case that tests the real-backend rule; the real backend then opens only in a run with `MM_TEST_REAL_PIO=1`.
void setPioBackendGateOpen(bool open);
/// Whether the run opted in to the real backend: `MM_TEST_REAL_PIO` is set to exactly `1`, and any other value, `0` included, is not an opt-in. Read from the environment at each call.
bool pioBackendRealOptedIn();
/// The real backends created since the program started: 0 in a test run, which the test binary's main checks at its end.
uint32_t pioBackendRealOpens();

/// Creates the stub backend, always linked in so the engine tests run it on any host; production selects the piolib backend.
PioBackend* createStubBackend();
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
/// Creates the piolib backend.
PioBackend* createPiolibBackend();
#endif

}  // namespace mm::platform
