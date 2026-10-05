#pragma once

/// The render path must not allocate or block, which clang checks transitively through overrides; a compiler fact, so a leaf header carries it without the platform contract.
#if defined(__clang__) && defined(__has_cpp_attribute) && __has_cpp_attribute(clang::nonblocking)
  // noexcept is part of the contract, since unwinding allocates and clang warns without it.
  #define MM_NONBLOCKING noexcept [[clang::nonblocking]]
#else
  #define MM_NONBLOCKING noexcept
#endif
