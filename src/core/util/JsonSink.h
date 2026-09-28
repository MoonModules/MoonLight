#pragma once

#include "platform/platform.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

/// @defgroup JsonSink Writing JSON
/// @{
/// Writes a document with no fixed size ceiling, in one of three modes, so a module tree of any size serializes correctly.
///
/// @moreinfo
///
/// ## The three modes
///
/// A socket mode flushes a small staging buffer to a connection as it fills, so a whole response never lives in memory at once.
/// A buffer mode collects into a heap block that grows on demand, for a caller that needs the assembled document and its length up front.
/// A fixed mode writes into a caller-owned slice and raises an overflow flag rather than truncating silently or growing, which the save path wants.
///
/// ## Growth steps down when doubling is refused
///
/// Doubling is the right default, giving amortized constant-time appends, but both buffers are live across the copy.
/// So growing a large block asks for half again as much CONTIGUOUS memory at once, and a device serving a big document has the free bytes without the block.
/// The allocation then failed, every later append was dropped, and the device shipped a truncated document that looked complete.
///
/// A refused doubling therefore steps down toward the minimum rather than giving up: slower to grow, and it fits where doubling cannot.
/// Measured on the bench, both classic boards cut their document at a power of two, losing eight to eleven kilobytes of the tree.
///
/// The step is a QUARTER of the current capacity rather than the bare minimum.
/// Backing off to just enough serves one append and grows again on the next character, which copies quadratically and thrashes the fragmented heap that refused the doubling.
///
/// ## Overflow is a flag, never a silent truncation
///
/// Once tripped, later appends do nothing, so a caller sees one consistent state.
/// A caller that ships the buffer anyway sends a truncated document, indistinguishable from a whole one at the far end.
/// The receiver parses it, throws, and drops the tail.
///
/// The fixed mode never allocates, its capacity being the caller's slice on purpose, since an allocation there would succeed even when the slice is too small.
///
/// ## The number writer takes a double, and firmware must not call it
///
/// Its only caller is the test runner's bridge, which stores numerics that way so it does not lose precision parsing fixtures.
/// A double runs in software emulation on one architecture, far slower than the single-precision type, so production paths use the typed serializers instead.
namespace mm {

class JsonSink {
public:
    // Socket mode.
    /// Socket mode: a staging buffer flushes to the connection as it fills.
    explicit JsonSink(platform::TcpConnection& conn) : conn_(&conn) {}

    // Buffer mode — collects into a growable heap buffer.
    /// Buffer mode: bytes collect in a block this owns.
    JsonSink() = default;

    /// Fixed mode, writing into a caller-owned slice with no allocation: @xref{overflow-is-a-flag-never-a-silent-truncation|what happens when it fills}.
    JsonSink(char* buf, size_t cap) : fixed_(buf), fixedCap_(cap) {
        if (fixed_ && fixedCap_ > 0) fixed_[0] = '\0';
    }


    /// Frees the block, when one was taken and not detached.
    ~JsonSink() { if (heap_) platform::free(heap_); }

    /// Non-copyable: it owns a heap block and possibly a connection.
    JsonSink(const JsonSink&) = delete;
    /// Non-assignable, for the same reason.
    JsonSink& operator=(const JsonSink&) = delete;

    /// Append a string, growing or flushing as the mode requires.
    void append(const char* s) {
        if (!s) return;
        while (*s) {
            if (conn_) {
                if (pos_ == STAGE_SIZE) flushStage();
                stage_[pos_++] = *s++;
            } else if (fixed_) {
                // One byte reserved for the terminator; once overflow trips, later appends do nothing.
                if (fixedLen_ + 1 >= fixedCap_) { overflowed_ = true; return; }
                fixed_[fixedLen_++] = *s++;
                fixed_[fixedLen_] = '\0';
            } else {
                // Out of memory, flagged as the fixed path does: @xref{overflow-is-a-flag-never-a-silent-truncation|why a truncated document is worse than none}.
                if (!ensureHeap(heapLen_ + 1)) { overflowed_ = true; return; }
                heap_[heapLen_++] = *s++;
            }
        }
        // Terminated after every append, so the data stays a valid string even when a caller skips the length.
        if (!conn_ && !fixed_ && heap_) heap_[heapLen_] = '\0';
    }

    /// Append a formatted fragment; the common case fits a stack buffer and a longer one is re-formatted so nothing is silently truncated.
    void appendf(const char* fmt, ...) MM_PRINTF_FORMAT(2, 3) {
        char frag[FRAG_MAX];
        va_list ap;
        va_start(ap, fmt);
        va_list ap2;
        va_copy(ap2, ap);
        int n = std::vsnprintf(frag, sizeof(frag), fmt, ap);
        va_end(ap);
        if (n < 0) { va_end(ap2); return; }
        if (static_cast<size_t>(n) < sizeof(frag)) {
            va_end(ap2);
            append(frag);
            return;
        }
        // Fragment longer than the stack buffer.
        if (fixed_) {
            // The fixed mode never allocates: @xref{overflow-is-a-flag-never-a-silent-truncation|why an allocation here would hide the problem}.
            overflowed_ = true;
            va_end(ap2);
            return;
        }
        // The growing modes format into an exactly-sized block, so a long fragment is never truncated.
        char* big = static_cast<char*>(platform::alloc(static_cast<size_t>(n) + 1));
        if (big) {
            std::vsnprintf(big, static_cast<size_t>(n) + 1, fmt, ap2);
            append(big);
            platform::free(big);
        }
        va_end(ap2);
    }

