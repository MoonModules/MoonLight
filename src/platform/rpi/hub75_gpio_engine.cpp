// SPDX-License-Identifier: GPL-3.0-or-later

/// The GPIO engine's open steps, shutdown, encode pacing, refresh loop and encode, documented in hub75_gpio_engine.h.
///
/// The Stripe mapping and the driven geometry come from hub75_gpio_multiplex.h, which credits hzeller's StripeMultiplexMapper and carries the license caveat; the option rules are in hub75_gpio_options.h.
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Realtime scheduling
///
/// What is asked and what a failed request costs: hub75_gpio_engine.h, Realtime scheduling.
///
/// Priority 99 and the last-CPU pin mirror the reference design's thread setup (priority 99, pinned to CPU 3, isolcpus recommended).
///
/// ## Mapped panels
///
/// A mapped panel (Stripe) is driven as half the rows and double the columns (hub75_gpio_multiplex.h), so each logical row lands on its driven row at its mapped column offset, panel by panel along the lane.
///
/// The copy goes through the mapping table, so a further mapping adds no branch. Two logical rows share a driven row on opposite halves, so the copies do not overlap.
///
/// The fast path keeps normal panels (entry 0, the identity map) at one memcpy.

// _GNU_SOURCE first: sched_setaffinity / CPU_SET need it, and the define must precede every system header this TU includes.
#define _GNU_SOURCE 1

#include "platform/rpi/hub75_gpio_engine.h"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

#include "platform/Hub75Encode.h"   // hub75Encode: the in-tree encoder
#include "platform/rpi/hub75_gpio_dither.h"   // hub75DitherScale: the brightness and its carried fraction
#include "platform/rpi/hub75_gpio_multiplex.h"
#include "platform/rpi/hub75_gpio_pwm.h"   // hub75GpioPwmTiming: the actual LSB time

namespace mm::platform {

namespace {

// The dumps in flight a stall is measured in: a dump longer than this many frames' wire time is a stall, with the two rate windows as the floor.
constexpr uint64_t kStallFrames = 4;

// The dumps the join deadline allows: the one the thread may be inside, then the margin for the scheduler.
constexpr uint64_t kJoinFrames = 3;

// The longest open() waits for the refresh thread to report its scheduling answer, in milliseconds: far above a thread's start, far below a visible delay.
constexpr int kRealtimeWaitMs = 100;

}  // namespace

void Hub75GpioEngine::requestRealtime() {
#ifdef __linux__
    constexpr int kRealtimePriority = 99;   // SCHED_FIFO's highest, the reference design's own
    sched_param param{};
    param.sched_priority = kRealtimePriority;
    if (::sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        realtimeState_.store(kRealtimeFailed);
        return;
    }
    const long ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 1) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(static_cast<int>(ncpu - 1), &set);
        (void)::sched_setaffinity(0, sizeof(set), &set);   // best effort
    }
    realtimeState_.store(kRealtimeHeld);
#else
    realtimeState_.store(kRealtimeFailed);   // no realtime scheduling off Linux; the engine runs on regardless
#endif
}

bool Hub75GpioEngine::admit(const Hub75GpioOptions& o, std::string& error) {
    return hub75GpioCheckOptions(o, error);
}

void Hub75GpioEngine::sizeTheWall() {
    lanes_ = options_.parallel;
    laneWidth_ = static_cast<int>(options_.cols) * options_.panelsPerLane;
    laneHeight_ = options_.rows;
    // The driven lane is the logical one, except under Stripe, where the panel is driven as half the rows and double the columns (same pixel count, reshaped); the published frame stays logical, the encoder, submit and prediction read the driven size.
    drivenWidth_ = static_cast<int>(hub75GpioDrivenWidth(options_));   // the options' rule held it to 16 bits
    drivenHeight_ = hub75GpioDrivenHeight(options_);
    virtWidth_ = laneWidth_;
    virtHeight_ = laneHeight_ * lanes_;

    const Hub75Geometry geo = hub75LaneGeometry(options_, drivenWidth_, drivenHeight_, 255);
    frameSlots_ = geo.frameSlots();
    groupWords_ = static_cast<size_t>(drivenWidth_) * (geo.rowsPerScan() / 2);
}

uint64_t Hub75GpioEngine::bufferBytes() const { return hub75GpioBufferBytes(options_); }

void Hub75GpioEngine::allocate() {
    buildGpioRepackTables(tables_);

    rgb_.reset(static_cast<size_t>(virtWidth_) * virtHeight_ * 3);
    // The strip the encoder reads is the driven one, the same pixel count as the logical strip under Stripe, reshaped.
    strip_.resize(static_cast<size_t>(drivenWidth_) * drivenHeight_ * 3);
    for (int c = 0; c < lanes_; c++) {
        slots_[c].resize(frameSlots_);
        remainder_[c].resize(strip_.size());
        hub75DitherSeed(remainder_[c], static_cast<size_t>(drivenWidth_), encodedCarryBits_);
    }
    hub75DitherTable(steps_, encodedBrightness_, options_.bitDepth, encodedCarryBits_);
    encoded_.reset(frameSlots_);
    blank_.assign(frameSlots_, 0);   // all color bits clear: the OE pulses show black
}

