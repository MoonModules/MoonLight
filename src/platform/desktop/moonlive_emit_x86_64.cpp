#include "core/moonlive/moonlive_emit.h"
#include <cstring>

/// The fill routines as native machine code for Windows, Linux and Intel macOS.
///
/// @technical
///
/// ## Two branches, one instruction set
///
/// The two calling conventions pass arguments in different registers, so the emitted prologue differs though the instruction set does not.
/// That axis is nested here rather than split into a third file, being a convention difference within one architecture.
/// Every byte array is verbatim assembler output, never hand-transcribed from a disassembly.
///
/// ## The light count is zero-extended first
///
/// This architecture leaves the upper half of a register holding a narrower argument undefined, and the loop bound below compares at full width.
/// So dirty upper bits run the loop far past the count and write past the end of the caller's buffer.
/// Whether the register arrives dirty depends on what ran before the call, which makes the corruption intermittent and lands it in whatever allocation follows.
/// A narrow register write zero-extends, so one instruction pins the bound.
///
/// The color bytes are the immediate of each store, at the offsets the patcher writes.
/// The second blob is a leaf function, so no shadow area is reserved and only volatile registers are used, needing no saves either.

namespace mm::moonlive {

#if defined(__x86_64__) && !defined(_WIN32) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

/// The blob for one calling convention, whose argument registers differ from the other's: @xref{the-light-count-is-zero-extended-first|why the first instruction is a widening move}.
static const uint8_t kX64[] = {
    0x89, 0xf6,                         // mov   esi, esi         (zero-extend nLights)
    0x85, 0xf6,                         // test  esi, esi
    0x74, 0x25,                         // je    .done (+0x25)
    0x0f, 0xb6, 0xca,                   // movzx ecx, dl          (stride)
    0x4d, 0x31, 0xc0,                   // xor   r8, r8           (i)
    0x4d, 0x31, 0xc9,                   // xor   r9, r9           (off)
    0x42, 0xc6, 0x04, 0x0f, 0x11,       // mov   byte [rdi+r9], R     ← off 0x13
    0x42, 0xc6, 0x44, 0x0f, 0x01, 0x22, // mov   byte [rdi+r9+1], G   ← off 0x19
    0x42, 0xc6, 0x44, 0x0f, 0x02, 0x33, // mov   byte [rdi+r9+2], B   ← off 0x1f
    0x49, 0xff, 0xc0,                   // inc   r8
    0x49, 0x01, 0xc9,                   // add   r9, rcx
    0x49, 0x39, 0xf0,                   // cmp   r8, rsi
    0x72, 0xe4,                         // jb    .loop (-0x1c)
    0xc3,                               // ret   .done:
};

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kX64)) return 0;
    std::memcpy(out, kX64, sizeof(kX64));
    out[0x13] = r;
    out[0x19] = g;
    out[0x1f] = b;
    return sizeof(kX64);
}

// x86-64 animated fill (assembled from anim_x64.s): red = (t>>3)&0xFF, green=0, blue=64.
// t arrives in ecx; nothing to patch — the color is computed from the runtime arg.
// Same `mov esi, esi` zero-extension of the loop bound as kX64 above.
static const uint8_t kX64Anim[] = {
    0x89, 0xf6,                         // mov   esi, esi        (zero-extend nLights)
    0x85, 0xf6,                         // test  esi, esi
    0x74, 0x2d,                         // je    .done (+0x2d)
    0x89, 0xc8,                         // mov   eax, ecx        (t)
    0xc1, 0xe8, 0x03,                   // shr   eax, 3          red = t>>3
    0x0f, 0xb6, 0xc0,                   // movzx eax, al
    0x44, 0x0f, 0xb6, 0xd2,             // movzx r10d, dl        (stride)
    0x4d, 0x31, 0xc0,                   // xor   r8, r8          (i)
    0x4d, 0x31, 0xc9,                   // xor   r9, r9          (off)
    0x42, 0x88, 0x04, 0x0f,             // mov   byte [rdi+r9], al    (red, dynamic)
    0x42, 0xc6, 0x44, 0x0f, 0x01, 0x00, // mov   byte [rdi+r9+1], 0   (green)
    0x42, 0xc6, 0x44, 0x0f, 0x02, 0x40, // mov   byte [rdi+r9+2], 64  (blue)
    0x49, 0xff, 0xc0,                   // inc   r8
    0x4d, 0x01, 0xd1,                   // add   r9, r10
    0x49, 0x39, 0xf0,                   // cmp   r8, rsi
    0x72, 0xe5,                         // jb    .loop (-0x1b)
    0xc3,                               // ret   .done:
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kX64Anim)) return 0;
    std::memcpy(out, kX64Anim, sizeof(kX64Anim));
    return sizeof(kX64Anim);
}
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(_WIN32) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

