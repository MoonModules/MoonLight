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
/// The encode thread owns the encode: it takes the freshest RGB frame, encodes it into the back buffer and publishes it. The refresh thread owns the submit: it takes the newest encoded frame, if one was published since its last take, and submits the frame it holds. The buffer being submitted is read-only, and a submit runs at its own pace, whatever the encode is doing.
///
/// If the producer is slower than the display, the refresh thread re-submits the current frame, so the panel keeps refreshing at the pixel-clock rate regardless. If the producer is faster, a publish overwrites the RGB frame still waiting and leaves the one being encoded intact, so the panel shows the newest frame.
///
/// A frame whose encode fails is dropped and the last good frame keeps submitting. The brightness is read once a frame.
///
/// The refresh thread stamps the time it enters each submit. A submit in flight longer than the stall time, two rate windows or four frames' wire time, whichever is longer, reads as `stalled()`, and `fps()` answers 0 meanwhile: a hung submit counts no frame, and the rate must fall to 0 rather than freeze at its last value.
///
/// ## Shutdown
///
/// Shutdown stops publishing, joins the encode thread, waits for the refresh thread within a deadline, stops the backend and frees the buffers.
///
/// The deadline is `kHub75JoinDeadlineMs` (3 s) at least. A slow pixel clock raises it: it is the wire time of the frame the thread may be inside plus the ring-deep blank, doubled for the feed lag and given half again as a margin (`pioShutdownWireMs`), so a healthy submit at a low clock is not read as stuck.
///
/// At the slowest divider (65535, about 610 Hz) the figure is minutes, about 400 s for a 16,512-slot frame by arithmetic, as long as the blank takes on the wire.
///
/// The refresh thread submits the blank itself as it leaves its loop, as deep as the whole DMA ring (`xferBufCount` buffers of `xferBufSize` bytes), so on a kernel whose submit returns once queued the blank returns only after every real chunk has gone out. It skips the blank when its last frame submit failed, and the stop then waits for none.
///
/// The deadline therefore covers the one call that can block for good, and the closing thread makes no submit. When the thread came back and its blank went out, the backend waits out the blank's wire time and then stops the SM with firmware-message calls: disable, dark word on the pins, clear the FIFOs, remove the program.
///
/// When the thread has not returned by the deadline, a submit is stuck in the kernel: the backend stops at once, skipping the wait, and shutdown reports it (wedged, shutdownError), keeps the buffers and the backend, which that thread reads while it is inside, and refuses a new open until a later shutdown finds the thread gone.
///
/// The report says what the stop did to the panel: dark when the disable and the pin write succeeded, with the backend's note such as a FIFO that did not drain, else that the stop failed and the panel may hold its last row (hub75_pio_backend.h, Stop). A clean shutdown reports them the same way: it returns true and `shutdownError` holds the text, which the seam publishes as the close error.
///
/// A backend whose stop failed retries it in the next shutdown until it has succeeded once: a retry that works replaces the failure with the dark report or clears it, and one that fails again names that failure. A later shutdown frees everything once the stuck thread has returned, as on kernels whose submits time out (rpi-6.6.y, rpi-6.12.y and the others that wait for delivery).
///
/// On kernels that queue (6.18.39 and later) a thread that returns right at the deadline can submit its blank into an SM about to be disabled, and that submit then waits for good once the SM stops draining. A stop on the wedged path is terminal there until the process exits (hub75_pio_backend.h, Submit semantics).
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

    /// Joins the encode thread, then the refresh thread within a deadline, stops the backend and frees the buffers; false while that thread is stuck, buffers kept (@xref{shutdown|shutdown}); `shutdownError()` holds a failed stop or note. Idempotent.
    bool shutdown() override;

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
    uint64_t bufferBytes() const override;
    void allocate() override;
    bool startBackend(bool useStubBackend, std::string& error) override;
    bool encodeLane(const uint8_t* rgb, int lane, uint16_t* slotWords, uint8_t brightness) override;
    void repack(uint32_t* out) override;
    void refreshLoop() override;
    void resetExtras() override { blankSent_.store(false); }
    /// Sets the shutdown report from this stop's result: the wedge's reason when `joined` is false, then what the stop did to the panel (dark, or failed), with `said`, the backend's note or failure; a clean shutdown reports a failed stop or a note only.
    void reportStop(bool joined, bool stopped, const std::string& said);

    size_t rowWords_ = 0;        // slots per (plane, row) group: the pass, hold words included
    uint32_t pixelClockActualHz_ = 0;
    uint16_t clkdivInt_ = 1;
    uint8_t clkdivFrac_ = 0;
    uint32_t xferBufSize_ = 0;    // the kernel DMA bounce buffer size, bytes
    uint32_t xferBufCount_ = 0;   // the kernel DMA bounce buffers

    std::string wedgeReason_;   // why the join deadline passed, kept apart from the report so each stop rebuilds the report from it
    std::atomic<bool> blankSent_{false};   // the refresh thread's blank went out, so the stop waits for it; the thread writes it before it ends

    RepackTables tables_{};

    std::unique_ptr<PioBackend> backend_;
};

}  // namespace mm::platform
