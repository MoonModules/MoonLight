#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>

/// The fill routines as native machine code for the classic ESP32 and the S3.
///
/// The engine copies the bytes into instruction RAM and calls them through a function pointer, which is the path a bench run validates.
/// One instruction set per file, self-guarding: the file is always compiled and its body disappears elsewhere.
///
/// @technical
///
/// ## Every byte array is copied, never transcribed
///
/// The arrays are the assembler's own binary output rather than a hand-grouping of its disassembly.
/// This architecture's mixed instruction widths and byte order make that error-prone, and one such typo crashed a board before the rule was adopted.
///
/// ## The templates
///
/// ```
/// fill   00: entry a1,32      03: beqz.n a3,.done   05: movi.n a7,0 off   07: movi.n a8,0 i
///        09: movi a5,R <-0x0b 0c: movi a6,G <-0x0e  0f: movi a9,B <-0x11
///        12: add.n a10,a2,a7  14: s8i +0  17: s8i +1  1a: s8i +2
///        1d: add.n a7,a7,a4   1f: addi.n a8,a8,1    21: bne a8,a3,.loop  24: retw.n
///
/// anim   06: srli a6,a5,3 red = t>>3    08: movi a7,0xff   0b: and a6,a6,a7
///        0e: movi.n a7,0 green          10: movi.n a8,64 blue
/// ```
///
/// The animated one derives its color from the elapsed-time argument at runtime, so nothing needs patching and the same code animates as the host feeds a changing value.

namespace mm::moonlive {

#if defined(__XTENSA__)

// --- Xtensa (LX6/LX7: classic ESP32, ESP32-S3) ---------------------------------------
// Little-endian, mixed 24-bit and 16-bit (narrow) instructions; windowed ABI prologue/ epilogue `entry`/`retw` so a plain C function pointer calls it. Color bytes are the immediate byte of three wide `movi`s (forced wide so all patch identically), at kR/kG/kB.

/// The plain fill template: @xref{the-templates|its disassembly and patch offsets}.
static const uint8_t kXtensaFill[] = {
    0x36, 0x41, 0x00,        // entry a1, 32
    0x9c, 0xd3,              // beqz.n a3, .done
    0x0c, 0x07,              // movi.n a7, 0
    0x0c, 0x08,              // movi.n a8, 0
    0x52, 0xa0, 0x00,        // movi a5, R   (offset 0x0b)
    0x62, 0xa0, 0x00,        // movi a6, G   (offset 0x0e)
    0x92, 0xa0, 0x00,        // movi a9, B   (offset 0x11)
    0x7a, 0xa2,              // add.n a10, a2, a7
    0x52, 0x4a, 0x00,        // s8i a5, a10, 0
    0x62, 0x4a, 0x01,        // s8i a6, a10, 1
    0x92, 0x4a, 0x02,        // s8i a9, a10, 2
    0x4a, 0x77,              // add.n a7, a7, a4
    0x1b, 0x88,              // addi.n a8, a8, 1
    0x37, 0x98, 0xed,        // bne a8, a3, .loop
    0x1d, 0xf0,              // retw.n
};
static constexpr size_t kR = 0x0b, kG = 0x0e, kB = 0x11;   // color-immediate byte offsets

size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || cap < sizeof(kXtensaFill)) return 0;
    std::memcpy(out, kXtensaFill, sizeof(kXtensaFill));
    out[kR] = r;
    out[kG] = g;
    out[kB] = b;
    return sizeof(kXtensaFill);
}

/// The animated fill template: @xref{the-templates|its disassembly, and why nothing needs patching}.
static const uint8_t kXtensaAnim[] = {
    0x36, 0x41, 0x00, 0xac, 0x13, 0x50, 0x63, 0x41, 0x72, 0xa0, 0xff, 0x70,
    0x66, 0x10, 0x0c, 0x07, 0x4c, 0x08, 0x0c, 0x09, 0x0c, 0x0b, 0x9a, 0xa2,
    0x62, 0x4a, 0x00, 0x72, 0x4a, 0x01, 0x82, 0x4a, 0x02, 0x4a, 0x99, 0x1b,
    0xbb, 0x37, 0x9b, 0xed, 0x1d, 0xf0,
};

size_t emitAnimatedFill(uint8_t* out, size_t cap) {
    if (!out || cap < sizeof(kXtensaAnim)) return 0;
    std::memcpy(out, kXtensaAnim, sizeof(kXtensaAnim));
    return sizeof(kXtensaAnim);
}

#endif  // __XTENSA__

}  // namespace mm::moonlive
