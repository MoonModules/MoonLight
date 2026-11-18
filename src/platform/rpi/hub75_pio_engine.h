// SPDX-License-Identifier: GPL-3.0-or-later

/// The HUB75 PIO engine: turns RGB frames into encoded slot words and streams them to the panels through the RP1 PIO.
///
/// The render thread publishes an RGB frame and returns at once; an encode thread encodes the newest frame into a buffer of its own and hands it over; a refresh thread submits the newest encoded frame as DMA transfers into the state machine's TX FIFO, which shifts it out at the pixel clock, and submits it again until the next one arrives (@xref{pipeline}, @xref{threading}).
///
/// @attribution pico-examples, pimoroni-pico
///
/// @technical
///
/// ## Pipeline
///
/// One frame travels these stages:
///
/// ```
/// RGB virtual frame, published by the PIO output through the seam
///   -> triple-buffered RGB slot (lock-free publish, newest wins)
///   -> encode thread: per lane, the lane's strip of the virtual frame
///      (lanes stack vertically, so each strip is one contiguous run; NO gamma, MoonLight's correction already applied it)
///   -> MoonLight hub75Encode() into 16-bit slot words (bitplanes, with address / latch / OE baked in, one word per pixel clock)
///   -> repack: three lanes' slot words -> one 32-bit wide word each
///   -> optional latch blanking (ESP32-style)
///   -> triple-buffered encoded frame (lock-free publish, newest wins)
///   -> refresh thread: PIO backend, DMA transfers into the SM's TX FIFO; the SM shifts it out at the pixel clock
/// ```
///
/// ## Threading
///
/// The render thread publishes RGB and returns at once: publishRgb is one memcpy, one atomic exchange and two atomic counts, so it is safe from MoonLight's MM_NONBLOCKING tick.
///
/// The encode thread owns the encode: it takes the freshest RGB frame, encodes it into the back buffer and publishes it. The refresh thread owns the submit: it takes the newest encoded frame, if one was published since its last take, and submits the frame it holds. The buffer being submitted is never written, and an encode never delays a submit.
///
/// If the producer is slower than the display, the refresh thread re-submits the current frame, so the panel keeps refreshing at the pixel-clock rate regardless. If the producer is faster, a publish overwrites the RGB frame still waiting and leaves the one being encoded intact, so the panel shows the newest frame.
///
/// A frame whose encode fails is dropped and the last good frame keeps submitting. The brightness is read once a frame.
///
/// The refresh thread stamps the time it enters each submit. A submit in flight longer than the stall time, two rate windows or four frames' wire time, whichever is longer, reads as `stalled()`, and `fps()` answers 0 meanwhile: a submit that never returns counts no frame, and the rate must not freeze at its last value.
///
/// ## Shutdown
///
/// Shutdown stops publishing, joins the encode thread, joins the refresh thread within a deadline, submits a blank at least as deep as the whole DMA ring (`xferBufCount` buffers of `xferBufSize` bytes), then frees the buffers.
///
/// The blank is ring-deep because on a kernel whose submit returns once queued, the real chunks may still be in flight, and a blank that fills the ring returns only after every one of them has gone out, so the SM stops on a dark word.
///
/// If the refresh thread does not return within the deadline, a submit is stuck in the kernel: shutdown reports it (wedged, shutdownError), keeps the buffers and the backend, which that thread still reads, and a new open is refused until a later shutdown finds the thread gone.
///
/// ## Feed
///
/// The refresh rate this engine computes is the wire rate, the pixel clock over the slots per frame. The DMA feed can run behind it: the bench measured a frame taking about 1.2 times its wire time at 10 MHz and about 1.8 times at 20 MHz (hub75-bench.md), so above 20 MHz the seam warns.
///
/// When the feed runs behind, the state machine stalls on an empty FIFO inside the frame, and a stall lengthens the word it stops on, lit or dark, which shifts plane weights.
///
/// ## Stub backend
///
/// `useStubBackend` of open() is the engine test's dry run: production passes false, and a build that lacks piolib fails open() with that reason.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "platform/Hub75Bus.h"   // Hub75Geometry: the per-lane encode contract
#include "platform/nonblocking.h"   // MM_NONBLOCKING: publishRgb is tick-safe
#include "platform/Hub75Pio.h"   // Hub75PioOptions
#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_options.h"   // hub75PioCheckOptions: the rules open() refuses on
#include "platform/rpi/hub75_pio_repack.h"
#include "platform/rpi/hub75_rpi_common.h"   // Hub75EngineBase: the open sequence and state both engines share

namespace mm::platform {

/// The engine: owns the RGB triple buffer, the encoded triple buffer, the PIO backend and the encode and refresh threads; its open sequence is in hub75_rpi_common.h, and `useStubBackend` runs the stub (@xref{stub-backend|stub backend}).
class Hub75PioEngine final : public Hub75EngineBase<Hub75PioOptions> {
  public:
    /// An engine that is not running.
    Hub75PioEngine() = default;
    /// Shuts the engine down, so a destroyed engine is stopped; an engine whose refresh thread is stuck detaches it.
    ~Hub75PioEngine() {
        shutdown();
        if (refreshThread_.joinable()) refreshThread_.detach();
    }

    /// Blank-first shutdown (@xref{shutdown|shutdown}): joins both threads, blanks ring-deep, drains the FIFO, disables the SM and frees everything; true when finished, false when the refresh thread never returned. Idempotent.
    bool shutdown();

    /// Brightness for subsequently encoded frames (0..255), as lit time; it takes effect on the next encode and leaves the frame on display as it is.
    void setBrightness(uint8_t brightness) MM_NONBLOCKING { brightness_.store(brightness); }

    /// The actual pixel clock after clock-divider rounding, valid after open().
    uint32_t pixelClockHz() const { return pixelClockActualHz_; }
    /// The wire rate in Hz (pixel clock / slots per frame), which the DMA feed may not reach (@xref{feed|feed}); 0 when the engine holds no frame.
    float refreshHz() const {
        return frameSlots_ ? static_cast<float>(pixelClockActualHz_) /
                                 static_cast<float>(frameSlots_)
                           : 0.0f;
    }
    /// DMA bandwidth in bytes/s at the current pixel clock.
    uint32_t dmaBytesPerSecond() const {
        return pixelClockActualHz_ * kBytesPerPixelClock;
    }
    /// The backend's name, or "(none)" before open().
    const char* backendName() const {
        return backend_ ? backend_->name() : "(none)";
    }

  private:
    bool admit(const Hub75PioOptions& o, std::string& error) override;
    void sizeTheWall() override;
    void allocate() override;
    bool startBackend(bool useStubBackend, std::string& error) override;
    void dropBackend() override;
    bool encodeLane(const uint8_t* rgb, int lane, uint16_t* slotWords, uint8_t brightness) override;
    void repack(uint32_t* out) override;
    void refreshLoop() override;

    size_t rowWords_ = 0;        // slots per (plane, row) group: the pass, hold words included
    uint32_t pixelClockActualHz_ = 0;
    uint16_t clkdivInt_ = 1;
    uint8_t clkdivFrac_ = 0;
    uint32_t xferBufSize_ = 0;    // the kernel DMA bounce buffer size, bytes
    uint32_t xferBufCount_ = 0;   // the kernel DMA bounce buffers

    RepackTables tables_{};

    std::unique_ptr<PioBackend> backend_;
};

}  // namespace mm::platform
