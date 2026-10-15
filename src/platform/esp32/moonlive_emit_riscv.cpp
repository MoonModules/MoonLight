#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>

/// The fill routines as native machine code for the P4 and S31.
///
/// The engine copies the bytes into instruction RAM and calls them through a function pointer, which is the path a bench run validates.
/// One instruction set per file, self-guarding: the file is always compiled and its body disappears elsewhere.
//
// Every byte array below is the VERBATIM assembler output (objcopy'd from .text), never hand-transcribed from a disassembly: a hand-grouped Xtensa byte once caused a StoreProhibited crash, and the rule since is copy the raw blob.

namespace mm::moonlive {

#if defined(__riscv)

// Little-endian with fixed-width instructions, assembled without the compressed forms so every word is uniform and patching is simple.
// The standard calling convention, and the color loads sit at fixed word indices whose immediate occupies the top bits, so a patch is the base combined with the shifted value.
// Taken verbatim from the assembler's own output.

// Disassembly (word index : instruction): 0: beqz a1,.done   1: li t0,0(off)  2: li t1,0(i)  3: li t2,R  4: li t3,G  5: li t4,B 6: add t5,a0,t0  7: sb t2,0(t5)  8: sb t3,1(t5)  9: sb t4,2(t5) 10: addi t1,t1,1  11: add t0,t0,a2  12: bne t1,a1,.loop   13: ret
static const uint8_t kRiscvFill[] = {
    0x63, 0x8a, 0x05, 0x02,  0x93, 0x02, 0x00, 0x00,  0x13, 0x03, 0x00, 0x00,
    0x93, 0x03, 0x10, 0x01,  0x13, 0x0e, 0x20, 0x02,  0x93, 0x0e, 0x30, 0x03,
    0x33, 0x0f, 0x55, 0x00,  0x23, 0x00, 0x7f, 0x00,  0xa3, 0x00, 0xcf, 0x01,
    0x23, 0x01, 0xdf, 0x01,  0x13, 0x03, 0x13, 0x00,  0xb3, 0x82, 0xc2, 0x00,
    0xe3, 0x14, 0xb3, 0xfe,  0x67, 0x80, 0x00, 0x00,
};
// li t2/t3/t4, 0 (zero-immediate bases) at word indices 3/4/5; patch | (color<<20).
static constexpr uint32_t kRvLiBaseR = 0x00000393u;  // li t2,0
static constexpr uint32_t kRvLiBaseG = 0x00000e13u;  // li t3,0
static constexpr uint32_t kRvLiBaseB = 0x00000e93u;  // li t4,0

static void putWord(uint8_t* p, uint32_t w) {        // little-endian store
    p[0] = uint8_t(w); p[1] = uint8_t(w >> 8); p[2] = uint8_t(w >> 16); p[3] = uint8_t(w >> 24);
}

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kRiscvFill)) return 0;
    std::memcpy(out, kRiscvFill, sizeof(kRiscvFill));
    putWord(out + 12, kRvLiBaseR | (uint32_t(r) << 20));   // word 3
    putWord(out + 16, kRvLiBaseG | (uint32_t(g) << 20));   // word 4
    putWord(out + 20, kRvLiBaseB | (uint32_t(b) << 20));   // word 5
    return sizeof(kRiscvFill);
}

// Animated: red=(t>>3)&0xFF computed at runtime (srli + zext.b on a3), green=0, blue=64. Nothing to patch. Verbatim from riscv32-esp-elf-as.
static const uint8_t kRiscvAnim[] = {
    0x63, 0x8c, 0x05, 0x02,  0x93, 0xd3, 0x36, 0x00,  0x93, 0xf3, 0xf3, 0x0f,
    0x13, 0x0e, 0x00, 0x00,  0x93, 0x0e, 0x00, 0x04,  0x93, 0x02, 0x00, 0x00,
    0x13, 0x03, 0x00, 0x00,  0x33, 0x0f, 0x55, 0x00,  0x23, 0x00, 0x7f, 0x00,
    0xa3, 0x00, 0xcf, 0x01,  0x23, 0x01, 0xdf, 0x01,  0x13, 0x03, 0x13, 0x00,
    0xb3, 0x82, 0xc2, 0x00,  0xe3, 0x14, 0xb3, 0xfe,  0x67, 0x80, 0x00, 0x00,
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kRiscvAnim)) return 0;
    std::memcpy(out, kRiscvAnim, sizeof(kRiscvAnim));
    return sizeof(kRiscvAnim);
}

#endif  // __riscv

}  // namespace mm::moonlive
