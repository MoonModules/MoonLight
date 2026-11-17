#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

/// The third case beside the arm64 and x86-64 backends: an architecture we have none for.
///
/// @technical
///
/// ## Two ways to land here
///
/// An instruction set nobody has written an assembler for, and a deliberate no-JIT build.
/// That build is a pre-merge gate: it is how a contributor sees what an unsupported desktop sees.
/// Both must link and run everything that is not a script.
///
/// ## Why a desktop may have no backend at all
///
/// The ESP32 side fails the build for an unknown instruction set, every chip being Xtensa or RISC-V, so a third is an unfinished port.
/// A desktop legitimately has this case.

#if !((defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT))

namespace mm::moonlive {

// Refusing rather than emitting something that cannot run: MoonLive::compile then reports a failure and a scripted module renders dark, the same path a too-large or unparseable script takes.
size_t lowerToBytes(IrProgram&, uint8_t*, size_t, const RegBudget*) { return 0; }

}  // namespace mm::moonlive

#endif
