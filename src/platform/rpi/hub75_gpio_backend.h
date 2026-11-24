// SPDX-License-Identifier: GPL-3.0-or-later

/// The seam between the HUB75 GPIO engine and the Pi 3/4 GPIO and PWM hardware: the `GpioBackend` interface, the host refusal and the column-time model that predicts the refresh rate.
///
/// The engine reaches the hardware only through the narrow interface: open, start (configure the PWM clock and the pulser, mux OE to the PWM), submitFrame (blocking dump of one whole frame), blankAndStop (the blank-first shutdown), close; register mapping, pin directions, the dark-first setup order and the dump loop live in the backend.
///
/// HwBackend (hub75_gpio_backend_hw.cpp) maps the GPIO / PWM / clock-manager registers through /dev/mem, drives the columns from the refresh thread and times OE with the PWM pulser (hub75_gpio_pwm.h); a Linux aarch64 build creates it (see the root CMakeLists.txt), it needs no external library, only /dev/mem and root, and its open() fails with the reason on a host that is no Pi 3 or 4.
///
/// StubBackend (hub75_gpio_backend_stub.cpp) has no hardware: submitFrame() sleeps for the time the frame would take on the wire, so the refresh thread, the swap handshake and the engine run, and can be tested, on a machine with no Pi; it is always built, is the engine test's dry run, and production runs the hardware backend.
///
/// The engine claims no DMA channel (@xref{why-no-dma|why no DMA}), and the dump is synchronous, so submitFrame() is the frame-done signal (@xref{frame-done|frame done}).
///
/// @technical
///
/// ## Why no DMA
///
/// The engine drives the panel columns from the CPU and times OE with the hardware PWM pulser, so no DMA channel is claimed, no DMA control block is built and the dma.dmachans mask is not consulted. A DMA submitter fits behind the backend interface if a hardware measurement overturns the DMA rate, since the engine does not care how submitFrame() moves bits.
///
/// DMA GPIO rates measured on a Pi 4 are ~1.5-1.8 MHz of GPIO operations (hzeller's rpi-gpio-dma-demo, oscilloscope-verified), which put a DMA-streamed wall at 15-18 Hz computed, far below the 80-100 Hz flicker threshold, while the CPU loop sustains ~235 ns per column at slowdown 1, ~86 Hz computed on the wall, inside that band and calibrated in `hub75GpioColumnTimeNs`.
///
/// ## Release semantics
///
/// open() and close() are symmetric: close() releases every claim open() made, so a second open() in the same process re-acquires everything from scratch. The PWM channel is disabled, the OE pin is muxed back to a GPIO output driving high (dark), the register mappings are unmapped, /dev/mem is closed and all state is reset.
///
/// No claim survives close(), and no piolib-style IRQ thread exists to leak one. close() is safe to call after a failed open or start (a no-op for anything not acquired), and whenever the registers are mapped it drives the dark levels first: blank before stopping, even on the error path.
///
/// The engine test pins this: open, shutdown, open again on the stub.
///
/// ## Frame done
///
/// submitFrame() is the frame-done signal: the dump is synchronous, so the call returns only after the last column of the last row-group has been clocked out. The last OE pulse may still be running, and it lights a row the next submit re-clocks before latching, exactly as the reference design's loop does.
///
/// That is the safe swap (wait for proof before recycling a buffer), provided by the call's return rather than by an interrupt.
///
/// ## Dark-first setup
///
/// open() writes the dark levels first (OE high, color and address 0, latch low), then makes the pins outputs; start() then configures the PWM and muxes OE to it, and OE stays high throughout: GPIO-driven high, then PWM idle high.
///
/// A failure of open() (not a Pi 3/4, no /dev/mem access, the audio driver owns the PWM, a mapping failed) fills `error`, returns false and leaves every claim released.
///
/// ## The frame dump
///
/// The frame is plane-major in memory, and the dump walks rows outer, planes inner (hub75_gpio_dump.h): for each row and plane it clocks the group's columns in while the previous OE pulse runs, waits for it to finish (which blanks the panel), sets the address and strobes in the dark, and fires the next OE pulse.
///
/// `words` is the GPIO data-word frame (one word per pixel clock, color bits only) and `wordCount` is its length; the return is 0 on success or a negative errno-style code on failure.
///
/// ## Blank-first shutdown
///
/// blankAndStop() dumps the blank frame (all color bits clear, so the OE pulses light no pixel), waits for the last pulse to finish, muxes OE back to a GPIO output driving high (dark) and stops the PWM; the pins are left driving the dark levels, so the panel stays blanked after shutdown.
///
/// If a step fails (the blank frame does not dump, or the last pulse does not finish in time), the remaining steps still run (best effort), `error` is filled and the call returns false.
///
/// ## Model detection
///
/// The SoC comes from platform/machine.h (`piSoc()`): the device tree's compatible list names it, with cpuinfo's Model line as the fallback, and a test sets it with `setTestPiSoc`. A BCM2711 is a Pi 4, a BCM2837 a Pi 3, and any other is refused.
///
/// ## Real-backend gate
///
/// A test must stay clear of the GPIO of the Pi it runs on, since an open there lights real panels. The gate sits where every real open converges, createGpioBackend(false), so no path around the seam, such as an engine opened directly, can reach /dev/mem from a test binary. The seam refuses earlier, with a readable reason.
///
/// The gate is open or closed: a program's gate is open, and the test binary closes it before every case. A case reopens it only to test a refusal that comes before any backend, and the test binary's main counts the real backends created and fails the run if any was.
///
/// ## Host refusal
///
/// The refusal is the SoC's answer: a host that is no Pi 3 or 4 is refused, a Pi 5 with a pointer to the PIO output. /dev/mem access and the audio-driver conflict are checked at open(), so their reasons reach the status line instead of hiding the backend.
///
/// ## Slowdown resolution
///
/// The backends resolve the slowdown at start(), and the output's rate prediction resolves through the same seam, so a Pi 4 on auto quotes its slowdown-2 rate in both places.
///
/// ## Column time
///
/// `hub75GpioColumnTimeNs` is how long one column step takes in the dump loop, in ns, at a resolved slowdown: 223 ns plus 12 ns a slowdown unit (235 ns at slowdown 1, 247 at 2, 259 at 3), calibrated on the Pi 4 sign with the encode on its own thread beside the refresh thread, so the carry does not move it (@xref{calibration}, @xref{delay-loops}, @xref{tolerance}).
///
/// The card stores the select's index (0=auto, 1="0", ..., 5="4") and slowdownRequested() returns index - 1 (Hub75GpioOutput.h), so bench notes quote the resolved slowdown the card shows as `slowdownTrue`.
///
/// ## Calibration
///
/// The sign at 6-bit and a 160 ns LSB is shift-bound at every slowdown, so its frame is 6 planes x 8 rows x (128 columns x column time + 0.5 us dark). Three readings at three true slowdowns:
///
/// ```
/// slowdown 1: 682 fps, 1,466,276 ns a frame, 234.7 ns a column
/// slowdown 2: 649 fps, 1,540,832 ns a frame, 246.9 ns a column
/// slowdown 3: 620 fps, 1,612,903 ns a frame, 258.6 ns a column
/// ```
///
/// The slope is 12 ns a column for each slowdown unit and the intercept 223 ns, one fit through the three single readings. Slowdown 0 is the one discontinuity (@xref{dark-interval-and-slowdown-0}).
///
/// ## Delay loops
///
/// A slowdown unit adds one dummy GPLEV0 read to each of the two delay loops of a column, 12 ns, so each additional read costs about 6 ns: the reads pipeline. The 223 ns intercept is the posted GPIO writes (about 15 ns) and two delay loops of about 104 ns each, whose first read stalls until the writes ahead of it have drained and the peripheral answers.
///
/// The loops dominate the column, not the reads or the writes. The mechanism is inferred from the three readings and is untraced on a scope.
///
/// ## Dark interval and slowdown 0
///
/// The dark interval is 0.5 us a group (`kHub75GpioDarkIntervalNs`): the wire dark of a few writes, the pulse wait's poll and its two delay loops is about 0.4 us, inside the error of the figure, and its slowdown dependence is small enough that the model keeps it constant.
///
/// The OE-bound branch (a long LSB outlasting the shift) reads within 3% on the sign at 8-bit, slowdown 2: it computes 473 Hz at a 306 ns LSB against 469 read, and 446 Hz at 433 ns against 435 read.
///
/// Slowdown 0 runs no delay loop, so no first-read penalty is paid and the column is the 15 ns of writes alone, fitted rather than measured; the panel likely cannot latch at that setup and hold. The frame formula's OE-bound branch keeps the slowdown-0 prediction sane.
///
/// ## Tolerance
///
/// The prediction holds within 1% of the three readings above, and the unit test pins it to them. The model is Pi 4-anchored: a Pi 3's slower CPU reads lower, and no Pi 3 bench exists.
///
/// ## Predicted frame time
///
/// Per (plane, row-group) the frame takes the longer of the group's shift (columns at the column time, plus the 0.5 us dark interval for the address and latch) and the plane's OE time (lsbNs x 2^p).
///
/// At 6-bit and 130 ns LSB every plane is shift-bound and the frame is the shift alone; at large LSB times the top planes go OE-bound.
///
/// The stub backend's wire-time sleep uses the same function, so host timing and the prediction agree by construction.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/Hub75Gpio.h"   // kHub75GpioDarkIntervalNs, kHub75GpioSlowdownMax: the figures the seam and the output share
#include "platform/machine.h"     // PiSoc: which Pi the backend drives

