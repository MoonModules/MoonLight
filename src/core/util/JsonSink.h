#pragma once

#include "core/util/JsonEscape.h"   // jsonEscape: a string as a literal's contents, control characters included
#include "platform/platform_heap.h"   // alloc, free
#include "platform/platform_net.h"   // TcpConnection

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

/// Writes a document with no fixed size ceiling, in one of three modes, so a module tree of any size serializes correctly.
///
/// @technical
///
/// ## The three modes
///
/// A socket mode flushes a small heap-staged buffer to a connection as it fills, so a whole response never lives in memory at once. Without a stage, each append goes to the connection as it comes, slower and still whole.
///
/// A buffer mode collects into a heap block that grows on demand, for a caller that needs the assembled document and its length up front.
///
/// A fixed mode writes into a caller-owned slice and raises an overflow flag rather than truncating silently or growing, which the save path wants.
///
/// ## Growth steps down when doubling is refused
///
/// Doubling gives amortized constant-time appends, but both buffers are live across the copy. Growing a large block asks for as much again as CONTIGUOUS memory, which a fragmented heap often lacks.
///
/// A refused doubling steps down toward the minimum rather than giving up. The step is a quarter of the capacity: backing off to the bare minimum grows again on the next character, copying quadratically.
///
/// ## Overflow is a flag, never a silent truncation
///
/// Once tripped, later appends do nothing, so a caller sees one consistent state. A caller that ships the buffer anyway sends a truncated document, indistinguishable from a whole one at the far end.
///
/// The fixed mode never allocates, its capacity being the caller's slice on purpose, since an allocation there would succeed even when the slice is too small.
namespace mm {

class JsonSink {
public:
    /// Socket mode: a staging buffer flushes to the connection as it fills, one of @xref{the-three-modes}.
    explicit JsonSink(platform::TcpConnection& conn)
        : conn_(&conn), stage_(static_cast<char*>(platform::alloc(STAGE_SIZE))) {}

    /// Buffer mode: bytes collect in a block this owns.
    JsonSink() = default;

    /// Fixed mode, writing into a caller-owned slice with no allocation: @xref{overflow-is-a-flag-never-a-silent-truncation|what happens when it fills}.
    JsonSink(char* buf, size_t cap) : fixed_(buf), fixedCap_(cap) {
        if (fixed_ && fixedCap_ > 0) fixed_[0] = '\0';
    }


    /// Frees the block, when one was taken and not detached, and the stage.
    ~JsonSink();

    /// Non-copyable: it owns a heap block and possibly a connection.
    JsonSink(const JsonSink&) = delete;
    /// Non-assignable, for the same reason.
    JsonSink& operator=(const JsonSink&) = delete;

    /// Append a string, growing or flushing as the mode requires.
    void append(const char* s) {
        if (s) append(s, std::strlen(s));
    }
    /// Append `len` bytes, a slice of a larger document that carries no terminator of its own.
    void append(const char* s, size_t len);

    /// Append a formatted fragment; the common case fits a stack buffer and a longer one is re-formatted so nothing is silently truncated.
    void appendf(const char* fmt, ...) MM_PRINTF_FORMAT(2, 3);

    /// Write a boolean literal.
    void writeBool(bool v) { append(v ? "true" : "false"); }
    /// Write a string as a quoted literal, escaping what the standard requires.
    void writeJsonString(const char* s);

    /// Push whatever is staged to the connection, in socket mode; call it once at the end.
    void flush() { flushStage(); }

    /// The collected document and its length, terminated; in fixed mode this is the caller's own buffer.
    const char* data() const {
        if (fixed_) return fixed_;
        return heap_ ? heap_ : "";
    }
    /// How many bytes the buffer or slice holds; a socket sink holds none, and `written()` counts what went out.
    size_t size() const { return fixed_ ? fixedLen_ : heapLen_; }

    /// Hand the block to the caller and give up ownership, so a large built document is not copied out; nothing in the other modes.
    char* detach();

    /// Empty a buffer-mode sink and give its block back, so a sink kept between uses holds no memory while empty.
    void clear();

    /// Bytes appended so far in any mode, a socket's included, which `size()` cannot tell: whether a writer wrote anything.
    size_t written() const { return written_; }

    /// Whether a write was refused for want of capacity, which makes the document incomplete.
    bool overflowed() const { return overflowed_; }

    /// Which single option a palette call wants, the default meaning the whole set: it rides here because the callback takes only a sink, the one channel into the light domain.
    int nameIndex() const { return nameIndex_; }
    /// Ask a palette callback for one option's name rather than the whole set.
    void requestName(uint8_t index) { nameIndex_ = static_cast<int>(index); }
    /// Which single option object a palette call writes, its comma before it past the first, or -1 for the whole set.
    int optionIndex() const { return optionIndex_; }
    /// Ask a palette callback for one option object, so a large set goes out a piece at a time; -1 asks for the whole set again.
    void requestOption(int index) { optionIndex_ = index; }

private:
    static constexpr size_t STAGE_SIZE = 1024;
    static constexpr size_t FRAG_MAX = 256;

    /// Write the staged bytes to the connection.
    void flushStage();
    /// Copy a run into the stage, flushing it as it fills.
    void appendStaged(const char* s, size_t len);
    /// Copy what fits of a run into the caller's slice, tripping the flag for the rest.
    void appendFixed(const char* s, size_t len);
    /// Copy a run into the block, grown to hold it first.
    void appendHeap(const char* s, size_t len);
    /// Format a fragment longer than the stack buffer into a block of its exact size.
    void appendLong(const char* fmt, va_list ap, size_t n);
    /// The capacity a growth asks for first: double, until it holds what is needed and a terminator.
    size_t growthTarget(size_t need) const;
    /// Grow to hold at least what is needed plus a terminator: @xref{growth-steps-down-when-doubling-is-refused|what a refused doubling does}.
    bool ensureHeap(size_t need);

    platform::TcpConnection* conn_ = nullptr;  // socket mode when non-null
    char* stage_ = nullptr;                    // socket mode, from the heap
    size_t pos_ = 0;

    char* heap_ = nullptr;                     // buffer mode
    size_t heapLen_ = 0;
    size_t heapCap_ = 0;

    char* fixed_ = nullptr;                    // fixed-buffer mode
    size_t fixedLen_ = 0;
    size_t fixedCap_ = 0;
    bool overflowed_ = false;
    int  nameIndex_ = -1;   // >= 0: this sink is asking for that option's name (see requestName)
    int  optionIndex_ = -1; // >= 0: this sink is asking for that option object (see requestOption)
    size_t written_ = 0;    // every byte appended, whatever the mode
};

} // namespace mm
