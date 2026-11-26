#pragma once
/// The base every platform concern header builds on: the target's flags, the `MM_NONBLOCKING` mark and two compiler macros.

#include <cstdint>
#include <cstddef>
#include "platform_config.h"  // hasPsram / …: the target's flags the platform contract refers to
#include "nonblocking.h"      // MM_NONBLOCKING, which the platform's declarations carry

// Per-thread state; a target with one thread and no TLS support in its linker script defines it `static`.
#ifndef MM_THREAD_LOCAL
  #define MM_THREAD_LOCAL thread_local
#endif

// Format checking, where the compiler offers it: one toolchain parses the attribute as an unknown specifier and fails the whole class downstream.
#if defined(__GNUC__) || defined(__clang__)
  #define MM_PRINTF_FORMAT(fmt_arg, va_arg) __attribute__((format(printf, fmt_arg, va_arg)))
#else
  #define MM_PRINTF_FORMAT(fmt_arg, va_arg)
#endif