namespace mm::platform {

/// The peripheral base address the ARM sees for `soc`: 0x3F000000 on a BCM2837, 0xFE000000 on a BCM2711, 0 for any other.
uint32_t hub75GpioPeripheralBase(PiSoc soc);

/// The rate the PWM clock's source, PLLD, runs at on `soc`: 750 MHz on a BCM2711 (read as `plld_per` on a Pi 4), 500 MHz on a BCM2837 and the others; the PWM tick derives from it (hub75_gpio_pwm.h).
uint32_t hub75GpioPlldHz(PiSoc soc);

/// The refusal for a Pi 5, whose header the PIO output drives.
inline constexpr const char* kGpioPi5Refusal = "this is a Raspberry Pi 5: use the Pi PIO output, not Pi GPIO";

/// The refusal for a host that is no Pi 3 or 4.
inline constexpr const char* kGpioNotAPiRefusal = "this host is not a Raspberry Pi 3 or 4";

/// Null when this host can serve the GPIO backend (a Pi 3 or 4 by its SoC, @xref{model-detection|model detection}), else the reason: @xref{host-refusal}.
const char* hub75GpioHostRefusal();

/// The time one column step takes in the dump loop, in ns, at a slowdown clamped to 0..4: calibrated on the Pi 4 sign (@xref{column-time}).
inline uint16_t hub75GpioColumnTimeNs(int slowdown) {
    if (slowdown < 0) slowdown = 0;
    if (slowdown > kHub75GpioSlowdownMax) slowdown = kHub75GpioSlowdownMax;
    if (slowdown == 0) return 15;   // no delay loop runs: the writes alone, unmeasured
    return static_cast<uint16_t>(223 + slowdown * 12);   // 235 ns at slowdown 1, 247 at 2, 259 at 3
}

/// One frame's predicted wire time in ns, computed from the column-time model (@xref{predicted-frame-time|predicted frame time}); the measured rate is Hub75GpioEngine::fps().
inline uint64_t hub75GpioPredictedFrameNs(uint16_t scanRows, uint32_t columnsPerGroup,
                                          uint8_t bitDepth, uint16_t lsbNs, uint16_t columnNs) {
    uint64_t frameNs = 0;
    for (uint8_t p = 0; p < bitDepth; p++) {
        const uint64_t shiftNs = static_cast<uint64_t>(columnsPerGroup) * columnNs + kHub75GpioDarkIntervalNs;
        const uint64_t oeNs = static_cast<uint64_t>(lsbNs) << p;
        frameNs += static_cast<uint64_t>(scanRows) * (oeNs > shiftNs ? oeNs : shiftNs);
    }
    return frameNs;
}

/// The slowdown an auto (-1) request resolves to for `soc`: 2 on a BCM2711 (its faster GPIO needs the longer setup), 1 on a BCM2837 and on any other, mirroring the reference design's per-model defaults.
inline int hub75GpioAutoSlowdown(PiSoc soc) {
    return soc == PiSoc::Bcm2711 ? 2 : 1;
}

/// The one home for auto slowdown: resolves a requested slowdown (-1 for auto, else 0..4) for `soc`, at start() and in the output's rate prediction alike (@xref{slowdown-resolution}).
inline int hub75GpioResolveSlowdown(int8_t slowdown, PiSoc soc) {
    return slowdown < 0 ? hub75GpioAutoSlowdown(soc) : static_cast<int>(slowdown);
}

/// What one submitFrame() call needs to know about the frame it dumps, the encoder's grouping and the pulser's timing: fixed for the run, so the engine passes them once at start() and submitFrame() takes only the words.
struct GpioSubmitConfig {
    uint16_t laneWidth = 0;  ///< columns per lane (cols x panelsPerLane)
    uint16_t scanRows = 0;    ///< address steps per plane (the panel's scan rate)
    uint16_t pairs = 0;       ///< color passes per address step (rowsPerScan / 2)
    uint16_t holdWords = 0;   ///< hold words leading each group (Hub75Geometry::holdWords): the dump skips them, clocking only the shift words
    uint8_t bitDepth = 6;     ///< bit planes per frame, 2..8
    uint16_t lsbNs = 130;     ///< the shortest plane's lit time, 50..3000
    int8_t slowdown = -1;     ///< GPIO slowdown, -1 for auto by model, else 0..4
    bool clockFallingEdge = false;  ///< the panel samples on the falling clock edge
};

/// Whether a backend that is `opened` can start with `config` (every dimension set, depth 2..8, LSB 50..3000 ns, slowdown -1..4), else false with the reason in `error`; both backends' start() refuse on it.
bool hub75GpioCheckStart(bool opened, const GpioSubmitConfig& config, std::string& error);

/// The interface the engine drives a Pi 3/4 panel through: open, start, submitFrame, blankAndStop and close, implemented by the hardware backend and the stub.
class GpioBackend {
  public:
    /// Virtual, so the engine destroys a backend through this interface.
    virtual ~GpioBackend() = default;

