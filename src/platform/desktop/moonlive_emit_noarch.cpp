#include "core/moonlive/moonlive_emit.h"

/// The third case beside the arm64 and x86-64 emitters: a host we have no machine code for.
///
/// The assembler beside it says when this is reached.

#if !((defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT))

namespace mm::moonlive {

size_t emitFill(uint8_t*, size_t, uint8_t, uint8_t, uint8_t) { return 0; }
size_t emitAnimatedFill(uint8_t*, size_t) { return 0; }

}  // namespace mm::moonlive

#endif
