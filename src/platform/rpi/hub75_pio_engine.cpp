// SPDX-License-Identifier: GPL-3.0-or-later

/// The PIO engine's option check, open steps, shutdown, refresh loop and encode, documented in hub75_pio_engine.h.
///
/// @technical
///
/// ## Bounce buffers
///
/// The kernel PIO driver offers at most 4 DMA bounce buffers. 64 KiB x 4 covers a 256 KiB frame with a buffer per chunk, so every chunk finds a free buffer, and smaller frames use fewer, smaller chunks.
///
/// At 8 bits the wall frame is exactly 256 KiB, so no spare buffer is needed: a transfer waits for a free buffer only when all four are outstanding, so a 4-chunk frame takes a distinct buffer per chunk and does not wait, and buffer reuse starts at a 5th chunk.
///
/// This is observed on the bench, and the bench's `shown` is still the empirical check.

#include "platform/rpi/hub75_pio_engine.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <thread>

#include "platform/Hub75Encode.h"   // hub75Encode: the in-tree encoder
#include "platform/rpi/hub75_pio_pins.h"

// The pioasm-generated program data: this file defines PICO_NO_HARDWARE, so the header provides the instruction arrays and wrap defines only, and no piolib types are needed here.
#define PICO_NO_HARDWARE 1
#include "platform/rpi/hub75.pio.h"
#undef PICO_NO_HARDWARE

// One program length for both clock edges: the backend loads the whole instruction array.
static_assert(std::size(hub75_wide_program_instructions) == std::size(hub75_wide_falling_program_instructions),
              "the rising and falling programs are the same length");

