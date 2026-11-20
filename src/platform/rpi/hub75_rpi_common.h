// SPDX-License-Identifier: GPL-3.0-or-later

/// What the Pi 5 PIO engine and the Pi 3/4 GPIO engine share: the engine base with its open sequence and its two threads, the option-rule table, the lane geometry, the triple buffers, the refresh-rate window and the repack's default-layout constants and color scatter.
///
/// The pin numbers both engines use are in hub75_board_pins.h, and this header is MoonLight's own shared code (@xref{open-sequence|open sequence}, @xref{threads|threads}, @xref{option-rules|option rules}, @xref{triple-buffer|triple buffer}, @xref{default-layout|default layout}).
///
/// @technical
///
/// ## Open sequence
///
/// open() is one sequence in the base, and each engine supplies the steps as protected virtuals (a template method): check the options, size the wall, allocate, encode the black RGB frame so the first submit has a frame, start the backend, then start the two threads. A failed step leaves the engine stopped, its reason in `error` and its buffers freed; a failed thread start ends in shutdown.
///
/// The option rules hold the wall's ceiling: a wall whose buffers (`bufferBytes`) total more than the limit is refused before anything is allocated (hub75_pio_options.h). A `std::bad_alloc` from the allocation, on a machine that is busy, frees what was taken and refuses with a reason naming the need in MB.
///
/// The base is a template over `Options` because the two engines take different option structs, which the base stores and passes. The hooks run at open, once per lane (encodeLane) and once per frame (repack), and the per-word and per-row loops stay inside them.
///
/// ## Threads
///
/// The render thread publishes an RGB frame and returns at once. The encode thread takes the newest RGB frame, encodes it into the encoded buffer it owns and publishes that buffer as the newest encoded frame. The refresh thread takes the newest encoded frame, if there is one, and submits the frame it holds, again and again, so the panel keeps refreshing at the pixel clock whatever the render rate.
///
/// The encode and the submit therefore each run at their own pace. The encode thread owns `framesEncoded` and `lastEncodeUs`, the refresh thread owns `framesSubmitted`, `submitErrors` and the rate window, and the brightness is read once per frame, so every lane of a frame is lit alike.
///
/// A frame whose encode fails is dropped and the last good frame keeps submitting: a stale picture beats a torn one.
///
/// The refresh thread blocks the stop signals (SIGINT, SIGTERM, SIGHUP) as it starts, so a service stop lands outside every transfer and each frame keeps its progress; the main thread takes the signal and ends the run.
///
/// ## Shutdown
///
/// shutdown order is fixed: stop publishing, join the encode thread, join the refresh thread, stop the backend, free the buffers. The refresh thread blanks the panels itself as it leaves its loop, and it is joined with a deadline, because a submit blocked in the kernel for good would otherwise hang the join.
///
/// When the deadline passes the engine stops the backend at once, which darkens the panel when the stop's calls succeed, reports the thread stuck, keeps every buffer and the backend, since the stuck thread still reads them, and refuses a new open until a later shutdown finds the thread gone.
///
/// publishRgb counts itself in flight, and shutdown waits for the count to reach zero before it frees the RGB slots, so every publish writes into live memory.
///
/// ## Option rules
///
/// A check is a table of rows, each a predicate over the options and the reason it gives; the first row that holds refuses. The five rows both engines share (lanes, panels, rows and columns, scan rate, depth) are `kHub75CommonRules`, and an engine adds its own rows after them.
///
/// ## Triple buffer
///
/// Both frame handovers are one lock-free triple buffer with one producer and one consumer: `ready_` holds a slot index in its low bits and `kFresh` when the producer has filled it since the consumer last took it. The producer's slot, the ready slot and the consumer's slot are three different slots, so a publish leaves the frame being read intact.
///
/// The RGB frames use it from the render thread to the encode thread, and the encoded frames from the encode thread to the refresh thread, which is why the encoded frames take three buffers and not two: a submit may still be reading the old frame while the next one is encoded.
///
/// ## Default layout
///
/// The repack hard-codes the default Hub75Layout, and the static_assert below pins it, so a moved default fails at compile time instead of scrambling the bus through the tables.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
    #include <csignal>
    #include <pthread.h>
