// SPDX-License-Identifier: GPL-3.0-or-later

/// The HUB75 GPIO engine: MoonLight RGB frames encoded into bitplane GPIO data words and clocked out to the panels of a Pi 3/4 through a `GpioBackend`, with the hardware PWM pulser timing OE.
///
/// The render thread publishes RGB with publishRgb(), an encode thread encodes the newest frame and a refresh thread dumps the newest encoded frame (@xref{threading|threading}); the frame's path is in @xref{pipeline|pipeline}, and the carry's re-encodes follow @xref{encode-due|encode due}.
///
/// The engine drives the columns from the CPU rather than through DMA (why: hub75_gpio_backend.h, Why no DMA), and the refresh thread asks for realtime scheduling (@xref{realtime-scheduling|realtime scheduling}).
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Pipeline
///
/// One frame travels these stages:
///
/// ```
/// RGB virtual frame, published by the GPIO output through the seam
///   -> triple-buffered RGB slot (lock-free publish, newest wins)
///   -> encode thread: per lane, one contiguous strip copy out of the
///      virtual frame (lanes stack vertically, so each strip is one
///      memcpy; NO gamma: MoonLight's correction already applied it).
///      Under Stripe the strip is remapped first: the logical panel is
///      reshaped into the driven panel (half the rows, double the
///      columns, hub75_gpio_multiplex.h), row by row, panel by panel
///      along the lane; the normal path keeps its single memcpy.
///   -> scale by the brightness, dithered in time (hub75_gpio_dither.h)
///   -> MoonLight hub75Encode() into 16-bit slot words (bitplanes, with
///      address / latch / OE baked in, one word per pixel clock)
///   -> repack: three lanes' slot words -> one 32-bit GPIO data word
///      each (color bits only; the address / latch / OE bits are
///      stripped: the dump loop and the PWM pulser own those lines)
///   -> triple-buffered encoded frame (lock-free publish, newest wins)
///   -> refresh thread, GpioBackend::submitFrame: clocks the GPIO
///      words out through GPSET0/GPCLR0, three writes a column, rows
///      outer and planes inner (hub75_gpio_dump.h), while the PWM
///      pulser times OE.
/// ```
///
/// ## Threading
///
/// The threads, the triple buffers and the shutdown are the base's (hub75_rpi_common.h, Threads). The refresh thread dumps the newest encoded frame, again and again, so the panel keeps refreshing at the dump rate whatever the render rate.
///
/// ## Encode due
///
/// The encode thread encodes when a frame is fresh, when the brightness or the carry bits changed, and, while the carry moves a fraction, once the refresh thread has taken the last encoded frame. That pacing keeps one encode to at most one refresh, so the pattern's period counts encodes, and keeps the encode thread from re-encoding faster than the panel shows.
///
/// The carry moves a fraction while its bits are on and the brightness is above 0, and the codes lose a fraction: the brightness is under 255, or the depth is under 8 planes.
///
/// A refresh that finds no new encoded frame, because the encode is late, dumps the held frame again.
///
/// ## Realtime scheduling
///
/// The refresh thread asks for realtime scheduling (SCHED_FIFO, priority 99, pinned to the last CPU) at loop start, best effort: scheduling jitter lands in the column timing as visible tearing when the request fails.
///
/// The request needs root or a realtime priority limit (LimitRTPRIO, set in the hub75-gpio.conf drop-in of installing-on-linux.md); when it fails the engine runs on regardless and the seam reports it as a warning.
///
/// ## Open and shutdown
///
/// `useStubBackend` is the engine test's dry run: production always passes false, and a host that is no Pi 3 or 4 fails open() with that reason.
///
/// shutdown() is blank-first: it joins the threads, dumps a blank frame, waits for it to drain, muxes OE back to a GPIO output driving high (dark), stops the PWM and releases everything. A refresh thread that stays inside a dump past the join deadline is reported stuck, and the backend and the buffers stay held for it.

#pragma once

#include <algorithm>   // std::min
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "platform/Hub75Bus.h"   // Hub75Geometry: the per-lane encode contract
#include "platform/Hub75Gpio.h"   // Hub75GpioOptions
#include "platform/nonblocking.h"   // MM_NONBLOCKING: setCorrection is tick-safe
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_options.h"   // hub75GpioCheckOptions: the rules open() refuses on
#include "platform/rpi/hub75_gpio_repack.h"
#include "platform/rpi/hub75_rpi_common.h"   // Hub75EngineBase: the open sequence and the threads both engines share

namespace mm::platform {

/// The engine: owns the RGB and encoded triple buffers, the GPIO backend (which owns the PWM pulser) and the encode and refresh threads; its open sequence is in hub75_rpi_common.h, and `useStubBackend` runs the stub (@xref{open-and-shutdown|open and shutdown}).
class Hub75GpioEngine final : public Hub75EngineBase<Hub75GpioOptions> {
  public:
    /// An engine that is not running.
    Hub75GpioEngine() = default;
    /// Shuts the engine down, joins a refresh thread stuck in a dump with no deadline and no detach, then shuts down again; the one production engine lives as long as the process.
    ~Hub75GpioEngine() {
        if (!shutdown() && refreshThread_.joinable()) {
            refreshThread_.join();
            shutdown();   // the one teardown again, now that the thread is gone
        }
    }