    /// Probes the hardware, maps the registers and sets the pins up dark, filling `error` and returning false on failure: @xref{dark-first-setup}.
    virtual bool open(std::string& error) = 0;

    /// Configures the PWM clock and the pulser for the config's LSB time, resolves the slowdown and muxes OE to the PWM, filling `error` and returning false on failure: @xref{dark-first-setup}.
    virtual bool start(const GpioSubmitConfig& config, std::string& error) = 0;

    /// Dumps one whole encoded frame to the panels and blocks until it is clocked out, which paces the refresh loop at the display rate: @xref{the-frame-dump}.
    virtual int submitFrame(const uint32_t* words, size_t wordCount) = 0;

    /// Blank-first shutdown, filling `error` and returning false if a step fails: dumps the blank frame, waits for the last pulse, muxes OE back to a GPIO output driving high and stops the PWM: @xref{blank-first-shutdown}.
    virtual bool blankAndStop(const uint32_t* blankWords, size_t blankWordCount,
                              std::string& error) = 0;

    /// Releases everything open() claimed (mappings, /dev/mem, PWM state) and resets, so the next open() re-acquires from scratch: @xref{release-semantics}.
    virtual void close() = 0;

    /// The backend's name, a literal such as "GPIO+PWM (Pi 3/4)" on hardware.
    virtual const char* name() const = 0;

