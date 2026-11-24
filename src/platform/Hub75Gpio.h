// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// HUB75 from a Pi 3/4's GPIO header, gated by `hasHub75Gpio`: the seam MoonLight's own GPIO engine sits behind, and the host test port that stands in for it.
///
/// The seam takes tightly packed RGB, since the engine encodes the bit planes and refreshes from its own threads, and a hardware PWM pulser times OE; the engine clocks the columns from the CPU rather than by DMA (why: rpi/hub75_gpio_backend.h, Why no DMA). It drives the same header pins as the PIO seam (hub75_board_pins.h), so the header's I2C1, SPI0, UART0, 1-wire, I2S and PWM must be off.
///
/// It lives apart from platform.h as a self-contained concern, as Hub75Pio.h does, and its users include it directly.

#include <cstddef>
#include <cstdint>

#include "platform/Hub75Pio.h"      // kHub75Lanes, kHub75PanelsPerLaneMax: the board's lanes and the panels a lane takes, which both Pi seams share
#include "platform/nonblocking.h"   // MM_NONBLOCKING

namespace mm::platform {

/// The fraction bits the GPIO dither carries by default: two bring back the two bits the default 6 planes drop.
inline constexpr uint8_t kHub75GpioCarryBitsDefault = 2;

/// The most fraction bits the GPIO dither carries: at 3 the pattern repeats within 8 refreshes, an eighth of the refresh rate.
inline constexpr uint8_t kHub75GpioCarryBitsMax = 3;

/// The shortest plane's lit time the engine takes, in nanoseconds, as the reference design validates it.
inline constexpr uint16_t kHub75GpioPwmLsbMinNs = 50;
/// The longest plane's lit time the engine takes, in nanoseconds.
inline constexpr uint16_t kHub75GpioPwmLsbMaxNs = 3000;

/// The longest slowdown the dump takes, in dummy reads after each GPIO write; -1 asks for the model's own.
inline constexpr int8_t kHub75GpioSlowdownMax = 4;

/// The panel's scan wirings, the values of the options' `multiplexing` and of the output's select: normal drives the panel as it is, Stripe as half the rows and double the columns (rpi/hub75_gpio_multiplex.h).
enum Hub75GpioMultiplexing : uint8_t { kHub75MultiplexNormal, kHub75MultiplexStripe, kHub75MultiplexCount };

/// The wirings' names, in that order: the options of the output's select.
inline constexpr const char* kHub75GpioMultiplexNames[kHub75MultiplexCount] = {"normal", "Stripe"};

/// The dark interval per row group in nanoseconds: the address and latch writes the dump loop makes while OE is off, which the rate prediction and the stub's wire time count.
inline constexpr uint16_t kHub75GpioDarkIntervalNs = 500;

/// One wall of panels under the GPIO output's own option names: the panels as wired, in lanes the active 3-channel board drives together. The canvas is `cols * panelsPerLane` wide and `rows * parallel` high; each lane is one `cols * panelsPerLane` by `rows` strip, the lanes stacked.
/// @technical The engine encodes the bit planes and refreshes from its own threads, so the seam takes RGB rather than the bus words `hub75Buffer` takes. MoonLight's correction already applied the curve, so the engine runs identity gamma.
struct Hub75GpioOptions {
    uint8_t  rows          = 64;         ///< rows of one panel
    uint16_t cols          = 128;        ///< columns of one panel
    uint8_t  panelsPerLane = 2;          ///< panels in series on one lane
    uint8_t  parallel      = 3;          ///< lanes, 1 to 3, one strip each
    uint8_t  scanRate      = 32;         ///< the panel's own scan rate, 8, 16 or 32: rows/2 direct, rows/4 under Stripe (the driven panel's scan)
    uint8_t  multiplexing  = 0;          ///< the panel's scan wiring: 0 normal, 1 Stripe (an outdoor panel's stretch-2 mapping: half the rows, double the columns)
    uint8_t  bitDepth      = 6;          ///< bit planes per frame, 2 to 8, default 6
    uint16_t pwmLsbNs      = 130;        ///< the shortest plane's lit time in ns, 50 to 3000; the PWM clock quantizes it (hub75_gpio_pwm.h)
    int8_t   slowdown      = -1;         ///< GPIO slowdown, -1 for auto by model (Pi 4: 2, Pi 3: 1), else 0 to 4
    bool     clockFallingEdge = false;   ///< the panel's shift registers sample on the falling clock edge rather than the rising one
};

/// Whether this host can open panels: the GPIO hardware backend built in and a Pi 3 or 4 under it, or a test mode holding the seam.
bool hub75GpioAvailable();

/// Why this host cannot clock panels through a Pi 3/4's GPIO, or null when it can: not a Raspberry Pi 3 or 4, or a Pi 5 whose header the PIO output drives.
const char* hub75GpioRefusal();

/// The slowdown the engine resolves a requested one to on this host: -1 (auto) takes the per-model default (Pi 4: 2, else 1), any other value passes through. The output's rate prediction reads it, so it quotes the rate the engine runs.
int hub75GpioResolvedSlowdown(int8_t slowdown);

/// The column time in ns at a resolved slowdown: the engine's model (hub75GpioColumnTimeNs in hub75_gpio_backend.h), through the seam so the output's rate prediction quotes the column the engine runs, not a copy of its formula.
uint16_t hub75GpioPredictedColumnTimeNs(int slowdown);

/// The scan a panel of `rows` wires under `multiplexing`: rows/2 direct, rows/4 under Stripe, the driven panel's.
uint8_t hub75GpioScanRate(uint8_t multiplexing, uint8_t rows);

/// Whether a panel of `rows` gives a scan the engine drives under `multiplexing`: a whole 8, 16 or 32 address steps.
bool hub75GpioRowsDriveScan(uint8_t multiplexing, uint8_t rows);

/// The columns the driven panel has for a panel of `cols` under `multiplexing`: the same direct, double under Stripe.
uint32_t hub75GpioDrivenCols(uint8_t multiplexing, uint16_t cols);

/// Open the panels, closing any already open; false leaves none open and hub75GpioLastError() says why. Open, close, push, correction and fps share one task, which serializes a push and a close: a close frees what a push copies into.
bool hub75GpioOpen(const Hub75GpioOptions& o);

/// Why the last open failed, or null when it did not.
const char* hub75GpioLastError();

/// What degrades a running panel, or null: the refresh thread has no real-time priority, so scheduling jitter lands in the column timing.
const char* hub75GpioWarning();

/// What the last close reported, or null when it finished clean: the refresh thread stuck in a dump, or a stop that failed.
const char* hub75GpioCloseError();

/// The canvas the panels form, 0 with none open.
uint16_t hub75GpioWidth() MM_NONBLOCKING;
/// The canvas height, 0 with none open.
uint16_t hub75GpioHeight() MM_NONBLOCKING;

/// Hand over one frame of tightly packed RGB, width*height*3 bytes; it shows at the next frame boundary, the newest frame winning. Returns at once: the encode thread encodes it and the refresh thread dumps it.
void hub75GpioPush(const uint8_t* rgb) MM_NONBLOCKING;

/// The brightness the engine scales the codes by, 0 to 255, and the fraction bits the dither carries between refreshes, 0 off, above kHub75GpioCarryBitsMax taken as the maximum; both hold across opens.
void hub75GpioCorrection(uint8_t brightness, uint8_t carryBits) MM_NONBLOCKING;

/// Frames shown in the last second; it falls to 0 while every dump fails.
uint16_t hub75GpioFps() MM_NONBLOCKING;

/// Whether a dump has been in flight longer than the stall time: the refresh thread is not returning and the panel holds its last row. `hub75GpioFps` reads 0 while it holds.
bool hub75GpioStalled() MM_NONBLOCKING;

/// Dumps that failed since the last open, which the output shows as the GPIO failing when it rises.
uint32_t hub75GpioSubmitErrors() MM_NONBLOCKING;

/// Stop the refresh, darken the panels and release the pins; safe with none open.
void hub75GpioClose();

#ifndef ESP_PLATFORM   // a host-test seam, which no ESP32 code calls
/// Who holds the GPIO seam on a host.
enum class Hub75GpioMode : uint8_t {
    Off,      ///< the host decides: where the build has the hardware backend the real engine on a Pi 3 or 4, otherwise a refusal naming the build
    Record,   ///< a recorder stands in for the panels: an open keeps its options and a push keeps its frame, and the encoder stays idle
    Stub,     ///< the real engine runs on the stub backend through the seam's real branch, so a push is encoded and dumped as on a Pi, with no hardware
};
/// Hold the seam in `mode`. Each call closes the seam and resets what hub75GpioTestPort() holds, the levers below and the stub backend's records.
void setTestHub75Gpio(Hub75GpioMode mode);
/// What hub75GpioFps answers while the recorder holds the panels; in Stub mode the engine's own rate answers.
void setTestHub75GpioFps(uint16_t hz);
/// What hub75GpioStalled answers while the recorder holds the panels; in Stub mode the engine's own answers.
void setTestHub75GpioStalled(bool stalled);
/// What hub75GpioSubmitErrors answers while the recorder holds the panels; in Stub mode the engine's own count answers.
void setTestHub75GpioSubmitErrors(uint32_t count);
/// Opens or closes the seam's gate outside a test mode: true in a program, false in the test binary before every case (hub75_gpio_backend.h, Real-backend gate).
void setTestHub75GpioGateOpen(bool open);
/// Opens outside a test mode that were asked for since the program started, whether the seam allowed them or not.
uint32_t hub75GpioTestRealAttempts();
/// Opens that reached the real backend since the program started: 0 in a test run, which a listener checks at its end.
uint32_t hub75GpioTestRealOpens();
/// Make hub75GpioOpen refuse in Record and Stub mode, as panels that will not open.
void setTestHub75GpioOpenFails(bool fails);
/// Make the engine's next `n` encodes fail while Stub mode holds the seam, every one when `n` is -1, none when 0.
void setTestHub75GpioEncodeFails(int n);
/// Force the availability hub75GpioAvailable() reports, whatever the host: -1 the host decides again, 0 unavailable, 1 available.
void setTestHub75GpioAvailable(int available);
/// What the GPIO test seam saw, as of the call to hub75GpioTestPort().
struct Hub75GpioTestPort {
    Hub75GpioOptions options;                                    ///< the options of the last successful open, the defaults while closed
    uint32_t opens = 0;                                          ///< successful opens since the mode was set
    uint32_t closes = 0;                                         ///< closes of an open seam since the mode was set
    uint8_t brightness = 255;                                    ///< the last brightness handed over
    uint8_t carryBits = kHub75GpioCarryBitsDefault;              ///< the last carry bits handed over
    size_t frames = 0;                                           ///< frames pushed since the last open: the recorder's count in Record mode, the engine's published count in Stub mode
    bool engineHoldsPanels = false;                              ///< whether the real engine, rather than the recorder, holds the panels
};
/// The GPIO test seam's record of what the output handed it.
const Hub75GpioTestPort& hub75GpioTestPort();
/// The newest frame pushed, width*height*3 bytes, taken from the recorder's triple buffer as its consumer takes it; null outside Record mode and before the first push.
const uint8_t* hub75GpioTestFrame();
#endif

}  // namespace mm::platform