bool Hub75GpioEngine::startBackend(bool useStubBackend, std::string& error) {
    backend_.reset(createGpioBackend(useStubBackend));
    if (!backend_) {
        error = gpioBackendRefusal();   // no hardware backend in this build, or the real backend is not allowed
        return false;
    }
    if (!backend_->open(error)) {
        backend_.reset();
        return false;
    }
    const Hub75Geometry geo = hub75LaneGeometry(options_, drivenWidth_, drivenHeight_, 255);
    GpioSubmitConfig config;
    config.laneWidth = static_cast<uint16_t>(drivenWidth_);
    config.scanRows = options_.scanRate;   // the driven scan: rows/4 under Stripe
    config.pairs = geo.rowsPerScan() / 2;
    config.holdWords = static_cast<uint16_t>(geo.holdWords());   // the dump skips them: no color, and the pulser sets the lit time
    config.bitDepth = options_.bitDepth;
    config.lsbNs = options_.pwmLsbNs;
    config.slowdown = options_.slowdown;
    config.clockFallingEdge = options_.clockFallingEdge;
    if (!backend_->start(config, error)) {
        backend_->close();
        backend_.reset();
        return false;
    }
    lsbActualNs_ = hub75GpioPwmTiming(options_.pwmLsbNs, backend_->plldHz()).lsbActualNs;
    // The prediction uses the resolved slowdown, so a Pi 4 on auto quotes its slowdown-2 rate and a Pi 3 its slowdown-1 rate.
    columnTimeNs_ = hub75GpioColumnTimeNs(backend_->resolvedSlowdown());
    predictedFrameNs_ = hub75GpioPredictedFrameNs(
        options_.scanRate, static_cast<uint32_t>(groupWords_), options_.bitDepth,
        options_.pwmLsbNs, columnTimeNs_);
    // A dump that takes longer than a few frames' wire time is a stall, and the join deadline covers the dump the thread may be inside.
    const uint64_t frameMs = (predictedFrameNs_ + 999'999) / 1'000'000;
    stallMs_ = std::max<uint64_t>(stallFloorMs_, kStallFrames * frameMs);
    sizeJoinDeadline(kJoinFrames * frameMs);
    return true;
}

void Hub75GpioEngine::resetExtras() {
    pulseTimeouts_ = 0;
    lastSubmitUs_ = 0;
    realtimeState_.store(kRealtimeUnknown);
}

void Hub75GpioEngine::releaseExtras() {
    std::vector<uint8_t>().swap(strip_);
    for (auto& lane : remainder_) std::vector<uint8_t>().swap(lane);
}

size_t Hub75GpioEngine::extrasHeapBytes() const {
    size_t bytes = strip_.capacity();
    for (const auto& lane : remainder_) bytes += lane.capacity();
    return bytes;
}

bool Hub75GpioEngine::shutdown() {
    if (!running_ && threadsJoined() && !backend_) return true;   // a fresh engine, or one already shut down
    const bool joined = stopThreads();
    if (!joined) return false;   // the stuck thread still reads the buffers and the backend
    if (backend_) {
        // Blank-first: the backend dumps the blank frame, waits for it to drain, muxes OE back to a GPIO output driving high, then stops the PWM, panel dark; a failure is counted and reported with the close.
        std::string error;
        if (!backend_->blankAndStop(blank_.data(), blank_.size(), error)) {
            submitErrors_++;
            shutdownError_ = error;
        }
        backend_->close();
        backend_.reset();
    }
    releaseBuffers();
    return true;
}

// Whether the carry moves a fraction: its bits are on and the codes lose one, at brightness 1 to 254, or from 1 below 8 planes.
bool Hub75GpioEngine::carryMoves(uint8_t brightness, uint8_t bits) const {
    return bits > 0 && brightness > 0 && (brightness < 255 || options_.bitDepth < 8);
}

// Takes the brightness and carry bits the seam last set, reseeding the remainders and rebuilding the table on a change, and says whether either changed.
bool Hub75GpioEngine::adoptCorrection() {
    const uint8_t brightness = brightness_.load(std::memory_order_relaxed);
    const uint8_t bits = carryBits_.load(std::memory_order_relaxed);
    const bool bitsChanged = bits != encodedCarryBits_;
    const bool changed = bitsChanged || brightness != encodedBrightness_;
    if (bitsChanged) {
        for (int c = 0; c < lanes_; c++) hub75DitherSeed(remainder_[c], static_cast<size_t>(drivenWidth_), bits);
    }
    if (changed) hub75DitherTable(steps_, brightness, options_.bitDepth, bits);
    encodedBrightness_ = brightness;
    encodedCarryBits_ = bits;
    return changed;
}

// While the carry moves a fraction the codes change every refresh, so the held frame re-encodes once the refresh thread has taken the last one; a change of brightness or bits re-encodes it once at once, and otherwise the frame re-submits.
bool Hub75GpioEngine::encodeDue(bool fresh) {
    const bool changed = adoptCorrection();
    return fresh || changed || (carryMoves(encodedBrightness_, encodedCarryBits_) && !encoded_.pending());
}

void Hub75GpioEngine::threadsStarted() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRealtimeWaitMs);
    while (!realtimeAnswered() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
}