#endif

#include "platform/Hub75Bus.h"   // Hub75Geometry, Hub75Layout
#include "platform/rpi/hub75_board_pins.h"   // kHub75Lanes: the board's lane count
#include "platform/rpi/hub75_pio_options.h"   // the option rules, hub75LaneGeometry
#include "platform/nonblocking.h"   // MM_NONBLOCKING: the triple buffer's publish is tick-safe

// hub75Encode writes little-endian slot words and the repack reads them back as uint16_t: ARM (Pi) and x86 Linux are little-endian, a big-endian host fails to compile rather than byte-swap the bus.
static_assert(std::endian::native == std::endian::little, "hub75 repack assumes a little-endian host");

namespace mm::platform {

/// Microseconds since `start`, for the encode and submit timers.
inline uint64_t hub75MicrosSince(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

/// Copies at most `cap` words of a stub backend's `capture` into `out` and returns the count, 0 for a null `out` or an empty capture; the caller holds the capture's lock.
inline size_t hub75CopyCapturedWords(const std::vector<uint32_t>& capture, uint32_t* out, size_t cap) {
    if (!out || capture.empty()) return 0;
    const size_t n = capture.size() < cap ? capture.size() : cap;
    std::memcpy(out, capture.data(), n * 4);
    return n;
}

/// The first three lanes' slot streams as the pointer array the repack takes, lanes beyond `lanes` null (their panels stay dark).
template <typename Slots>
std::array<const uint16_t*, kHub75Lanes> hub75LaneSlots(const Slots (&slots)[kHub75Lanes], int lanes) {
    return {slots[0].data(), lanes > 1 ? slots[1].data() : nullptr, lanes > 2 ? slots[2].data() : nullptr};
}

/// Adds lanes 1 and 2's color words for slot `i` to `w`, a lane beyond `lanes` or with no stream adds no word, so its panels stay dark; it stays a step of the repack loop, as an inline function.
[[gnu::always_inline]] inline uint32_t hub75OrLaneColors(uint32_t w, const uint16_t* const laneSlots[kHub75Lanes], int lanes,
                                                         size_t i, const uint32_t (&color)[kHub75Lanes][64]) {
    if (lanes > 1 && laneSlots[1] != nullptr) w |= color[1][laneSlots[1][i] & 0x3F];
    if (lanes > 2 && laneSlots[2] != nullptr) w |= color[2][laneSlots[2][i] & 0x3F];
    return w;
}

/// One lock-free handover of frames from a producer to a consumer, newest wins: three slots, one the producer fills, one ready, one the consumer reads (@xref{triple-buffer|triple buffer}).
template <class Elem>
class TripleBuffer {
  public:
    /// Sizes the three slots to `count` elements each, zeroed, and restarts with no frame published.
    void reset(size_t count) {
        for (auto& slot : slot_) slot.assign(count, 0);
        ready_.store(0);
        writeSlot_ = 1;
        readSlot_ = 2;
        published_ = 0;
    }

    /// Frees the three slots.
    void release() {
        for (auto& slot : slot_) std::vector<Elem>().swap(slot);
        ready_.store(0);
        published_ = 0;
    }

    /// The elements one slot holds, 0 once released.
    size_t size() const { return slot_[0].size(); }

    /// The bytes the three slots hold.
    size_t heapBytes() const { return (slot_[0].capacity() + slot_[1].capacity() + slot_[2].capacity()) * sizeof(Elem); }

    /// The producer's slot, which no consumer reads: fill it, then publishWritten().
    Elem* writeSlot() { return slot_[writeSlot_].data(); }

    /// Hands the producer's slot over as the freshest and takes the slot it replaced as the next to fill; allocation-free, and a frame still waiting is superseded.
    void publishWritten() MM_NONBLOCKING {
        writeSlot_ = ready_.exchange(static_cast<uint8_t>(writeSlot_ | kFresh), std::memory_order_acq_rel) & 3;
        published_++;
    }

    /// Copies one frame of size() elements into the producer's slot and hands it over; a no-op when the slots are released.
    void publish(const Elem* frame) MM_NONBLOCKING {
        const size_t count = size();
        if (count == 0) return;
        std::memcpy(slot_[writeSlot_].data(), frame, count * sizeof(Elem));
        publishWritten();
    }

    /// Takes the freshest frame as the consumer's when the producer published one since the last take, and says whether it did.
    bool take() {
        if (!(ready_.load(std::memory_order_acquire) & kFresh)) return false;
        readSlot_ = ready_.exchange(readSlot_, std::memory_order_acq_rel) & 3;
        return true;
    }

    /// The consumer's frame, the last one taken, zeros before the first.
    const Elem* current() const { return slot_[readSlot_].data(); }

    /// Frames published since the last reset.
    uint64_t published() const { return published_.load(); }

  private:
    static constexpr uint8_t kFresh = 4;
    std::vector<Elem> slot_[3];
    std::atomic<uint8_t> ready_{0};
    uint8_t writeSlot_ = 1;   // the producer's slot
    uint8_t readSlot_ = 2;    // the consumer's slot
    std::atomic<uint64_t> published_{0};
};

/// The RGB frames between the render thread and the encode thread.
using RgbTripleBuffer = TripleBuffer<uint8_t>;

/// The refresh-rate window the refresh loop keeps: frames counted over about a second.
class RefreshWindow {
  public:
    /// Counts one submit, shown or failed; each full second stores the rate in `fps` and starts the next window, so a run of failed submits brings the rate to 0.
    void count(std::atomic<uint16_t>& fps, bool shown = true) {
        if (shown) windowCount_++;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart_);
        if (elapsed.count() < 1000) return;
        fps.store(static_cast<uint16_t>(windowCount_ * 1000 / elapsed.count()));
        windowCount_ = 0;
        windowStart_ = now;
    }

  private:
    uint32_t windowCount_ = 0;
    std::chrono::steady_clock::time_point windowStart_ = std::chrono::steady_clock::now();
};

/// Blocks SIGINT, SIGTERM and SIGHUP on the calling thread, so a stop lands outside every transfer: from 6.18.53 a handler lacking SA_RESTART makes a submit return -EINTR, and 6.18.39 to 6.18.52 cut the frame short. A no-op where signals are absent.
inline void blockStopSignals() {
#if defined(__unix__) || defined(__APPLE__)
    sigset_t stops;
    sigemptyset(&stops);
    sigaddset(&stops, SIGINT);
    sigaddset(&stops, SIGTERM);
    sigaddset(&stops, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &stops, nullptr);
#endif
}

/// Milliseconds on the steady clock, plus 1 so the value starts at 1 and 0 can mean "none". The clock read is a vDSO call, nonblocking and allocation-free, which the function-effects analyzer cannot know.
inline uint64_t hub75NowMs() MM_NONBLOCKING {
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wfunction-effects")
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wfunction-effects"
#  endif
#endif
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wfunction-effects")
#    pragma clang diagnostic pop
#  endif
#endif
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) + 1;
}

