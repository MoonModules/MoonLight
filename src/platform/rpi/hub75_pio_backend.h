// SPDX-License-Identifier: GPL-3.0-or-later

/// The seam between the PIO engine and RP1 PIO: the `PioBackend` interface the engine talks through, and the RP1 clock and per-pixel-clock constants the refresh rate derives from.
///
/// Two implementations exist: PiolibBackend (hub75_pio_backend_piolib.cpp) is the real one, using Raspberry Pi's piolib (BSD-3-Clause) over /dev/pio0, built only when piolib is found (see the root CMakeLists.txt).
///
/// StubBackend (hub75_pio_backend_stub.cpp) has no hardware: submitFrame() takes the time the frame would take on the wire, or only the wait for a free buffer, so the refresh thread, the swap handshake and the engine run, and can be tested, on a machine with no Pi attached; it is always built.
///
/// The interface is deliberately narrow: open, start (@xref{start|start}), submitFrame (the DMA transfer of one whole frame, @xref{submit-semantics|submit semantics}), blankAndStop (@xref{blank-first-shutdown|blank-first shutdown}) and close. Everything PIO-specific (pin directions, shift config, FIFO join, clock divider) lives in the backend, not the engine.
///
/// @technical
///
/// ## Submit semantics
///
/// submitFrame() is piolib's pio_sm_xfer_data(): it copies the buffer into kernel DMA bounce buffers (at most `xferBufCount` of `xferBufSize` bytes) and submits the DMA. What the return means depends on the kernel.
///
/// On rpi-6.12.y it returns when every chunk has been delivered to the SM's TX FIFO, so the whole frame has been handed over and the SM has shifted out all but the few words the FIFO holds. The wait has a 1 s timeout.
///
/// When the timeout passes, the kernel answers -ETIMEDOUT and the pinned piolib (third_party/rpi-utils, whose `rp1_ioctl` returns it like any other error) hands it to the caller, so the engine counts a failed submit and retries. Stock piolib calls pio_panic() there, which exits the process with the panel on the last word it shifted, so a build against it ends the program.
///
/// On 6.18.39 and later it returns once the last chunk is queued, and blocks only while all `xferBufCount` buffers are busy, with no timeout. A frame is therefore not delivered on return.
///
/// A state machine that stopped draining makes the call block for good. The engine's shutdown deadline reports that case (hub75_pio_engine.h, Shutdown); the refresh thread stays stuck until the kernel lets it go or the process exits.
///
/// On either kernel a failed submit returns a negative errno-style code, such as -EINTR, -EIO, -EINVAL, -ETIMEDOUT or -EREMOTEIO, which the engine counts and retries after a pause; the pinned piolib returns every code.
///
/// A return from either kernel leaves the pins' state open, which is why the shutdown blank is as deep as the whole ring. The FIFO holds at most 8 words per direction; what the joined TX FIFO holds is not stated by the sources.
///
/// PIO IRQ delivery to userspace is new in piolib and absent from some kernels (rpi-6.12.y has no IRQ path), so the engine does not depend on IRQs.
///
/// The stub models both: the blocking model returns after the frame's wire time, the ring model returns once queued.
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
/// A test must stay clear of the PIO of the Pi it runs on. The gate sits where every open of the real backend converges, createPioBackend(false), so no path around the seam, such as an engine opened directly, can reach /dev/pio0 while the gate is closed. The seam refuses earlier, with a readable reason, and the test binary's main counts the real backends created and fails the run if any was.
///
/// ## Blank-first shutdown
///
/// blankAndStop() submits the blank, which the engine makes as deep as the whole DMA ring, waits until the TX FIFO is empty (the SM is then holding the dark word), disables the SM, clears the FIFOs and removes the program.
///
/// The pins are left in PIO function holding the dark state, so the panel stays blanked after shutdown. If a step fails, the remaining steps still run (best effort), `error` is filled and the call returns false.

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

/// The hardware interface the engine drives: open the device, start the state machine, submit frames, shut down blank.
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

    /// Shuts down blank-first: the panel is dark before the SM stops; the engine passes a blank as deep as the whole DMA ring; fills `error` and returns false if any step fails (@xref{blank-first-shutdown|blank-first shutdown}).
    virtual bool blankAndStop(const void* blankData, uint32_t blankBytes,
                              std::string& error) = 0;

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

/// Returns the stub backend when `preferStub` is set, else the piolib backend, or null when the build has none or the real one is not allowed (@xref{real-backend-gate|the real-backend gate}); the caller owns it. Every path to the real PIO ends here.
PioBackend* createPioBackend(bool preferStub);

/// Why createPioBackend(false) returned null: the real backend is not allowed, or this build has no piolib.
const char* pioBackendRefusal();

/// Whether createPioBackend(false) may return the real backend: true in a program, and false in the test binary, where the listener sets it before every case (@xref{real-backend-gate|the real-backend gate}).
bool pioBackendRealAllowed();
/// Allow or refuse the real backend.
void setPioBackendRealAllowed(bool allowed);
/// The real backends created since the program started: 0 in a test run, which the test binary's main checks at its end.
uint32_t pioBackendRealOpens();

/// Creates the stub backend, always linked in so the engine tests run it on any host; production selects the piolib backend.
PioBackend* createStubBackend();
#ifdef MM_HUB75_PIO_HAVE_PIOLIB
/// Creates the piolib backend.
PioBackend* createPiolibBackend();
#endif

}  // namespace mm::platform