void Hub75GpioEngine::refreshLoop() {
    requestRealtime();
    RefreshWindow window;
    while (!stop_.load()) {
        // Take the newest encoded frame if the encode thread published one since the last take, else dump the current frame again, as when the carry's next encode is late.
        encoded_.take();
        const auto t0 = std::chrono::steady_clock::now();
        submitEnteredMs_.store(hub75NowMs());   // a dump that does not return shows as a stall, not as a frozen rate
        const int rc = backend_->submitFrame(encoded_.current(), frameSlots_);
        submitEnteredMs_.store(0);
        lastSubmitUs_ = hub75MicrosSince(t0);
        const uint64_t timeouts = backend_->pulseTimeouts();
        // A frame an OE pulse timed out in showed degraded, so it counts as failed and the status line shows it.
        const bool timedOut = timeouts != pulseTimeouts_.exchange(timeouts);
        if (rc == 0 && !timedOut) {
            framesSubmitted_++;
            window.count(fps_);
        } else {
            submitErrors_++;
            window.count(fps_, false);
        }
        // Don't spin on a wedged backend: let the window see the miss so the rate falls to 0, pause briefly, retry.
        if (rc != 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void Hub75GpioEngine::repack(uint32_t* out) {
    repackSlotsToGpio(hub75LaneSlots(slots_, lanes_).data(), lanes_, frameSlots_, tables_, out);
}

bool Hub75GpioEngine::encodeLane(const uint8_t* rgb, int lane, uint16_t* slotWords, uint8_t /*brightness*/) {
    // Lanes stack vertically and span the full canvas width, so a lane's strip is one contiguous run of the virtual frame (one memcpy, no per-pixel walk) and takes no gamma: MoonLight's correction applies the curve before the frame reaches the seam.
    const size_t stripBytes = static_cast<size_t>(laneWidth_) * laneHeight_ * 3;
    const uint8_t* src = rgb + static_cast<size_t>(lane) * stripBytes;
    if (options_.multiplexing == kHub75MultiplexNormal) {
        std::memcpy(strip_.data(), src, stripBytes);
    } else {
        copyMappedPanels(src);
    }
    // The codes carry the brightness (the table of encodeDue, not the per-frame level the base reads), so the encoder lights every plane in full.
    const Hub75Geometry geo = hub75LaneGeometry(options_, drivenWidth_, drivenHeight_, 255);
    hub75DitherScale(strip_, remainder_[lane], steps_, options_.bitDepth, encodedCarryBits_);   // at 255 with the carry off, the scale is plain truncation
    // Default layout (the repack assumes it; see hub75_gpio_repack.h).
    const size_t bytes = hub75Encode(
        strip_.data(), reinterpret_cast<uint8_t*>(slotWords), geo);
    return bytes == geo.frameBytes();
}

void Hub75GpioEngine::copyMappedPanels(const uint8_t* src) {
    // A mapped panel (Stripe) is copied row by row through the mapping table (@xref{mapped-panels|Technical → Mapped panels}).
    const size_t panelBytes = static_cast<size_t>(options_.cols) * 3;
    const size_t drivenRowBytes = static_cast<size_t>(drivenWidth_) * 3;
    // The driven columns a logical column takes: the table entry's stretch, so a further mapping changes no line here.
    const size_t stretch = hub75GpioMultiplexEntry(options_.multiplexing).stretch;
    for (int r = 0; r < laneHeight_; r++) {
        // The mapped offset is the mapped x of column 0: 0, or cols where the row folds to the driven row's second half.
        const Hub75MultiplexPos p = hub75GpioMultiplexMap(options_.multiplexing, 0, static_cast<uint8_t>(r), options_.rows, options_.cols);
        for (int panel = 0; panel < options_.panelsPerLane; panel++) {
            std::memcpy(strip_.data() + static_cast<size_t>(p.y) * drivenRowBytes +
                            (static_cast<size_t>(panel) * stretch * options_.cols + p.x) * 3,
                        src + static_cast<size_t>(r) * laneWidth_ * 3 +
                            static_cast<size_t>(panel) * panelBytes,
                        panelBytes);
        }
    }
}

}  // namespace mm::platform
