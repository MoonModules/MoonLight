#pragma once

/// Keep a function out of line, so its callers share one copy rather than each inlining its own; a compiler fact, so a leaf header carries it, spelled for each compiler the tree builds with.
#if defined(_MSC_VER)
  #define MM_NOINLINE __declspec(noinline)
#else
  #define MM_NOINLINE __attribute__((noinline))
#endif