/// How long a submit may be in flight before the engine reads as stalled, in milliseconds: two rate windows.
constexpr uint64_t kHub75StallMs = 2000;

/// The least shutdown waits for the refresh thread before it reports the thread stuck, in milliseconds; a wall that takes long on the wire raises it (hub75_pio_engine.h, Shutdown).
constexpr int kHub75JoinDeadlineMs = 3000;

/// The state and the open sequence both engines share, over their options `Options`: the buffers, the two threads, the counters and the accessors that read them (@xref{open-sequence|open sequence}, @xref{threads|threads}).
template <class Options>
class Hub75EngineBase {
  public:
    /// Refuses a running or wedged engine, checks options, sizes the wall, allocates, opens the backend, starts the threads; every failure fills `error`; one after the two refusals also stops the engine and frees the buffers unless a thread is stuck.
    bool open(const Options& o, std::string& error, bool useStubBackend = false) {
        if (running_.load()) {
            error = "PIO engine already running";
            return false;
        }
        if (wedged_.load()) {
            error = shutdownError_;   // the last refresh thread is still in the kernel
            return false;
        }
        if (!admit(o, error)) return false;
        options_ = o;
        sizeTheWall();
        if (!allocateWall(error)) return false;
        if (!firstFrame(error) || !startBackend(useStubBackend, error)) {
            releaseBuffers();
            return false;
        }
        if (launchThreads(error)) return true;
        shutdown();   // the one teardown, the refresh thread joined within the deadline: one stuck in a submit leaves the engine wedged, with its backend and buffers kept
        return false;
    }

