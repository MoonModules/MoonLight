#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm {

/// The state machine that joins chunked Improv op frames back into one op-JSON buffer.
///
/// @technical
///
/// ## Where it sits
///
/// This is the pure logic behind the device's 0xFC handler, which treats Improv as REST over serial.
/// Each platform's listener owns its transport and hands each APPLY_OP payload to @ref improvApplyOp, which feeds the chunks here and holds the `opReady` handshake.
/// That is the same core-against-platform line `ImprovFrame.h` draws, so the reassembly and its sequence guard are proven on the desktop without hardware.
///
/// ## Why the sequence guard is real
///
/// A chunk carries `[seq][last][chunk bytes]`. Sequence 0 starts a fresh op and resets the buffer, and every later chunk must be the next sequence in order.
/// A duplicate, which an installer retry on a misread timeout produces, or an out-of-order chunk would splice garbage into the buffer. Both are rejected and the buffer is reset.
/// USB serial delivers in order, but the installer's send is open-loop and can re-emit a chunk, so the guard guards something that happens.
///
/// Joins the chunks of one op into a NUL-terminated buffer the caller owns.
class ImprovOpReassembler {
public:
    /// What a fed chunk left the reassembler in.
    enum class Result : uint8_t {
        Continue,  // chunk accepted, more expected (not the last)
        Ready,     // last chunk accepted; out() is a complete NUL-terminated op
        Error,     // bad chunk (out-of-order / duplicate / overflow); buffer reset
    };

    /// Reassemble into `buf`, of which one byte is reserved for the NUL.
    ImprovOpReassembler(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    /// Feed one chunk, `seq` being its 0-based index and `last` true on the final one.
    Result feed(uint8_t seq, bool last, const uint8_t* chunk, size_t chunkLen) {
        // seq 0 always starts fresh, so a new op recovers after one that errored mid-stream: @xref{why-the-sequence-guard-is-real}.
        if (seq == 0) {
            len_ = 0;
            nextSeq_ = 1;
        } else if (seq != nextSeq_) {
            reset();
            return Result::Error;
        } else {
            nextSeq_++;
        }

        // Overflow guard: keep one byte for the NUL. Drop + error rather than truncate.
        if (len_ + chunkLen >= cap_) {
            reset();
            return Result::Error;
        }
        if (chunkLen) std::memcpy(buf_ + len_, chunk, chunkLen);
        len_ += chunkLen;

        if (last) {
            buf_[len_] = 0;
            size_t complete = len_;
            reset();          // ready for the next op
            len_ = complete;  // ...but keep the length readable until the next feed()
            return Result::Ready;
        }
        return Result::Continue;
    }

    /// The reassembled op, complete and NUL-terminated once @ref feed returns `Ready`.
    const char* out() const { return buf_; }
    /// How many bytes of @ref out the finished op filled.
    size_t len() const { return len_; }

    /// Drop any partial op, for a consumer that wants a clean slate.
    void reset() {
        len_ = 0;
        nextSeq_ = 0;
    }

private:
    char* buf_;
    size_t cap_;
    size_t len_ = 0;       // bytes reassembled so far
    uint8_t nextSeq_ = 0;  // next chunk index expected (0 = awaiting a fresh op)
};

/// The vendor command carrying one state document over serial, and the error a refused chunk replies with.
inline constexpr uint8_t kImprovCmdApplyOp = 0xFC;
inline constexpr uint8_t kImprovErrorInvalidOp = 0x82;
/// The protocol's own errors: a frame failing its checksum, and a command the listener does not take.
inline constexpr uint8_t kImprovErrorInvalidRpc = 0x01;
inline constexpr uint8_t kImprovErrorUnknownRpc = 0x02;

/// What a transport replies to one APPLY_OP frame: an empty response for a taken chunk, the invalid-op error otherwise.
enum class ImprovApplyOpReply : uint8_t { Ack, Refused };

/// Handle one APPLY_OP payload, `[0xFC][seq][last][chunk…]`, for every transport (@xref{where-it-sits}): refused while a document waits or when malformed, and a last chunk sets `opReady`.
inline ImprovApplyOpReply improvApplyOp(ImprovOpReassembler& reasm, const uint8_t* payload, size_t len, std::atomic<bool>& opReady) {
    // Single-buffered: the acquire pairs with the consumer's release when it clears the flag after applying.
    if (opReady.load(std::memory_order_acquire)) return ImprovApplyOpReply::Refused;
    // `last` is a flag on the wire, so anything but 0 or 1 is a malformed frame rather than "more chunks".
    if (len < 3 || payload[2] > 1) return ImprovApplyOpReply::Refused;
    switch (reasm.feed(payload[1], payload[2] != 0, payload + 3, len - 3)) {
        case ImprovOpReassembler::Result::Error: return ImprovApplyOpReply::Refused;
        case ImprovOpReassembler::Result::Ready: opReady.store(true, std::memory_order_release); break;
        case ImprovOpReassembler::Result::Continue: break;
    }
    return ImprovApplyOpReply::Ack;
}

}  // namespace mm

