#include "core/moonlive/moonlive_emit.h"
#include <cstring>

/// The fill routines as native machine code, copied into an executable page and called through a function pointer.
///
/// One instruction set per file, self-guarding, matching the assembler beside it.
/// Every byte array is verbatim assembler output, never hand-transcribed from a disassembly.

namespace mm::moonlive {

#if defined(__aarch64__) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

// arm64 template (assembled from fill_arm64.s, verified with clang/objdump). 18 words. buf=x0, nLights=w1, cpl=w2. R/G/B live in `mov w4/w5/w6, #imm` at word indices 4,5,6.
static const uint32_t kArm64[] = {
    0x34000221,  // cbz   w1, .done
    0xd2800003,  // mov   x3, #0          (byte offset)
    0x12001c42,  // and   w2, w2, #0xff
    0x53001c42,  // uxtb  w2, w2          (cpl stride)
    0x52800004,  // mov   w4, #0          ← R patched: | (r<<5)
    0x52800005,  // mov   w5, #0          ← G patched: | (g<<5)
    0x52800006,  // mov   w6, #0          ← B patched: | (b<<5)
    0xd2800007,  // mov   x7, #0          (light index)
    0x38236804,  // strb  w4, [x0, x3]    .loop:
    0x91000468,  // add   x8, x3, #1
    0x38286805,  // strb  w5, [x0, x8]
    0x91000868,  // add   x8, x3, #2
    0x38286806,  // strb  w6, [x0, x8]
    0x910004e7,  // add   x7, x7, #1
    0x8b020063,  // add   x3, x3, x2
    0xeb0100ff,  // cmp   x7, x1
    0x54ffff03,  // b.lo  .loop          (-0x20)
    0xd65f03c0,  // ret                  .done:
};

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kArm64)) return 0;
    uint32_t code[sizeof(kArm64) / 4];
    std::memcpy(code, kArm64, sizeof(kArm64));
    // Patch the color immediates: mov wN,#imm encodes imm at bits [20:5]; the base word has imm=0 so OR-ing (imm<<5) sets it cleanly.
    code[4] = 0x52800004u | (static_cast<uint32_t>(r) << 5);
    code[5] = 0x52800005u | (static_cast<uint32_t>(g) << 5);
    code[6] = 0x52800006u | (static_cast<uint32_t>(b) << 5);
    std::memcpy(out, code, sizeof(code));
    return sizeof(code);
}

// arm64 animated fill (assembled from anim_arm64.s): red = (t>>3)&0xFF, green=0, blue=64. t arrives in w3; nothing to patch, the color is computed from the runtime arg.
static const uint32_t kArm64Anim[] = {
    0x34000241,  // cbz   w1, .done
    0x53037c64,  // lsr   w4, w3, #3      red = t>>3
    0x12001c84,  // and   w4, w4, #0xff
    0x52800005,  // mov   w5, #0          green
    0x52800806,  // mov   w6, #64         blue
    0xd2800003,  // mov   x3, #0          off
    0x12001c42,  // and   w2, w2, #0xff
    0x53001c42,  // uxtb  w2, w2          stride
    0xd2800007,  // mov   x7, #0          i
    0x38236804,  // strb  w4, [x0, x3]    .loop:
    0x91000468,  // add   x8, x3, #1
    0x38286805,  // strb  w5, [x0, x8]
    0x91000868,  // add   x8, x3, #2
    0x38286806,  // strb  w6, [x0, x8]
    0x910004e7,  // add   x7, x7, #1
    0x8b020063,  // add   x3, x3, x2
    0xeb0100ff,  // cmp   x7, x1
    0x54ffff03,  // b.lo  .loop
    0xd65f03c0,  // ret   .done:
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kArm64Anim)) return 0;
    std::memcpy(out, kArm64Anim, sizeof(kArm64Anim));
    return sizeof(kArm64Anim);
}

#endif  // __aarch64__

}  // namespace mm::moonlive