    /// Whether open() succeeded and the engine has not been shut down.
    bool isRunning() const { return running_.load(); }
    /// Publishes one RGB frame of width()*height()*3 bytes, allocation-free, superseding one still waiting; ignored while stopped, and writes only live memory (@xref{shutdown|shutdown}).
    void publishRgb(const uint8_t* rgb) MM_NONBLOCKING {
        publishing_.fetch_add(1);
        if (running_.load() && rgb != nullptr) rgb_.publish(rgb);
        publishing_.fetch_sub(1);
    }
    /// Measured refresh in Hz, from the submit counter over a ~1 s window; 0 after shutdown, and while a submit has been in flight longer than the stall time, since a hung submit counts no frame.
    uint16_t fps() const MM_NONBLOCKING { return stalled() ? 0 : fps_.load(); }
    /// Whether the refresh thread has been inside one submit for longer than the stall time: the PIO is not draining, and the panel holds its last word until a close darkens it (@xref{shutdown|shutdown}).
    bool stalled() const MM_NONBLOCKING {
        const uint64_t entered = submitEnteredMs_.load();
        return entered != 0 && hub75NowMs() - entered > stallMs_;
    }
    /// Sets the least time a submit may be in flight before the engine reads as stalled, in milliseconds; a big frame's own wire time can raise it.
    void setStallMs(uint64_t ms) { stallFloorMs_ = stallMs_ = ms; }

    /// The logical canvas width the output publishes, cols*panelsPerLane; valid after open().
    int width() const { return virtWidth_; }
    /// The logical canvas height the output publishes, rows*parallel; valid after open().
    int height() const { return virtHeight_; }
    /// The number of lanes the wall is split into, valid after open().
    int lanes() const { return lanes_; }
    /// Slots per frame, which is pixel clocks per frame, per lane; valid after open().
    size_t frameSlots() const { return frameSlots_; }

    /// Frames submitted to the backend (thread-safe).
    uint64_t framesSubmitted() const { return framesSubmitted_.load(); }
    /// Frames encoded by the encode thread (thread-safe).
    uint64_t framesEncoded() const { return framesEncoded_.load(); }
    /// Frames published by the render thread (thread-safe).
    uint64_t framesPublished() const { return rgb_.published(); }
    /// Submits the backend reported as failed, the refresh thread's blank on exit included (thread-safe).
    uint64_t submitErrors() const MM_NONBLOCKING { return submitErrors_.load(); }
    /// The last encode duration in microseconds (the encode thread writes it).
    uint64_t lastEncodeUs() const { return lastEncodeUs_.load(); }
    /// The bytes the engine's frame buffers hold: 0 once shut down.
    size_t heapBytes() const {
        return rgb_.heapBytes() + encoded_.heapBytes() + blank_.capacity() * sizeof(uint32_t) +
               (slots_[0].capacity() + slots_[1].capacity() + slots_[2].capacity()) * sizeof(uint16_t);
    }
    /// Whether both threads have ended: false while the engine runs and while shutdown found the refresh thread stuck.
    bool threadsJoined() const { return !encodeThread_.joinable() && !refreshThread_.joinable(); }
    /// Whether the last shutdown gave up on a refresh thread stuck in a submit; the engine then holds its buffers and refuses an open.
    bool wedged() const { return wedged_.load(); }
    /// Why the last shutdown did not finish, or that its stop failed, or the note a stop left when it darkened the panel; empty after a clean stop.
    const std::string& shutdownError() const { return shutdownError_; }
    /// Sets the least shutdown waits for the refresh thread, in milliseconds, and the deadline now; the next open raises the deadline above it for a wall that takes long on the wire.
    void setJoinDeadlineMs(int ms) { joinFloorMs_ = joinDeadlineMs_ = ms; }
    /// How long shutdown waits for the refresh thread now, in milliseconds: the floor, or more for the wall the last open sized.
    int joinDeadlineMs() const { return joinDeadlineMs_; }

