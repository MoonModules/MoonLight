// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// HUB75 from a Pi 5's RP1 PIO: the seam MoonLight's own PIO engine sits behind, and the host test port that stands in for it.
///
/// The seam takes tightly packed RGB, since the engine encodes the bit planes and refreshes from its own threads. It drives GPIO 2 to 27, so the header's I2C1, SPI0, UART0, 1-wire, I2S and PWM must be off (hub75_board_pins.h, Pins the kernel may own). It lives apart from platform.h as a self-contained concern, as edid.h and Hub75Bus.h do, and its users include it directly.

#include <cstddef>
#include <cstdint>

#include "platform/nonblocking.h"   // MM_NONBLOCKING

namespace mm::platform {

/// One wall of panels under the PIO output's own option names: the panels as wired, in lanes the active 3-channel board drives together. The canvas is `cols * panelsPerLane` wide and `rows * parallel` high; each lane is one `cols * panelsPerLane` by `rows` strip, the lanes stacked.
/// @technical The engine encodes the bit planes and refreshes from its own thread, so the seam takes RGB rather than the bus words `hub75Buffer` takes. MoonLight's correction already applied the curve, so the engine runs identity gamma.
struct Hub75PioOptions {
    uint8_t  rows         = 64;          ///< rows of one panel
    uint16_t cols         = 128;         ///< columns of one panel
    uint8_t  panelsPerLane = 2;          ///< panels in series on one lane
    uint8_t  parallel     = 3;           ///< lanes, 1 to 3, one strip each
    uint8_t  scanRate     = 32;          ///< the panel's own scan rate: 8, 16 or 32
    uint8_t  bitDepth     = 6;           ///< bit planes per frame, 2 to 8, default 6
    uint32_t pixelClockHz = 10'000'000;  ///< the HUB75 pixel clock; tops out at 40 MHz
    uint8_t  latchBlankWords = 0;        ///< extra dark words before each latch, 0 to 8, for a panel that ghosts
    bool     clockFallingEdge = false;   ///< the panel's shift registers sample on the falling clock edge rather than the rising one
};

/// Whether this host can open panels: piolib linked and a PIO the kernel serves, or a test mode holding the seam.
bool hub75PioAvailable();

/// Why this host cannot clock the panels through a Pi 5's PIO, or null when it can: no `/dev/pio0`, or a kernel that stalls a PIO reopen. Read once.
const char* hub75PioRefusal();

/// Open the panels, closing any already open; false leaves none open and hub75PioLastError() says why. Open, close, push, correction and fps share one task, and a push never overlaps a close, which frees what a push copies into.
bool hub75PioOpen(const Hub75PioOptions& o);

/// Why the last open failed, or null when it did not.
const char* hub75PioLastError();

/// What degrades a running panel, or null: a pixel clock above the highest the bench ran (`kPioBenchedPixelClockHz`), where the DMA feed falls behind the wire.
const char* hub75PioWarning();

/// Why the last close did not finish, or null when it did: the refresh thread never returned from a submit, so the panel holds its last word and the PIO stays held until the process exits.
const char* hub75PioCloseError();

/// The canvas the panels form, 0 with none open.
uint16_t hub75PioWidth() MM_NONBLOCKING;
/// The canvas height, 0 with none open.
uint16_t hub75PioHeight() MM_NONBLOCKING;

/// Hand over one frame of tightly packed RGB, width*height*3 bytes; it shows at the next frame boundary, the newest frame winning. Returns at once: the encode thread encodes it and the refresh thread submits it.
void hub75PioPush(const uint8_t* rgb) MM_NONBLOCKING;

/// The brightness the engine lights the next frame at, 0 to 255, 0 dark, as lit time: the planes' windows shorten, the values stay whole; it holds across opens, so the output sets it before its first push.
void hub75PioCorrection(uint8_t brightness) MM_NONBLOCKING;

/// Frames shown in the last second; it falls to 0 while every submit fails.
uint16_t hub75PioFps() MM_NONBLOCKING;

/// Whether a submit has been in flight longer than the stall time: the PIO is not draining and the panel holds its last word. `hub75PioFps` reads 0 while it holds.
bool hub75PioStalled() MM_NONBLOCKING;

/// Submits that failed since the last open, which the output shows as the PIO failing when it rises.
uint32_t hub75PioSubmitErrors() MM_NONBLOCKING;

/// Stop the refresh, darken the panels and release the pins; safe with none open.
void hub75PioClose();

#ifndef ESP_PLATFORM   // a host-test seam, which no ESP32 code calls
/// Who holds the PIO seam on a host.
enum class Hub75PioMode : uint8_t {
    Off,      ///< the host decides: with piolib the real engine on a Pi 5's PIO, without it a refusal naming the library
    Record,   ///< a recorder stands in for the panels: an open keeps its options and a push keeps its frame, and nothing encodes
    Stub,     ///< the real engine runs on the stub backend through the seam's real branch, so a push is encoded and submitted as on a Pi, with no hardware
};
/// Hold the seam in `mode`. Each call closes the seam and resets what hub75PioTestPort() holds, the levers below and the stub backend's faults, queue model and records.
void setTestHub75Pio(Hub75PioMode mode);
/// What hub75PioFps answers while the recorder holds the panels; in Stub mode the engine's own rate answers.
void setTestHub75PioFps(uint16_t hz);
/// What hub75PioStalled answers while the recorder holds the panels; in Stub mode the engine's own answers.
void setTestHub75PioStalled(bool stalled);
/// What hub75PioSubmitErrors answers while the recorder holds the panels; in Stub mode the engine's own count answers.
void setTestHub75PioSubmitErrors(uint32_t count);
/// How long a submit may be in flight before the engine reads as stalled, in milliseconds, while Stub mode holds the seam.
void setTestHub75PioStallMs(uint64_t ms);
/// Whether the seam may reach the real backend (piolib, /dev/pio0) outside a test mode: true in a program, false in the test binary, which refuses such an open first, whatever is linked. A case that wants the host's own refusal sets it.
void setTestHub75PioReal(bool allowed);
/// Opens outside a test mode that were asked for since the program started, whether the seam allowed them or not.
uint32_t hub75PioTestRealAttempts();
/// Opens that reached the real backend since the program started: 0 in a test run, which a listener checks at its end.
uint32_t hub75PioTestRealOpens();
/// How long a close waits for the refresh thread to return from a submit before it reports the thread stuck, in milliseconds, while Stub mode holds the seam.
void setTestHub75PioJoinDeadlineMs(int ms);
/// Make hub75PioOpen refuse in Record and Stub mode, as a PIO that will not open.
void setTestHub75PioOpenFails(bool fails);
/// Make the engine's next `n` encodes fail while Stub mode holds the seam, every one when `n` is -1, none when 0.
void setTestHub75PioEncodeFails(int n);
/// Force the availability hub75PioAvailable() reports, whatever the host: -1 the host decides again, 0 unavailable, 1 available.
void setTestHub75PioAvailable(int available);
/// Stand in for the host the PIO check reads: a kernel release as `uname -r` prints it and whether `/dev/pio0` exists; a null release reads the real host again.
void setTestPioHost(const char* kernelRelease, bool pioDevice);
/// What the PIO test seam saw, as of the call to hub75PioTestPort().
struct Hub75PioTestPort {
    Hub75PioOptions options;          ///< the options of the last successful open, the defaults while closed
    uint32_t opens = 0;               ///< successful opens since the mode was set
    uint32_t closes = 0;              ///< closes of an open seam since the mode was set
    uint8_t brightness = 255;         ///< the last brightness handed over
    size_t frames = 0;                ///< frames pushed since the last open: the recorder's count in Record mode, the engine's published count in Stub mode
    bool engineHoldsPanels = false;   ///< whether the real engine, rather than the recorder, holds the panels
};
/// The PIO test seam's record of what the output handed it.
const Hub75PioTestPort& hub75PioTestPort();
/// The newest frame pushed, width*height*3 bytes, taken from the recorder's triple buffer as its consumer takes it; null outside Record mode and before the first push.
const uint8_t* hub75PioTestFrame();
#endif

}  // namespace mm::platform