    /// Blank-first shutdown, idempotent and safe after a failed open(); false while the refresh thread is stuck, buffers kept; a second open() re-acquires the hardware from scratch (@xref{open-and-shutdown|open and shutdown}).
    bool shutdown() override;

    /// The brightness the engine scales the codes by (0..255), and the fraction bits the dither carries over the refreshes, 0 off, clamped to kHub75GpioCarryBitsMax; both hold across opens.
    void setCorrection(uint8_t brightness, uint8_t carryBits) MM_NONBLOCKING {
        brightness_.store(brightness, std::memory_order_relaxed);
        carryBits_.store(std::min(carryBits, kHub75GpioCarryBitsMax), std::memory_order_relaxed);
    }

    /// The driven lane's width: the logical lane under normal, double the columns under Stripe.
    int drivenWidth() const { return drivenWidth_; }
    /// The driven lane's height: the logical lane under normal, half the rows under Stripe (the same pixel count).
    int drivenHeight() const { return drivenHeight_; }
    /// The column time the prediction assumes, in ns, after slowdown resolution.
    uint16_t columnTimeNs() const { return columnTimeNs_; }
    /// The LSB time the PWM produces in ns, after divider quantization.
    uint16_t lsbActualNs() const { return lsbActualNs_; }
    /// Predicted refresh rate in Hz, computed from the column-time model (hub75GpioPredictedFrameNs).
    float refreshHz() const {
        const uint64_t ns = predictedFrameNs_;
        return ns ? 1e9f / static_cast<float>(ns) : 0.0f;
    }
    /// Whether the refresh thread's realtime request failed; false until the thread reports, which open() waits for.
    bool realtimeFailed() const { return realtimeState_.load() == kRealtimeFailed; }
    /// Whether the refresh thread has reported its realtime answer, which open() waits for, bounded.
    bool realtimeAnswered() const { return realtimeState_.load() != kRealtimeUnknown; }
    /// The backend's name, "(none)" before open().
    const char* backendName() const {
        return backend_ ? backend_->name() : "(none)";
    }

    /// OE pulses that timed out.
    uint64_t pulseTimeouts() const { return pulseTimeouts_.load(); }
    /// Last submit duration in microseconds: the dump time of one frame, the number the lab reads to check the column-time model.
    uint64_t lastSubmitUs() const { return lastSubmitUs_.load(); }

  private:
    bool admit(const Hub75GpioOptions& o, std::string& error) override;
    void sizeTheWall() override;
    uint64_t bufferBytes() const override;
    void allocate() override;
    bool startBackend(bool useStubBackend, std::string& error) override;
    void resetExtras() override;
    void releaseExtras() override;
    size_t extrasHeapBytes() const override;
    bool encodeDue(bool fresh) override;
    /// Takes the brightness and carry bits the seam last set, reseeding the remainders and rebuilding the table on a change; true when either changed.
    bool adoptCorrection();
    /// Whether the carry moves a fraction at `brightness` and `bits`, so each refresh re-encodes.
    bool carryMoves(uint8_t brightness, uint8_t bits) const;
    void threadsStarted() override;
    bool encodeLane(const uint8_t* rgb, int lane, uint16_t* slotWords, uint8_t brightness) override;
    void repack(uint32_t* out) override;
    void refreshLoop() override;
    /// Copies a lane's logical strip at `src` into strip_ row by row through the Stripe mapping table.
    void copyMappedPanels(const uint8_t* src);

    /// Asks for realtime scheduling on the calling (refresh) thread, best effort, and records the answer (@xref{realtime-scheduling|realtime scheduling}).
    void requestRealtime();

    /// The realtime request's states: unknown until the thread reports, then held or refused.
    enum RealtimeState : int { kRealtimeUnknown, kRealtimeHeld, kRealtimeFailed };

    int drivenWidth_ = 0;   // one lane's driven width: laneWidth_, doubled under Stripe
    int drivenHeight_ = 0;  // one lane's driven height: laneHeight_, halved under Stripe
    size_t groupWords_ = 0;      // slots per (plane, row) group
    uint16_t columnTimeNs_ = 0;
    uint16_t lsbActualNs_ = 0;
    uint64_t predictedFrameNs_ = 0;

    GpioRepackTables tables_{};

    std::vector<uint8_t> strip_;                   // one lane's driven strip, RGB24 (the encode thread's)
    std::vector<uint8_t> remainder_[kHub75Lanes];  // per lane, per channel: the fraction of a level carried to the next refresh (the encode thread's)
    std::array<uint16_t, 256> steps_{};            // the fractions of a level each code holds at the encoded brightness and bits (the encode thread's; open() sets it before the thread starts)

    std::unique_ptr<GpioBackend> backend_;
    std::atomic<uint8_t> carryBits_{kHub75GpioCarryBitsDefault};
    uint8_t encodedBrightness_ = 255;   // the brightness the live frame was encoded at (the encode thread's; open() reads it before the thread starts)
    uint8_t encodedCarryBits_ = kHub75GpioCarryBitsDefault;   // the carry bits the live frame was encoded at (the encode thread's; open() reads it before the thread starts)
    std::atomic<int> realtimeState_{kRealtimeUnknown};

    std::atomic<uint64_t> pulseTimeouts_{0};
    std::atomic<uint64_t> lastSubmitUs_{0};
};

}  // namespace mm::platform