    /// A test hook: the next `n` frames fail to encode, every frame when `n` is -1, none when 0; it holds across opens until set again.
    void failEncodes(int n) { encodeFaults_.store(n); }
    /// A test hook: each encode takes `us` microseconds longer, 0 for none.
    void delayEncodes(uint32_t us) { encodeDelayUs_.store(us); }
    /// A test hook: while paused, the encode thread takes no frame.
    void pauseEncodes(bool paused) { encodePaused_.store(paused); }
    /// A test hook: while set, the open's refresh thread fails to start, as on a machine that cannot make a thread; it holds across opens until cleared.
    void failRefreshThreadStart(bool fails) { refreshStartFails_.store(fails); }
    /// A test hook: while set, the open's encode thread fails to start, as on a machine that cannot make a thread; it holds across opens until cleared.
    void failEncodeThreadStart(bool fails) { encodeStartFails_.store(fails); }
    /// A test hook: while set, the open's allocation ends in std::bad_alloc once the buffers are sized, as a machine that runs out of memory does; it holds across opens until cleared.
    void failAllocations(bool fails) { allocationFails_.store(fails); }

    /// An engine owns threads and a backend, so it is not copyable.
    Hub75EngineBase(const Hub75EngineBase&) = delete;
    /// An engine owns threads and a backend, so it is not copy-assignable.
    Hub75EngineBase& operator=(const Hub75EngineBase&) = delete;

    /// Virtual, so an engine is destroyed through its own destructor, which shuts it down first.
    virtual ~Hub75EngineBase() = default;

  protected:
    Hub75EngineBase() = default;

    /// Refuses options the engine cannot drive, with the reason in `error`; open() has refused a running or wedged engine already.
    virtual bool admit(const Options& o, std::string& error) = 0;
    /// Derives the lane and canvas sizes and the slots per frame from the validated options.
    virtual void sizeTheWall() = 0;
    /// The bytes allocate() takes for the sized wall: the RGB buffer, the lane scratch, the three encoded frames and the blank frame.
    virtual uint64_t bufferBytes() const = 0;
    /// Sizes the RGB buffer, the lane scratch, the three encoded frames and the blank frame.
    virtual void allocate() = 0;
    /// Creates and opens the backend and starts it; on failure fills `error` and leaves no backend.
    virtual bool startBackend(bool useStubBackend, std::string& error) = 0;
    /// Joins the encode thread, then the refresh thread within the deadline, stops the backend and frees the buffers; false when the refresh thread stayed stuck, which leaves the backend and the buffers kept.
    virtual bool shutdown() = 0;
    /// Resets the counters only the engine keeps, before the threads start; an engine with none leaves this default.
    virtual void resetExtras() {}
    /// Encodes one lane's strip of `rgb` into `slotWords` at `brightness`; false when the encoder refuses the geometry.
    virtual bool encodeLane(const uint8_t* rgb, int lane, uint16_t* slotWords, uint8_t brightness) = 0;
    /// Repacks the lanes' slot streams into `out`, one frame of frameSlots_ words.
    virtual void repack(uint32_t* out) = 0;
    /// The refresh thread's body: submits the newest encoded frame until stop_ is set.
    virtual void refreshLoop() = 0;

    /// Sets the join deadline for a wall whose shutdown needs `wireMs` milliseconds on the wire: that, or the floor, whichever is more.
    void sizeJoinDeadline(uint64_t wireMs) {
        joinDeadlineMs_ = static_cast<int>(std::min<uint64_t>(std::max<uint64_t>(static_cast<uint64_t>(joinFloorMs_), wireMs),
                                                               static_cast<uint64_t>(std::numeric_limits<int>::max())));
    }

