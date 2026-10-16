/// `platform::zstdInflateAll`, decoding one headerless FSEQ zstd block through the vendored decompressor.
///
/// @technical
///
/// FPP/xLights omit the zstd frame header; the deprecated `ZSTD_decompressBlock` could read that directly but is not stable, so this synthesizes the minimal RFC 8878 header (`buildZstdFrameHeader`) for the stable `ZSTD_decompress()`.
///
/// The synthesized header matched the real zstd CLI's own, byte for byte; the same decoder, on real hardware, decoded two real zstd blocks correctly and repeatedly.
///
/// Not yet measured: the real heap and stack cost of `ZSTD_decompress()`'s own `ZSTD_DCtx`; log `dynamicBytes()` around a real call and record it in `Kconfig.projbuild`'s `MM_FSEQ_ZSTD` help text, its canonical home.
///
/// Inert when `CONFIG_MM_FSEQ_ZSTD` is unset: a board leaving the option off still links and reports as much.

#include "sdkconfig.h"

#if defined(CONFIG_MM_FSEQ_ZSTD)

#include "platform/platform.h"
#include "core/util/ZstdFrameHeader.h"

#include <cstring>

// zstddeclib.c (its own C translation unit, per esp32/main/CMakeLists.txt) declares no header of its own; these are its only two callees, given C linkage instead of pulling in the real zstd.h.
extern "C" {
size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t srcSize);
unsigned ZSTD_isError(size_t code);
}

namespace mm::platform {

bool zstdInflateAll(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t dstCap, size_t& outLen) {
    // Heap scratch, not stack: -Wframe-larger-than cannot see this platform-exclusive file, and the header-plus-block span can run to tens of KB.
    uint8_t hdr[kMaxZstdFrameHeaderBytes];
    const size_t hdrLen = buildZstdFrameHeader(dstCap, hdr);
    auto* scratch = static_cast<uint8_t*>(platform::alloc(hdrLen + srcLen));
    if (!scratch) return false;
    std::memcpy(scratch, hdr, hdrLen);
    std::memcpy(scratch + hdrLen, src, srcLen);
    const size_t n = ZSTD_decompress(dst, dstCap, scratch, hdrLen + srcLen);
    platform::free(scratch);
    if (ZSTD_isError(n) || n != dstCap) return false;
    outLen = n;
    return true;
}

} // namespace mm::platform

#else // !CONFIG_MM_FSEQ_ZSTD

#include "platform/platform.h"

namespace mm::platform {
bool zstdInflateAll(const uint8_t*, size_t, uint8_t*, size_t, size_t&) { return false; }
} // namespace mm::platform

#endif // CONFIG_MM_FSEQ_ZSTD
