/// The bodies of JsonSink.h with more than a line of logic, so a change to how a document is written rebuilds one object.

#include "core/util/JsonSink.h"

namespace mm {

JsonSink::~JsonSink() {
    if (heap_) platform::free(heap_);
    if (stage_) platform::free(stage_);
}

void JsonSink::append(const char* s, size_t len) {
    if (!s) return;
    written_ += len;
    if (conn_ && !stage_) { conn_->write(reinterpret_cast<const uint8_t*>(s), len); return; }
    if (conn_) appendStaged(s, len);
    else if (fixed_) appendFixed(s, len);
    else appendHeap(s, len);
}

void JsonSink::appendStaged(const char* s, size_t len) {
    while (len > 0) {
        if (pos_ == STAGE_SIZE) flushStage();
        const size_t n = len < STAGE_SIZE - pos_ ? len : STAGE_SIZE - pos_;
        std::memcpy(stage_ + pos_, s, n);
        pos_ += n;
        s += n;
        len -= n;
    }
}

void JsonSink::appendFixed(const char* s, size_t len) {
    if (len == 0) return;
    if (overflowed_ || fixedCap_ == 0) { overflowed_ = true; return; }
    // One byte reserved for the terminator; what fits is kept and the flag trips for the rest.
    const size_t room = fixedCap_ - 1 - fixedLen_;
    const size_t n = len < room ? len : room;
    std::memcpy(fixed_ + fixedLen_, s, n);
    fixedLen_ += n;
    fixed_[fixedLen_] = '\0';
    if (n < len) overflowed_ = true;
}

void JsonSink::appendHeap(const char* s, size_t len) {
    if (len == 0) return;
    // Out of memory is flagged as the fixed path does: a truncated document is worse than none (JsonSink.h, "Overflow is a flag").
    if (overflowed_ || !ensureHeap(heapLen_ + len)) { overflowed_ = true; return; }
    std::memcpy(heap_ + heapLen_, s, len);
    heapLen_ += len;
    // Terminated after every append, so the data stays a valid string even when a caller skips the length.
    heap_[heapLen_] = '\0';
}

void JsonSink::appendf(const char* fmt, ...) {
    char frag[FRAG_MAX];
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(frag, sizeof(frag), fmt, ap);
    va_end(ap);
    if (n >= 0 && static_cast<size_t>(n) < sizeof(frag)) append(frag, static_cast<size_t>(n));
    else if (n >= 0) appendLong(fmt, ap2, static_cast<size_t>(n));
    va_end(ap2);
}

void JsonSink::appendLong(const char* fmt, va_list ap, size_t n) {
    // The fixed mode stays within its buffer: an allocation here would hide the problem (JsonSink.h, "Overflow is a flag").
    if (fixed_) { overflowed_ = true; return; }
    // The growing modes format into an exactly-sized block, so a long fragment arrives whole.
    char* big = static_cast<char*>(platform::alloc(n + 1));
    if (!big) { overflowed_ = true; return; }
    std::vsnprintf(big, n + 1, fmt, ap);
    append(big, n);
    platform::free(big);
}

void JsonSink::writeJsonString(const char* s) {
    // A character at a time, so there is no truncation ceiling; the standard requires escaping the quote, the backslash and every control byte.
    if (!s) s = "";
    append("\"", 1);
    for (char esc[8]; *s; s++) append(esc, jsonEscapeByte(static_cast<unsigned char>(*s), esc));
    append("\"", 1);
}

char* JsonSink::detach() {
    if (conn_ || fixed_) return nullptr;
    char* out = heap_;
    heap_ = nullptr;
    heapCap_ = heapLen_ = 0;
    return out;
}

void JsonSink::clear() {
    if (conn_ || fixed_) return;
    if (heap_) platform::free(heap_);
    heap_ = nullptr;
    heapCap_ = heapLen_ = 0;
    written_ = 0;
    overflowed_ = false;
}

void JsonSink::flushStage() {
    if (conn_ && pos_ > 0) {
        conn_->write(reinterpret_cast<const uint8_t*>(stage_), pos_);
        pos_ = 0;
    }
}

size_t JsonSink::growthTarget(size_t need) const {
    size_t want = heapCap_ == 0 ? 2048 : heapCap_ * 2;
    while (want < need + 1) want *= 2;
    return want;
}

bool JsonSink::ensureHeap(size_t need) {
    if (need + 1 <= heapCap_) return true;
    size_t want = growthTarget(need);
    // In quarters rather than to the bare minimum: a minimal step thrashes (JsonSink.h, "Growth steps down").
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

} // namespace mm