    /// Stops publishing and joins the encode thread, then the refresh thread within the deadline; false when it stayed stuck, leaving it joinable. A stop that finds it still stuck returns at once, so only the first pays the deadline.
    bool stopThreads() {
        running_.store(false);
        stop_.store(true);
        if (encodeThread_.joinable()) encodeThread_.join();
        if (refreshThread_.joinable()) {
            if (!refreshDone_.load()) {
                if (wedged_.load()) return false;   // already reported, and still in the kernel
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(joinDeadlineMs_);
                while (!refreshDone_.load() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!refreshDone_.load()) {
                    wedged_.store(true);
                    // What the stop then did to the panel is the engine's to say, once it has run.
                    shutdownError_ = "a submit did not return within " + std::to_string(joinDeadlineMs_) +
                                     " ms: the backend stays held until a close after the refresh thread returns";
                    return false;
                }
            }
            refreshThread_.join();
        }
        wedged_.store(false);
        shutdownError_.clear();
        return true;
    }

    /// Frees every frame buffer and zeroes the readouts, after a publish in flight has finished (seq_cst atomics, the default, make that handshake sound); the threads are joined, so the buffers are idle.
    void releaseBuffers() {
        while (publishing_.load() != 0) std::this_thread::yield();
        rgb_.release();
        encoded_.release();
        for (auto& lane : slots_) std::vector<uint16_t>().swap(lane);
        std::vector<uint32_t>().swap(blank_);
        fps_.store(0);
        frameSlots_ = 0;
    }

    /// Encodes every lane's strip of `rgb` at one brightness, repacks the lanes' slot streams into `out` and stamps the encode time; false when a lane's encode failed or a test hook failed it.
    bool encodeFrame(const uint8_t* rgb, uint32_t* out) {
        const auto t0 = std::chrono::steady_clock::now();
        if (const uint32_t us = encodeDelayUs_.load()) std::this_thread::sleep_for(std::chrono::microseconds(us));
        if (takeEncodeFault()) return false;
        const uint8_t brightness = brightness_.load();   // once a frame: every lane is lit alike
        for (int c = 0; c < lanes_; c++) {
            if (!encodeLane(rgb, c, slots_[c].data(), brightness)) return false;
        }
        repack(out);
        lastEncodeUs_ = hub75MicrosSince(t0);
        framesEncoded_++;
        return true;
    }

    Options options_{};
    int lanes_ = 0;
    int laneWidth_ = 0;     // one lane's logical width: cols*panelsPerLane
    int laneHeight_ = 0;    // one lane's logical height: rows
    int virtWidth_ = 0;
    int virtHeight_ = 0;
    size_t frameSlots_ = 0;   // pixel clocks per frame, per lane

    RgbTripleBuffer rgb_;                      // render thread to encode thread
    std::vector<uint16_t> slots_[kHub75Lanes]; // one lane's slot stream (encode thread only)
    TripleBuffer<uint32_t> encoded_;           // encode thread to refresh thread: the frames the backend submits
    std::vector<uint32_t> blank_;              // the all-dark frame

    std::thread encodeThread_;
    std::thread refreshThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> refreshDone_{false};   // the refresh thread has left its loop
    std::atomic<bool> wedged_{false};
    std::atomic<int> publishing_{0};         // publishRgb calls in flight; seq_cst with running_, so a publish sees the stop or the stop sees the publish
    std::atomic<uint64_t> submitEnteredMs_{0};   // when the refresh thread entered the submit it is in, 0 between submits
    uint64_t stallFloorMs_ = kHub75StallMs;  // the least a submit may take before the engine reads as stalled
    uint64_t stallMs_ = kHub75StallMs;       // how long a submit may take before the engine reads as stalled: the floor, or more for a frame that takes long on the wire
    std::string shutdownError_;
    int joinFloorMs_ = kHub75JoinDeadlineMs;     // the least the join deadline may be
    int joinDeadlineMs_ = kHub75JoinDeadlineMs;  // how long shutdown waits for the refresh thread: the floor, or more for a wall that takes long on the wire
    std::atomic<uint8_t> brightness_{255};
    std::atomic<uint16_t> fps_{0};

