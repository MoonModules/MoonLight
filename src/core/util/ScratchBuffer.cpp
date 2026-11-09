/// The out-of-line half of ScratchBuffer: the resize that owns memory.

#include "core/util/ScratchBuffer.h"

#include "core/module/MoonModule.h"    // full definition: the dynamic-bytes readout
#include "platform/platform.h"  // alloc / free

#include <cstring>              // memset

namespace mm {

ScratchBufferBase::~ScratchBufferBase() {
    resizeBytes(0);   // frees raw_ + subtracts our bytes from owner_; ModuleResource's destructor unlinks it
}

bool ScratchBufferBase::resizeBytes(std::size_t bytes) {
    if (bytes == bytes_) return raw_ != nullptr || bytes == 0;  // no-op fast path

    if (raw_) { platform::free(raw_); raw_ = nullptr; }

    const std::size_t old = bytes_;
    bytes_ = 0;

    if (bytes > 0) {
        raw_ = platform::alloc(bytes);
        if (raw_) {
            std::memset(raw_, 0, bytes);
            bytes_ = bytes;
        }
        // alloc failed → raw_ stays null, bytes_ stays 0 (the caller's `if (!buf)` guard fires)
    }

    // Keep owner_'s dynamic-bytes readout current: adjust by the signed delta.
    owner_.addDynamicBytes(static_cast<std::ptrdiff_t>(bytes_) -
                           static_cast<std::ptrdiff_t>(old));
    return bytes_ == bytes;
}

} // namespace mm
