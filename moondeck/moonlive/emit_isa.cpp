#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

/// Compile a MoonLive script through ONE REAL BACKEND on the development machine, and print the bytes it emits.
/// Each backend is guarded by its target macro (`__XTENSA__`, `__riscv`, `__ARM_ARCH_7EM__`, `__aarch64__`) so on this host it normally compiles to nothing. Defining the macro and including the sources is what lets the host run a device emitter.
/// The firmware build never sees this file, so there is still exactly one definition of each backend in the shipped binary.
///
/// Which backend is selected by -DMM_EMIT_<ISA> from disasm.py. One file rather than one per ISA: the whole body below is identical for every backend, and only the includes differ.

#if defined(MM_EMIT_XTENSA)
#define __XTENSA__ 1
#include "platform/esp32/moonlive_asm_xtensa.h"
#include "platform/esp32/moonlive_asm_xtensa.cpp"
#elif defined(MM_EMIT_RISCV)
// The RISC-V backend tests `__riscv` only, so defining it is enough to expose the whole file.
#define __riscv 1
#include "platform/esp32/moonlive_asm_riscv.h"
#include "platform/esp32/moonlive_asm_riscv.cpp"
#elif defined(MM_EMIT_THUMB2)
#define __ARM_ARCH_7EM__ 1
#include "platform/teensy/moonlive_asm_thumb2.h"
#include "platform/teensy/moonlive_asm_thumb2.cpp"
#elif defined(MM_EMIT_ARM64)
#define __aarch64__ 1
#include "platform/desktop/moonlive_asm_host.h"
#include "platform/desktop/moonlive_asm_arm64.cpp"
#elif defined(MM_EMIT_X86_64)
// This ISA is selected by the host's own architecture, so it runs only on that host, where the device ISAs cross-emit because no host defines their macros.
#if !defined(__x86_64__) && !defined(_M_X64)
#error "MM_EMIT_X86_64 requires an x86-64 host; run --isa x86_64 on an x86-64 machine"
#endif
#include "platform/desktop/moonlive_asm_host.h"
#include "platform/desktop/moonlive_asm_x86_64.cpp"
#else
#error "define MM_EMIT_XTENSA, MM_EMIT_RISCV, MM_EMIT_THUMB2, MM_EMIT_ARM64 or MM_EMIT_X86_64"
#endif

// The lowerer body, with the emit seam it expects.
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveBuiltins.h"
// The CANONICAL declaration, rather than a copy. This tool once redeclared a three-argument lowerToBytes that linked only because it never passes `squeeze`, so the tool's view of the backend could drift from the backend's own.
#include "core/moonlive/moonlive_emit.h"

// Each backend carries its own lowerToBytes inside its arch guard, so the asm file above already brought it in. What remains here is taking the arch macros back off.
#if defined(MM_EMIT_XTENSA)
#undef __XTENSA__
#elif defined(MM_EMIT_RISCV)
#undef __riscv
#elif defined(MM_EMIT_THUMB2)
#undef __ARM_ARCH_7EM__
#elif defined(MM_EMIT_ARM64)
#undef __aarch64__
#endif

#include "core/moonlive/MoonLiveCompiler.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
using namespace mm;
int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }";
    // Sized the way the ENGINE sizes it, from the script's own token count, so the one place that measures emitted size agrees with the device. A fixed 4 KB refuses ripples.mle and rose.mll on RISC-V, which emits ~1.3x Xtensa.
    static uint8_t buf[moonlive::kCodeCap];
    // Which binding to compile as. The system-variable tables are different vocabularies rather than nested supersets: a layout keeps the coordinate names as ordinary counters, so each binding compiles against its own table.
    const char* binding = argc > 2 ? argv[2] : "layout";
    const auto sysvars = std::strcmp(binding, "modifier") == 0 ? moonlive::modifierSysVars()
                       : std::strcmp(binding, "effect")   == 0 ? moonlive::effectSysVars()
                                                               : moonlive::layoutSysVars();
    // A string pool, as the engine supplies one: `addControl("name", ...)` interns its label there and the emitted code carries a pointer to it. Static so the pointers stay valid while the bytes below are dumped.
    static char strings[moonlive::CompileResult::kStringPool];
    auto r = moonlive::compileSource(src, moonlive::lightBuiltins(), sysvars, buf, moonlive::codeCapFor(moonlive::countTokens(src)),
                                     nullptr, nullptr, strings, sizeof(strings));
    if (!r.ok) { printf("compile failed: %s\n", r.error); return 1; }
    printf("# %s\n# %zu bytes\n", src, r.len);
    for (size_t i = 0; i < r.len; i++) printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
    printf("\n");
    return 0;
}