    std::atomic<uint64_t> framesSubmitted_{0};   // the refresh thread writes it
    std::atomic<uint64_t> framesEncoded_{0};     // the encode thread writes it
    std::atomic<uint64_t> submitErrors_{0};      // the refresh thread writes it
    std::atomic<uint64_t> lastEncodeUs_{0};      // the encode thread writes it

  private:
    /// Allocates the wall's buffers; an allocation that throws std::bad_alloc frees what was taken, fills `error` with the need and returns false.
    bool allocateWall(std::string& error) {
        constexpr uint64_t kMegabyte = 1024 * 1024;
        const std::string needMb = std::to_string((bufferBytes() + kMegabyte - 1) / kMegabyte) + " MB";
        try {
            allocate();
            if (allocationFails_.load()) throw std::bad_alloc();
        } catch (const std::bad_alloc&) {
            releaseBuffers();
            error = "the system ran out of memory allocating the wall's buffers (" + needMb + ")";
            return false;
        }
        return true;
    }

    /// Encodes the black RGB frame into the first encoded frame, so the refresh thread has a valid frame from its first iteration.
    bool firstFrame(std::string& error) {
        if (!encodeFrame(rgb_.current(), encoded_.writeSlot())) {
            error = "initial encode failed (encoder refused the geometry)";
            return false;
        }
        encoded_.publishWritten();
        return true;
    }

    /// Whether a test hook fails this frame: the next `n` do, every one at -1.
    bool takeEncodeFault() {
        int n = encodeFaults_.load();
        while (n != 0) {
            if (n < 0) return true;
            if (encodeFaults_.compare_exchange_weak(n, n - 1)) return true;
        }
        return false;
    }

    /// The encode thread's body: takes the newest RGB frame, encodes it and publishes the result; a failed encode skips the publish.
    void encodeLoop() {
        while (!stop_.load()) {
            if (!encodePaused_.load() && rgb_.take()) {
                if (encodeFrame(rgb_.current(), encoded_.writeSlot())) encoded_.publishWritten();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(kEncodePollUs));
            }
        }
    }

    /// Resets the run's counters, after the engine's own through resetExtras(), and starts the refresh thread and then the encode thread.
    bool launchThreads(std::string& error) {
        resetExtras();
        stop_.store(false);
        framesSubmitted_ = 0;
        framesEncoded_ = 0;
        submitErrors_ = 0;
        fps_.store(0);
        refreshDone_.store(false);
        running_ = true;
        try {
            if (refreshStartFails_.load()) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            refreshThread_ = std::thread([this] {
                blockStopSignals();
                refreshLoop();
                refreshDone_.store(true);
            });
        } catch (...) {
            error = "failed to start the refresh thread";
            return false;
        }
        try {
            if (encodeStartFails_.load()) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            encodeThread_ = std::thread([this] { encodeLoop(); });
        } catch (...) {
            error = "failed to start the encode thread";   // the refresh thread is joined by open()'s shutdown, within the deadline: it submits a blank as it leaves, which can block for good
            return false;
        }
        return true;
    }

    /// How often the idle encode thread looks for a new RGB frame, in microseconds; it polls because publishRgb must stay nonblocking, at about 2,000 idle wake-ups a second (an estimate from the period).
    static constexpr int kEncodePollUs = 500;

    std::atomic<int> encodeFaults_{0};      // the frames the failEncodes hook still fails
    std::atomic<uint32_t> encodeDelayUs_{0};
    std::atomic<bool> encodePaused_{false};
    std::atomic<bool> refreshStartFails_{false};   // whether the failRefreshThreadStart hook fails the refresh thread's start
    std::atomic<bool> encodeStartFails_{false};   // whether the failEncodeThreadStart hook fails the encode thread's start
    std::atomic<bool> allocationFails_{false};   // whether the failAllocations hook ends the allocation in std::bad_alloc
};