    /// The rate the PWM clock's source runs at, which the pulse timing derives from: the SoC's on hardware, 500 MHz on the stub; valid after a successful start().
    virtual uint32_t plldHz() const = 0;
    /// The slowdown start() resolved (auto by model, else as asked), valid after a successful start().
    virtual int resolvedSlowdown() const = 0;
    /// OE pulses whose finish wait timed out (the PWM wedged): the dump proceeded anyway, so this counts a degraded panel, not a hung one.
    virtual uint64_t pulseTimeouts() const = 0;
};

/// The stub backend when `preferStub` is set, else the hardware backend, or null when the build has none or the gate is closed (@xref{real-backend-gate|real-backend gate}); the caller owns it.
GpioBackend* createGpioBackend(bool preferStub);

/// The refusal for a build with no hardware backend, anything but Linux aarch64.
inline constexpr const char* kGpioNoBackendRefusal = "needs a Raspberry Pi 3 or 4 build (Linux aarch64)";

/// The refusal a closed gate gives for the real backend, which is the test binary's gate in every case.
inline constexpr const char* kGpioTestRefusal = "a test binary keeps the real GPIO closed";

/// Why createGpioBackend(false) returned null: the gate is closed, or this build has no hardware backend (anything but Linux aarch64).
const char* gpioBackendRefusal();

/// Whether the gate is open: true in a program, false in the test binary before every case; the real backend is created only through an open gate.
bool gpioBackendGateOpen();
/// Closes the gate (`open` false), or opens it again for a case that tests a refusal coming before any backend.
void setGpioBackendGateOpen(bool open);
/// The real backends created since the program started: 0 in a test run, which the test binary's main checks at its end.
uint32_t gpioBackendRealOpens();

/// The hardware backend (hub75_gpio_backend_hw.cpp), built on Linux aarch64 alone; createGpioBackend is the one way to it.
GpioBackend* createGpioHwBackend();
/// The stub backend (hub75_gpio_backend_stub.cpp), always linked in so the engine tests run it on any host.
GpioBackend* createGpioStubBackend();

}  // namespace mm::platform
