#pragma once

/// @module format
/// @also SystemModule, NetworkModule, AudioService

/// @defgroup format Formatting on the render path
/// @{
/// Formatting into a fixed buffer, from a function the hot-path check can see through.
///
/// @moreinfo
///
/// ## What the promise rests on
///
/// `snprintf` carries no annotation in libc, so clang cannot infer that a call to it is nonblocking.
/// Every status line written on a tick then reported as a blocking call: fifty of them, most of what the hot-path report contained.
///
/// The promise is about the FORMATS this project uses rather than about `snprintf` in general.
/// An integer or string conversion writes through the caller's buffer and allocates nowhere.
/// A floating-point one reaches `_dtoa_r` and then `_malloc_r` on the ESP32's newlib, so a `%f` does not belong on a tick.
///
/// ## What enforces it
///
/// The signature cannot: a C varargs function accepts any conversion, and no C++ type system reaches into a format string.
/// So the annotation is held by `check_nonblocking.py`, which reads every `formatTo` call in `src/` and fails the run on a float conversion.
/// It reads the whole call rather than its first line, because seven sites here put the format string on the line below.
/// A control test plants a `%f` and confirms the check fires, since a checker that reads nothing looks exactly like a clean tree.

#include "platform/platform.h"   // MM_NONBLOCKING

#include <cstdarg>
#include <cstdio>

namespace mm {

/// Write a formatted string into `buf`, as `snprintf` does, and promise it neither blocks nor allocates.
MM_PRINTF_FORMAT(3, 4)
inline int formatTo(char* buf, size_t size, const char* fmt, ...) MM_NONBLOCKING {
    va_list ap;
    va_start(ap, fmt);
    // THE SEAM: beyond this line the compiler trusts the annotation, and the appendix above says what it rests on.
#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wfunction-effects"
#endif
    const int n = std::vsnprintf(buf, size, fmt, ap);
#if defined(__clang__)
    #pragma clang diagnostic pop
#endif
    va_end(ap);
    return n;
}

/// @}

}  // namespace mm