/// The default Hub75Layout the repack hard-codes, pinned by the static_assert below (@xref{default-layout|default layout}).
inline constexpr mm::Hub75Layout kHub75DefaultLayout{};
static_assert(kHub75DefaultLayout.r1 == 0 && kHub75DefaultLayout.g1 == 1 && kHub75DefaultLayout.b1 == 2 &&
                  kHub75DefaultLayout.r2 == 3 && kHub75DefaultLayout.g2 == 4 && kHub75DefaultLayout.b2 == 5 &&
                  kHub75DefaultLayout.a == 8 && kHub75DefaultLayout.b == 9 && kHub75DefaultLayout.c == 10 &&
                  kHub75DefaultLayout.d == 11 && kHub75DefaultLayout.e == 12 && kHub75DefaultLayout.lat == 13 &&
                  kHub75DefaultLayout.oe == 14,
              "hub75 repack assumes the default Hub75Layout");

/// The default Hub75Layout bit positions in the 16-bit slot word (platform/Hub75Bus.h), hard-coded by the repack.
inline constexpr int kSlotR1 = 0, kSlotG1 = 1, kSlotB1 = 2;
inline constexpr int kSlotR2 = 3, kSlotG2 = 4, kSlotB2 = 5;
inline constexpr int kSlotA = 8, kSlotB = 9, kSlotC = 10, kSlotD = 11, kSlotE = 12;
inline constexpr int kSlotLat = 13, kSlotOe = 14;

/// The six color slot bits, in the order they index a lane's color table.
inline constexpr int kColorSlotBits[6] = {kSlotR1, kSlotG1, kSlotB1, kSlotR2, kSlotG2, kSlotB2};

/// A lane's six color GPIOs in slot-bit order: r1, g1, b1, r2, g2, b2.
template <typename Pins>
constexpr std::array<uint8_t, 6> hub75ColorGpios(const Pins& pins) {
    return {pins.r1, pins.g1, pins.b1, pins.r2, pins.g2, pins.b2};
}

/// The output word for the color bits `slot` sets: each set bit of kColorSlotBits puts its GPIO, placed by `bitOf`, in the word.
template <typename BitOf>
constexpr uint32_t hub75ScatterColor(const std::array<uint8_t, 6>& gpios, unsigned slot, BitOf bitOf) {
    uint32_t mask = 0;
    for (int i = 0; i < 6; i++)
        if (slot & (1u << kColorSlotBits[i])) mask |= 1u << bitOf(gpios[i]);
    return mask;
}

/// The color word of the first `lanes` lanes' slot words, bit by bit with no tables: the reference the tests check the tables against, the lane pins placed by `bitOf`.
template <typename Pins, typename BitOf, size_t kLanes>
uint32_t hub75ReferenceColors(const uint16_t (&slotByLane)[kLanes], int lanes, const Pins (&lanePins)[kLanes], BitOf bitOf) {
    uint32_t w = 0;
    for (int c = 0; c < lanes && c < static_cast<int>(kLanes); c++) {
        const std::array<uint8_t, 6> gpios = hub75ColorGpios(lanePins[c]);
        for (int i = 0; i < 6; i++)
            if (slotByLane[c] & (1u << kColorSlotBits[i])) w |= 1u << bitOf(gpios[i]);
    }
    return w;
}

/// Fills each lane's 64-entry color table: entry `bits` is the scatter of those six color bits through the lane's pins.
template <typename Pins, typename BitOf, size_t kLanes>
void hub75FillColorTables(uint32_t (&color)[kLanes][64], const Pins (&lanePins)[kLanes], BitOf bitOf) {
    for (size_t c = 0; c < kLanes; c++) {
        const std::array<uint8_t, 6> gpios = hub75ColorGpios(lanePins[c]);
        for (unsigned bits = 0; bits < 64; bits++) color[c][bits] = hub75ScatterColor(gpios, bits, bitOf);
    }
}

}  // namespace mm::platform
