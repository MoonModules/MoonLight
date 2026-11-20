// SPDX-License-Identifier: GPL-3.0-or-later

/// The stub PIO backend's test surface: a copy of the last frame it was asked to submit, the records of what the engine handed it, the faults it can raise and the queue model it paces by. An engine test reads the wide words the refresh thread produced, and what it started with, on a machine with no Pi 5 attached; production calls none of these.
///
/// What it keeps and raises is in @xref{records|Records}, @xref{faults|Faults}, @xref{queue-model|Queue model} and @xref{limits|Limits}.
///
/// @technical
///
/// ## Records
///
/// The stub keeps the last start configuration with a copy of its program, the count and byte size of the blank frames, and an identity for each submitted frame: a checksum and its first and last words. The identities sit in a ring of the last `kStubFrameIdRing` frames, so a test can tell which published frame a submit carried while holding only that ring.
///
/// ## Faults
///
/// `StubFaults` makes the open, the start, the stop, the next submits or the blank fail, or makes a submit hang until the fault is cleared. A hang stands in for the wait of a kernel from 6.18.39, whose submit has no timeout, and it holds in both queue models.
///
/// A failed submit returns -EIO, or the code a test sets: the codes a failed ioctl hands back through piolib (-EINTR, -EIO, -EFAULT, -EINVAL, -ETIMEDOUT, -EREMOTEIO).
///
/// `drainTimesOut` makes a stop with `drain` return true with a note in `error`, as a FIFO that stays full does on the real backend, which the disable darkens all the same.
///
/// `fifoEmptyEarly` makes stop() skip the wait for the queue, as an early FIFO-empty poll does, recording how many chunks of real frames were then still in flight. A blank that does not cover the whole ring leaves some, and the SM stops on whatever word of one it shifted last.
///
/// ## Limits
///
/// start() refuses what the kernel refuses: more than 4 bounce buffers, a buffer size of 0 or one that is not a multiple of 4. The stub does not model the feed ceiling the bench measured: a chunk completes at exactly its wire time. The engine's encode has its own hook, Hub75PioEngine::failEncodes. Every hook lasts until the next `stubBackendReset()`.
///
/// ## Queue model
///
/// `Blocking` models the 6.12 kernel, where a submit returns once the frame has been delivered, so it sleeps the frame's wire time. `Ring` models a kernel that queues: a frame splits into chunks of `xferBufSize` bytes, a submit returns once its chunks are queued, and it blocks only while `xferBufCount` chunks are in flight, each completing after its own wire time. `Blocking` is the default.

#pragma once

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "platform/rpi/hub75_pio_backend.h"

namespace mm::platform {

/// How the stub paces a submit.
enum class StubQueueModel : uint8_t {
    Blocking,   ///< a submit returns after the frame's whole wire time
    Ring,       ///< a submit returns once queued, and blocks while the ring of `xferBufCount` chunks is full
};

/// What the stub refuses.
struct StubFaults {
    bool failOpen = false;     ///< open() fails
    bool failStart = false;    ///< start() fails
    int failSubmits = 0;       ///< the next this many submits, the blank's included, fail with `submitError`; every one when -1
    int submitError = -5;      ///< the code a failed submit returns, -EIO by default
    bool hangSubmits = false;  ///< a submit blocks until the faults change, as one stuck in the kernel
    bool failBlank = false;        ///< a blank submit fails with `submitError`, and the frames' submits succeed
    bool failStop = false;         ///< a stop fails with an error and leaves the pins as they were, as a disable or a pin write the kernel refuses does; every stop fails while it is set, retries included
    bool fifoEmptyEarly = false;   ///< a stop with `drain` skips the wait for the queue, as an early FIFO-empty poll does
    bool drainTimesOut = false;    ///< a stop with `drain` notes that the FIFO did not drain, as piolib's does, and still darkens the pins
    uint32_t copyMicros = 0;       ///< microseconds a submit takes to copy the frame out of the caller's buffer, reading half before and half after, as a slow kernel copy does
};

/// What start() last received.
struct StubStartRecord {
    uint32_t starts = 0;           ///< start() calls that succeeded since the last reset
    PioStartConfig config;         ///< the last configuration, its program pointer as given
    std::vector<uint16_t> program; ///< the `programLength` instruction words the pointer held at the call
};

/// One submitted frame's identity.
struct StubFrameId {
    uint64_t index = 0;      ///< the frame's place in the run of submits, from 1
    uint64_t checksum = 0;   ///< a hash of every word, so two frames differ in it unless they are the same words
    uint32_t first = 0;      ///< the first word
    uint32_t last = 0;       ///< the last word
    uint32_t words = 0;      ///< the frame's length in words
};

/// The identities kept: the last this many frames.
constexpr size_t kStubFrameIdRing = 64;

/// Frames the stub backend has been asked to submit since the last reset, the blank frames excluded.
uint64_t stubBackendSubmittedFrames();

/// Copies the last submitted frame's wide words into `out`, at most `capWords`, and returns the words copied (0 before any frame); one whole frame, which the refresh thread cannot replace mid-copy. A blank counts as the last frame.
size_t stubBackendCopyLastFrame(uint32_t* out, size_t capWords);

/// Drops the captured frame and resets the submit count.
void stubBackendResetCapture();

/// Copies the identities of the last submitted frames into `out`, oldest first, at most `cap`, and returns how many.
size_t stubBackendFrameIds(StubFrameId* out, size_t cap);

/// What the last start() received, with the count of starts.
StubStartRecord stubBackendLastStart();

/// Submits attempted after a stop and before the next start, which a refresh thread still alive at the stop, or released late, would make.
uint32_t stubBackendSubmitsAfterStop();

/// Frame submits made by a thread with SIGTERM deliverable, a set the engine's refresh thread stays out of (blockStopSignals); the blank is not counted.
uint32_t stubBackendSubmitsWithStopOpen();

/// How many chunks of real frames were still in flight when the last stop with `drain` ran: 0 when the stop was safe, since only the blank's own chunks were left.
uint32_t stubBackendRealChunksInFlightAtStop();

/// Blank submits the stub was asked for since the last reset, the failed ones included.
uint32_t stubBackendBlankAttempts();

/// The thread that made the last recorded blank submit, a default id before any.
std::thread::id stubBackendBlankThread();

/// The submits, frames and blanks, that `thread` made since the last reset.
uint32_t stubBackendSubmitsFromThread(std::thread::id thread);

/// The stops that stopped a started stub since the last reset: one per start, whatever later stops and retries follow.
uint32_t stubBackendStops();

/// Whether the last of those stops had `drain` set: true after a clean shutdown, false after one that skipped the wait, as on a missed join deadline; false before any.
bool stubBackendLastStopDrained();

/// Whether a stop has run and set the pins to the dark word since the last start; a start clears it, and a stop that failed (`failStop`) leaves it clear.
bool stubBackendPinsDark();

/// Blank frames the stub has been asked to submit since the last reset.
uint32_t stubBackendBlanks();

/// The byte size of the last blank frame, 0 before the first.
uint32_t stubBackendLastBlankBytes();

/// Sets what the stub refuses, replacing the earlier faults.
void stubBackendSetFaults(const StubFaults& faults);

/// Sets how a submit paces.
void stubBackendSetQueueModel(StubQueueModel model);

/// Returns the stub to its start: no capture, no records, no faults, the blocking model.
void stubBackendReset();

}  // namespace mm::platform
