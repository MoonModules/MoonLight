/// The out-of-line half of ScratchBuffer: the resize that owns memory.

#include "core/util/ScratchBuffer.h"

#include "core/module/MoonModule.h"    // full definition: the dynamic-bytes readout
#include "platform/platform.h"  // alloc / free

#include <cstring>              // memset, memcpy

namespace mm {

ScratchBufferBase::~ScratchBufferBase() {
    resizeBytes(0);   // frees raw_ + subtracts our bytes from owner_; ModuleResource's destructor unlinks it
}

bool ScratchBufferBase::resizeBytes(std::size_t bytes) {
    if (bytes == bytes_) return raw_ != nullptr || bytes == 0;  // no-op fast path

    const HeapBytes old = heapOf(raw_, bytes_);
    if (raw_) { platform::free(raw_); raw_ = nullptr; }
    bytes_ = 0;

    if (bytes > 0) {
        raw_ = platform::alloc(bytes);
        if (raw_) {
            std::memset(raw_, 0, bytes);
            bytes_ = bytes;
        }
        // alloc failed → raw_ stays null, bytes_ stays 0 (the caller's `if (!buf)` guard fires)
    }

    owner_.moveDynamicBytes(old, heapOf(raw_, bytes_));   // keep owner_'s memory readout current
    return bytes_ == bytes;
}

bool ScratchBufferBase::ensureBytes(std::size_t bytes, std::size_t minBytes) {
    if (bytes <= bytes_) return true;
    std::size_t grown = bytes_ ? bytes_ : minBytes;
    while (grown < bytes) grown *= 2;
    // The doubled size first, then the exact one, which is how a nearly full heap still takes one more row.
    return growTo(grown) || (grown != bytes && growTo(bytes));
}

bool ScratchBufferBase::growTo(std::size_t want) {
    if (!platform::fitsWithReserve(want)) return false;
    void* fresh = platform::alloc(want);
    if (!fresh) return false;
    if (raw_) std::memcpy(fresh, raw_, bytes_);
    std::memset(static_cast<uint8_t*>(fresh) + bytes_, 0, want - bytes_);
    owner_.moveDynamicBytes(heapOf(raw_, bytes_), heapOf(fresh, want));
    if (raw_) platform::free(raw_);
    raw_ = fresh;
    bytes_ = want;
    return true;
}

} // namespace mm