    /// Write one syntactically correct value: @xref{the-number-writer-takes-a-double-and-firmware-must-not-call-it|why firmware uses the typed serializers instead}.
    void writeNumber(double v) {
        // JSON has no NaN or infinity, and the cast below is undefined for them.
        if (!std::isfinite(v)) { append("null"); return; }
        // A whole value renders as an integer; the range test guards the cast the test itself uses.
        if (v >= -9.2e18 && v <= 9.2e18 && v == static_cast<double>(static_cast<long long>(v))) {
            appendf("%lld", static_cast<long long>(v));
        } else {
            appendf("%g", v);
        }
    }
    /// Write a boolean literal.
    void writeBool(bool v) { append(v ? "true" : "false"); }
    /// Write a string as a quoted literal, escaping what the standard requires.
    void writeJsonString(const char* s) {
        // A character at a time, so there is no truncation ceiling; the standard requires escaping the quote, the backslash and every control byte.
        if (!s) s = "";
        append("\"");
        char buf[8];  // longest emission is "\uXXXX" (6) + NUL; 8 is round
        for (; *s; s++) {
            unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') {
                buf[0] = '\\'; buf[1] = static_cast<char>(c); buf[2] = 0;
                append(buf);
            } else if (c == '\n') { append("\\n"); }
            else if (c == '\r') { append("\\r"); }
            else if (c == '\t') { append("\\t"); }
            else if (c == '\b') { append("\\b"); }
            else if (c == '\f') { append("\\f"); }
            else if (c < 0x20) {
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                append(buf);
            } else {
                buf[0] = static_cast<char>(c); buf[1] = 0;
                append(buf);
            }
        }
        append("\"");
    }

    // Socket mode: flush staged bytes to the socket. Call once at the end.
    /// Push whatever is staged to the connection, in socket mode.
    void flush() { flushStage(); }

    /// The collected document and its length, terminated; in fixed mode this is the caller's own buffer.
    const char* data() const {
        if (fixed_) return fixed_;
        return heap_ ? heap_ : "";
    }
    /// How many bytes have been written.
    size_t size() const { return fixed_ ? fixedLen_ : heapLen_; }

    /// Hand the block to the caller and give up ownership, so a large built document is not copied out; nothing in the other modes.
    char* detach() {
        if (conn_ || fixed_) return nullptr;
        char* out = heap_;
        heap_ = nullptr;
        heapCap_ = heapLen_ = 0;
        return out;
    }

    // Fixed-buffer mode only: did any append run out of capacity?
    /// Whether a write was refused, which makes the document incomplete.
    bool overflowed() const { return overflowed_; }

    /// Which single option a palette call wants, the default meaning the whole set: it rides here because the callback takes only a sink, the one channel into the light domain.
    int nameIndex() const { return nameIndex_; }
    /// Ask a palette callback for one option's name rather than the whole set.
    void requestName(uint8_t index) { nameIndex_ = static_cast<int>(index); }

private:
    static constexpr size_t STAGE_SIZE = 1024;
    static constexpr size_t FRAG_MAX = 256;

    void flushStage() {
        if (conn_ && pos_ > 0) {
            conn_->write(reinterpret_cast<const uint8_t*>(stage_), pos_);
            pos_ = 0;
        }
    }

    /// Grow to hold at least what is needed plus a terminator: @xref{growth-steps-down-when-doubling-is-refused|what a refused doubling does}.
    bool ensureHeap(size_t need) {
        if (need + 1 <= heapCap_) return true;
        size_t want = heapCap_ == 0 ? 2048 : heapCap_ * 2;
        while (want < need + 1) want *= 2;
        // In quarters rather than to the bare minimum: @xref{growth-steps-down-when-doubling-is-refused|why a minimal step thrashes}.
        const size_t step = heapCap_ / 4 > 4096 ? heapCap_ / 4 : 4096;
        const size_t floorCap = need + 1 > step ? need + 1 : step;
        for (;;) {
            if (char* grown = static_cast<char*>(platform::alloc(want))) {
                if (heap_) { std::memcpy(grown, heap_, heapLen_); platform::free(heap_); }
                heap_ = grown;
                heapCap_ = want;
                heap_[heapLen_] = 0;
                return true;
            }
            if (want <= floorCap) return false;   // even a useful minimum is refused: genuinely out
            const size_t next = want - step;
            want = next < floorCap ? floorCap : next;
        }
    }

    platform::TcpConnection* conn_ = nullptr;  // socket mode when non-null
    char stage_[STAGE_SIZE];
    size_t pos_ = 0;

    char* heap_ = nullptr;                     // buffer mode
    size_t heapLen_ = 0;
    size_t heapCap_ = 0;

    char* fixed_ = nullptr;                    // fixed-buffer mode
    size_t fixedLen_ = 0;
    size_t fixedCap_ = 0;
    bool overflowed_ = false;
    int  nameIndex_ = -1;   // >= 0: this sink is asking for that option's name (see requestName)
};

/// Escape a string for embedding in a literal, without the surrounding quotes, truncating rather than overflowing its output.
inline void jsonEscape(const char* in, char* out, size_t outMax) {
    if (outMax == 0) return;  // no room even for the terminator
    size_t oi = 0;
    for (; *in && oi + 2 < outMax; in++) {
        if (*in == '"' || *in == '\\') out[oi++] = '\\';
        out[oi++] = *in;
    }
    out[oi] = 0;
}

/// @}

} // namespace mm