/// The same routine for the other calling convention, with its argument registers remapped: @xref{the-light-count-is-zero-extended-first|the same widening move}.
static const uint8_t kWin64[] = {
    0x89, 0xd2,                         // mov   edx, edx        (zero-extend nLights — see above)
    0x85, 0xd2,                         // test  edx, edx        (nLights == 0?)
    0x74, 0x26,                         // je    .done (+0x26)
    0x4d, 0x0f, 0xb6, 0xc0,             // movzx r8, r8b         (stride, zero-extended)
    0x4d, 0x31, 0xd2,                   // xor   r10, r10        (i)
    0x4d, 0x31, 0xdb,                   // xor   r11, r11        (off)
    0x42, 0xc6, 0x04, 0x19, 0x11,       // mov   byte [rcx+r11], R    ← off 0x14  .loop:
    0x42, 0xc6, 0x44, 0x19, 0x01, 0x22, // mov   byte [rcx+r11+1], G  ← off 0x1A
    0x42, 0xc6, 0x44, 0x19, 0x02, 0x33, // mov   byte [rcx+r11+2], B  ← off 0x20
    0x49, 0xff, 0xc2,                   // inc   r10
    0x4d, 0x01, 0xc3,                   // add   r11, r8         (off += stride)
    0x49, 0x39, 0xd2,                   // cmp   r10, rdx
    0x72, 0xe4,                         // jb    .loop (-0x1C)
    0xc3,                               // ret                        .done:
};

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kWin64)) return 0;
    std::memcpy(out, kWin64, sizeof(kWin64));
    out[0x14] = r;
    out[0x1a] = g;
    out[0x20] = b;
    return sizeof(kWin64);
}

// Win64 animated fill: red = (t>>3) & 0xFF (t arrives in r9d), green=0, blue=64. Same `mov edx, edx` zero-extension of the loop bound as kWin64 above.
static const uint8_t kWin64Anim[] = {
    0x89, 0xd2,                         // mov   edx, edx        (zero-extend nLights)
    0x85, 0xd2,                         // test  edx, edx
    0x74, 0x2e,                         // je    .done (+0x2e)
    0x44, 0x89, 0xc8,                   // mov   eax, r9d        (t)
    0xc1, 0xe8, 0x03,                   // shr   eax, 3          (red = t>>3)
    0x0f, 0xb6, 0xc0,                   // movzx eax, al
    0x4d, 0x0f, 0xb6, 0xc0,             // movzx r8, r8b         (stride)
    0x4d, 0x31, 0xd2,                   // xor   r10, r10        (i)
    0x4d, 0x31, 0xdb,                   // xor   r11, r11        (off)
    0x42, 0x88, 0x04, 0x19,             // mov   byte [rcx+r11], al   (red, dynamic)   .loop:
    0x42, 0xc6, 0x44, 0x19, 0x01, 0x00, // mov   byte [rcx+r11+1], 0  (green)
    0x42, 0xc6, 0x44, 0x19, 0x02, 0x40, // mov   byte [rcx+r11+2], 64 (blue)
    0x49, 0xff, 0xc2,                   // inc   r10
    0x4d, 0x01, 0xc3,                   // add   r11, r8
    0x49, 0x39, 0xd2,                   // cmp   r10, rdx
    0x72, 0xe5,                         // jb    .loop (-0x1B)
    0xc3,                               // ret                        .done:
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kWin64Anim)) return 0;
    std::memcpy(out, kWin64Anim, sizeof(kWin64Anim));
    return sizeof(kWin64Anim);
}

#endif  // __x86_64__

}  // namespace mm::moonlive