namespace mm::platform {

namespace {

// Kernel DMA bounce-buffer sizing: at most 4 buffers of 64 KiB (@xref{bounce-buffers|Technical → Bounce buffers}).
constexpr uint32_t kXferBufSize = 65536;
constexpr uint32_t kXferBufCount = 4;

}  // namespace

bool Hub75PioEngine::admit(const Hub75PioOptions& o, std::string& error) {
    if (running_) {
        error = "PIO engine already running";
        return false;
    }
    if (wedged_) {
        error = shutdownError_;   // the last refresh thread is still in the kernel
        return false;
    }
    return hub75PioCheckOptions(o, error);
}

void Hub75PioEngine::sizeTheWall() {
    lanes_ = options_.parallel;
    laneWidth_ = static_cast<int>(options_.cols) * options_.panelsPerLane;
    laneHeight_ = options_.rows;
    virtWidth_ = laneWidth_;
    virtHeight_ = laneHeight_ * lanes_;

    const Hub75Geometry geo = hub75LaneGeometry(options_, laneWidth_, laneHeight_, 255);
    frameSlots_ = geo.frameSlots();
    // The latch blanking walks the frame a row group at a time, and the group is the pass (shift words plus the hold words a narrow row leads with), so a stretched frame blanks the words before its own latch, not the next group's.
    rowWords_ = geo.passWords();

    const PioClockDivider divider = pioClockDivider(options_.pixelClockHz);
    clkdivInt_ = divider.intPart;
    clkdivFrac_ = divider.fracPart;
    pixelClockActualHz_ = divider.actualHz;

    // The DMA ring the backend sets up: at most 4 bounce buffers, each at most 64 KiB and no bigger than a frame.
    xferBufSize_ = std::min(kXferBufSize, static_cast<uint32_t>(frameSlots_ * kBytesPerPixelClock));
    xferBufCount_ = kXferBufCount;

    // A submit that takes longer than a few frames' wire time is a stall; the floor is two rate windows, for a frame too short to matter.
    const uint64_t wireMs = pixelClockActualHz_ ? static_cast<uint64_t>(frameSlots_) * 1000 / pixelClockActualHz_ : 0;
    stallMs_ = std::max<uint64_t>(stallFloorMs_, 4 * wireMs);

    buildRepackTables(tables_);
}

void Hub75PioEngine::allocate() {
    rgb_.reset(static_cast<size_t>(virtWidth_) * virtHeight_ * 3);
    for (int c = 0; c < lanes_; c++)
        slots_[c].resize(frameSlots_);
    encoded_.reset(frameSlots_);
    // The blank is as deep as the whole DMA ring, so its submit returns only after every real chunk has gone out (hub75_pio_engine.h, Shutdown).
    blank_.assign(static_cast<size_t>(xferBufCount_) * xferBufSize_ / kBytesPerPixelClock, kBlankWideWord);
}

bool Hub75PioEngine::startBackend(bool useStubBackend, std::string& error) {
    backend_.reset(createPioBackend(useStubBackend));
    if (!backend_) {
        error = pioBackendRefusal();   // no piolib in this build, or the real backend is not allowed
        return false;
    }
    if (!backend_->open(error)) {
        backend_.reset();
        return false;
    }
    PioStartConfig config;
    config.programInstructions = options_.clockFallingEdge ? hub75_wide_falling_program_instructions
                                                           : hub75_wide_program_instructions;
    config.wrapTarget = options_.clockFallingEdge ? hub75_wide_falling_wrap_target : hub75_wide_wrap_target;
    config.wrap = options_.clockFallingEdge ? hub75_wide_falling_wrap : hub75_wide_wrap;
    config.programLength = static_cast<int>(std::size(hub75_wide_program_instructions));
    config.clkdivInt = clkdivInt_;
    config.clkdivFrac = clkdivFrac_;
    config.xferBufSize = xferBufSize_;
    config.xferBufCount = xferBufCount_;
    if (!backend_->start(config, error)) {
        backend_->close();
        backend_.reset();
        return false;
    }
    return true;
}

void Hub75PioEngine::dropBackend() {
    backend_->close();
    backend_.reset();
}

bool Hub75PioEngine::shutdown() {
    if (!running_ && !backend_ && threadsJoined()) return true;
    // Both threads first: the blank goes out only when nothing else submits, and nothing reads a buffer once it is freed.
    if (!stopThreads()) return false;
    if (backend_) {
        // Blank-first: the backend submits the ring-deep blank, waits for the FIFO to drain, then disables the SM, leaving the panel dark; the seam's close returns void, so a failure is counted, not printed.
        std::string error;
        if (!backend_->blankAndStop(blank_.data(),
                                    static_cast<uint32_t>(blank_.size() * 4),
                                    error)) {
            submitErrors_++;
        }
        backend_->close();
        backend_.reset();
    }
    releaseBuffers();
    return true;
}

void Hub75PioEngine::refreshLoop() {
    RefreshWindow window;
    while (!stop_.load()) {
        // Take the newest encoded frame if the encode thread published one since the last take, else submit the current frame again.
        encoded_.take();
        submitEnteredMs_.store(hub75NowMs());   // a submit that never returns shows as a stall, not as a frozen rate
        const int rc = backend_->submitFrame(encoded_.current(), static_cast<uint32_t>(frameSlots_ * kBytesPerPixelClock));
        submitEnteredMs_.store(0);
        if (rc == 0) {
            framesSubmitted_++;
            window.count(fps_);
        } else {
            // Count the failure, let the window see the miss so the rate falls to 0, pause and retry: the ioctl's error (-EINTR, -EIO, or -ETIMEDOUT on 6.12) lands here, and a submit that never returns is the shutdown deadline's case.
            submitErrors_++;
            window.count(fps_, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void Hub75PioEngine::repack(uint32_t* out) {
    repackSlots(hub75LaneSlots(slots_, lanes_).data(), lanes_, frameSlots_, tables_, out);
    if (options_.latchBlankWords > 0)
        applyLatchBlanking(out, frameSlots_, rowWords_, options_.latchBlankWords);
}

bool Hub75PioEngine::encodeLane(const uint8_t* rgb, int lane, uint16_t* slotWords, uint8_t brightness) {
    // Lanes stack vertically and span the full canvas width, so a lane's strip is one contiguous run of the virtual frame, encoded in place, and takes no gamma: MoonLight's correction applies the curve before the frame reaches the seam.
    const size_t stripBytes = static_cast<size_t>(laneWidth_) * laneHeight_ * 3;
    const Hub75Geometry geo = hub75LaneGeometry(options_, laneWidth_, laneHeight_, brightness);
    // Default layout (the repack assumes it; see hub75_pio_repack.h).
    const size_t bytes = hub75Encode(
        rgb + static_cast<size_t>(lane) * stripBytes, reinterpret_cast<uint8_t*>(slotWords), geo);
    return bytes == geo.frameBytes();
}

}  // namespace mm::platform
