#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_xtensa.h"

#include <cstring>

#if defined(__XTENSA__)   // the Xtensa assembler is only built for Xtensa targets

/// @defgroup moonlive_asm_xtensa_impl MoonLive Xtensa encodings
/// Named instructions encoded once, composed by the shared lowering.
///
/// Encodings are verified against the assembler rather than transcribed from a disassembly.
///
/// @moreinfo
///
/// ## The conventions this encoder assumes
///
/// The architecture is little-endian with mixed wide and narrow instructions.
/// The first four registers map to the windowed argument registers and the rest to the next block.
/// For the short conditional branches used here the offset is measured from four bytes past the instruction.
/// And every one of them places that byte at the same position, so a single fixup kind covers them all.
///
/// ## The top of every frame belongs to the hardware
///
/// The windowed convention reserves save areas at the top of every frame that makes a rotating call.
/// The overflow handler writes two bands there from two different stack pointers: an older frame's first four registers including its return address, and this routine's own next four.
/// So the top thirty-two bytes are the hardware's and never ours, and the compiler obeys the same rule.
///
/// ## Both bands were found on the bench, separately
///
/// Reserving nothing put the parked arguments under the first band, and any script at all reset the board with the return address overwritten.
/// Reserving only half left the highest slot under the second band, which is written on any interrupt landing while a call is in flight.
/// Shallow calls rarely coincided with one and worked, while the deep library chains of one effect gave every tick a wide window to hit.
/// The parked pointer then came back as an expression temporary.
/// The corruption is spatial rather than timed, so no code sequence can dodge it and only the layout can.
/// Never seen on the other architectures, which have no register window and no hardware-owned frame bytes.
///
/// ## The reserve is derived, never written down twice
///
/// It follows the widest call this assembler emits, taken from that instruction itself.
/// A wider call rotates the window further and its save area grows to match.
/// So a future one with a hand-held number would put the top slot back under the band and resurrect this with every static check still green.
///
/// ## Encodings are words, not memory bytes
///
/// Every encoding here is emitted as a word, and a first version built the bytes by hand from a disassembly listing.
/// The two toolchains print differently: one shows the instruction word and the other shows memory byte order.
/// Reading word hex as memory bytes reversed every instruction, and one reversed shift decoded as a load that clobbered the stack pointer, hanging the board hard enough for the watchdog.
/// The host tests can never execute these bytes, so only a device shows it.
///
/// ## The narrow add encodes one to fifteen, and zero means minus one
///
/// So a caller asking to add nothing must emit no instruction at all, and anything outside that range needs the wide form.
/// Every caller passed one until an array whose base offset was zero asked for it, which emitted a subtract and shifted every element access down a byte.
/// That reached a device as a fixture that stayed dark while all host tests passed.
///
/// ## Constants wider than a byte, and negative ones
///
/// The wide move's field is signed and is the only encoding here that holds a negative constant.
/// The compiler emits one to express subtraction, and building that through the zero-extended path would make every subtraction correct only modulo a byte.
/// Invisible in a stored color, silently fatal in a bounds-guarded index or a call argument.
/// A negative beyond that field's reach has no encoding here, and falling through would materialize a different number in silence, so the compile fails instead.
/// The older positive path masked to sixteen bits silently, invisible while the language capped literals there and the first thing a fixed-point literal stepped on.
///
/// ## The narrow slot access reaches only part of the arena
///
/// Its offset field is four bits counting whole words, so it reaches a fixed distance and no further.
/// Past that the field overflows into the neighboring nibbles and the instruction silently addresses somewhere else entirely.
///
/// That is not a theoretical bound.
/// The control arena puts a script's own members low and the HOST SYSTEM VARIABLES above them.
/// So the first system variable wrapped the field to zero and read the script's first member instead.
/// Every two-dimensional script on this family therefore saw its dimensions as whatever that member held, usually nothing.
/// One shipped script drew its row at the top forever, and another looped zero times.
/// It reproduced on two boards and was correct on the other architecture and on the host.
/// No test caught it, the host using a different backend and the golden bytes never pinning such a load.
///
/// ## The wide forms cover the rest
///
/// They carry a much larger word-scaled offset spanning the whole arena, so they are used whenever the narrow one cannot reach.
/// The narrow one is kept for what it does fit, a member access being the common case.
///
/// ## Which register builds the call target is load-bearing
///
/// A rotating call shifts the window, so the caller's upper registers become the callee's lower ones, and one of them IS the callee's stack pointer.
/// Building the target address through that register handed the callee a fragment of a function pointer as its stack pointer.
/// Its own prologue then sized a frame from garbage and stored through it, landing anywhere in memory, including over this routine's own parked arguments.
/// That is the null arena pointer a script later read.
/// It only bit scripts whose loop both makes a call and reads a system variable, both halves having to be present to notice.
/// The safe temporary maps to an argument slot a three-argument function never reads, which is why it is this assembler's scratch.
///
/// ## The result is parked in the frame, not a register
///
/// It used to be stashed in a register that the rotation maps onto one of the callee's own arguments, so the callee overwrote the stash while it ran.
/// What was then moved to the destination was whatever the callee happened to leave there, measured in a crash dump as a script value reaching the return-address register.
/// Those registers are safe as scratch only BEFORE a call, never across it, and the frame slot sits in bytes the call already owns, so this costs nothing extra.
///
/// ## Conditional branches are relaxed, always
///
/// A conditional branch here carries a single signed byte of displacement, which a loop body outgrows easily once spill traffic is in it.
/// Truncating that silently retargets the branch into the middle of the program, so the choice is to relax or to refuse.
/// This is the textbook relaxation every compiler does, and the fixed longer form skips the iterate-to-convergence step.
/// That is worth a few bytes per branch on a cold path, against not having to reason about shifting offsets.

namespace mm::moonlive {

// R0..R3 → a2..a5 (the windowed-ABI args); R4..R11 → a6..a11, a14, a15. a12/a13 are internal scratch (store8 address, branchIfZero zero-reg, call result stash), so not in the pool.
static constexpr uint8_t kXtReg[kRegCount] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
// Map a virtual register to a machine one, bounds-checked: the inline operations address their scratch past the end of the map when a program uses every register.
// An out-of-bounds read returns whatever byte follows, so the emitted instruction names a register by accident, which once produced a window register no program had business touching.
// Clamped to the last real entry, so a mistake is wrong but safe; the assertion below and the reservation in the lowering are what prevent it entirely.
static uint8_t ar(Reg r) { return kXtReg[r < kRegCount ? r : kRegCount - 1]; }

// The map, for the codegen test to assert against. Exposed rather than copied into the test, so the property being checked cannot drift away from the map it is about.
const uint8_t* xtRegMap(uint8_t& count) { count = kRegCount; return kXtReg; }

// A scratch register that is ALSO a vreg silently corrupts values, see the RISC-V backend, where kScratchFn aliased vreg R12 and every call returned a stale value. Checked here so the map can never grow over a scratch.
constexpr bool xtScratchOutsideMap() {
    // a12/a13 are call() scratch; a14/a15 carry the retw.n return linkage of our own `entry` frame. Both classes are fatal as vregs, and the second one only faults once a script makes a CALL, which is why it survived every effect and killed every layout.
    constexpr uint8_t scratch[] = {12, 13, 14, 15};
    for (uint8_t r : kXtReg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(xtScratchOutsideMap(), "a scratch or window register is also a vreg — calls corrupt it");


void XtensaAssembler::emit(const uint8_t* p, size_t n) {
    // !buf_ covers a failed allocation: the compile then fails cleanly at overflowed() instead of writing through a null pointer.
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}
void XtensaAssembler::emit2(uint16_t w) {
    const uint8_t b[2] = {uint8_t(w), uint8_t(w >> 8)}; emit(b, 2);
}
void XtensaAssembler::emit3(uint32_t w) {
    const uint8_t b[3] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16)}; emit(b, 3);
}

// The prologue: a frame with room for the window rotation, so any program can call a built-in.
// Spill slots start above what a call uses and the frame simply grows, the whole-routine frame already existing, so spilling costs a bigger immediate on one instruction and nothing else.
// The frame pointer is preserved across a call by the convention, which is why a slot read afterwards still finds its value.
// One word inside the call's own save area holds the return value, since a register cannot carry a result across a window rotation.
static constexpr uint8_t  kResultSlot = 8;    // byte offset 32
static constexpr uint16_t kFrameBase  = 48;   // first byte past the bytes call() reserves
static constexpr uint16_t kSlotStride = 4;

// The save areas the convention reserves at the top of every frame that makes a rotating call: @xref{the-top-of-every-frame-belongs-to-the-hardware|the two bands, and what each cost}. The size is derived from the widest call this assembler emits rather than written down twice.
static constexpr uint32_t kCallxOpcode = 0x0000e0u;   // callx8 a8, the one call we emit

/// Bytes the overflow handler may write at the top of a frame, given its widest call: @xref{the-reserve-is-derived-never-written-down-twice|how the size follows the instruction}.
static constexpr uint32_t windowSaveReserveFor(uint32_t callxOpcode) {
    return 16u * ((callxOpcode >> 4) & 0x3u);
}
static constexpr uint32_t kWindowSaveReserve = windowSaveReserveFor(kCallxOpcode);
static_assert(kWindowSaveReserve == 32,
              "callx width changed: the frame reserve moved with it, so re-check the frame "
              "layout and MM_ISA_RESERVED_TOP in the codegen test before accepting this");

// ENTRY is a BRI12-format instruction: op0=6, n=3, s=the base register, and the 12-bit immediate at bits 12..23 counts EIGHT-byte units. `entry a1, 48` is therefore 0x006136.
void XtensaAssembler::prologue(uint8_t slots) {
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // Rounded up to 8 because the immediate counts 8-byte units; the ABI additionally wants the frame 16-byte aligned, and 48 + a multiple of 16 keeps that. kWindowSaveReserve is added ON TOP of the slots so the highest slot still ends below the hardware's two save bands.
    const uint32_t bytes =
        (kFrameBase + uint32_t(slots) * kSlotStride + kWindowSaveReserve + 15u) & ~15u;
    emit3(0x000136u | ((bytes / 8u) << 12));
}
void XtensaAssembler::epilogue() { emit2(0xf01du); }     // retw.n

// s32i/l32i aR, a1, #off, the offset field counts 4-byte words, so a slot index maps straight onto it. No teardown counterpart: `entry`'s frame is released by retw.n, so unlike the RISC-V and arm64 backends there is nothing for an epilogue to undo.
void XtensaAssembler::spillStore(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint8_t off4 = static_cast<uint8_t>((kFrameBase + slot * kSlotStride) / 4);
    const uint8_t b[3] = {uint8_t((ar(r) << 4) | 0x2), 0x61, off4};
    emit(b, 3);
}
void XtensaAssembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint8_t off4 = static_cast<uint8_t>((kFrameBase + slot * kSlotStride) / 4);
    const uint8_t b[3] = {uint8_t((ar(r) << 4) | 0x2), 0x21, off4};
    emit(b, 3);
}

Label XtensaAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = kUnbound;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit
    Label l = labelCount_++; labelPos_[l] = kUnbound; return l;
}
void XtensaAssembler::bind(Label l) { if (len_ >= kUnbound) overflow_ = true; else if (l < kMaxLabels) labelPos_[l] = static_cast<CodePos>(len_); }

// Record a pending branch fixup, guarding the fixed table (overflow_ rather than an OOB write).
void XtensaAssembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

// A call to a function in this block, the script-to-script call.
// The direct form rather than the indirect one, the target being a label whose displacement is encoded and patched, with no address to build in a register.
// That is why a local call is a handful of bytes where a host call is forty lines.
// The rotating form rather than the flat one, since this assembler emits the windowed prologue and the other convention would hand a callee a frame it never allocated.
void XtensaAssembler::callLabel(Label l, Reg d, bool take) {
    // The same preservation call() gives a builtin. The window protects a2..a7 (R0..R5) by itself; a8..a11 (R6..R9) become the callee's a0..a3 and are overwritten, so they go to the frame first, in the slots call() owns. Without this a value computed before the call and used after it, `a() + b()`, read the second call's result twice.
    auto s32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x61,off4}; emit(enc,3); };
    auto l32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x21,off4}; emit(enc,3); };
    s32i(8, 4); s32i(9, 5); s32i(10, 6); s32i(11, 7);
    // Pass the host arguments on (the contract is with IrOp::CallScript in core). The Xtensa delta: call8 ROTATES the window by 8, so the callee's a2..a6 are this routine's a10..a14 and the arguments are written to the OUTGOING window, not to a2..a6, which stay this frame's own.
    for (uint8_t v = 0; v < kHostArgSlots; v++) {
        const uint8_t off4 = static_cast<uint8_t>((kFrameBase + hostArgSlot(v) * kSlotStride) / 4);
        const uint8_t enc[3] = {static_cast<uint8_t>(((10 + v) << 4) | 0x2), 0x21, off4};
        emit(enc, 3);                                      // l32i a(10+v), a1, #slot
    }
    addFixup(len_, l, FixKind::Call);
    // CALL8 is format CALL: the low six bits are 0x25 (op0 = 5, n = 2) and an 18-bit signed offset occupies bits 6..23, counting FOUR-BYTE UNITS from the call's PC rounded down to a 4-byte boundary. Emitted as a placeholder and patched in patchBranches; verified against xtensa-esp32-elf-as, which encodes `call8 target` at pc 6 with target 0 as a5 ff ff.
    const uint8_t enc[3] = {0x25, 0x00, 0x00};
    emit(enc, 3);
    // The result lands in a10, which the restore below overwrites: park it in the frame first, the slot call() already uses for exactly this, then deliver it once the pool is back.
    if (take) s32i(10, kResultSlot);
    l32i(8, 4); l32i(9, 5); l32i(10, 6); l32i(11, 7);
    if (take) l32i(ar(d), kResultSlot);
}

// Move an immediate: the narrow form carries one byte, so a wider constant is built from its halves through the reserved scratch, which holds no live value.
// A single instruction serves the common small case; without it a constant above a byte truncates.
// The address form below is for a host call that reads its arguments from the frame.
// The slots already hold them and the call passes where they start, which makes the argument count a memory question rather than a register one.
// addi carries a signed byte, so a slot past byte 127 takes addmi's 256-byte step first, as a compiler builds any large frame offset.
void XtensaAssembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const int32_t off = static_cast<int32_t>(kFrameBase + uint32_t(slot) * kSlotStride);
    const uint8_t dr = ar(d);
    uint8_t base = 1;                                      // a1
    int32_t lo = off;
    if (off > 127) {
        const int32_t hi = (off + 128) >> 8;               // leaves lo in -128..127
        const uint8_t m[3] = {uint8_t((dr << 4) | 0x2), uint8_t(0xd0 | 1), uint8_t(hi)};
        emit(m, 3);                                        // addmi aD, a1, #hi*256
        lo = off - hi * 256;
        base = dr;
    }
    const uint8_t b[3] = {uint8_t((dr << 4) | 0x2), uint8_t(0xc0 | base), uint8_t(static_cast<int8_t>(lo))};
    emit(b, 3);                                            // addi aD, aBase, #lo
}

void XtensaAssembler::movImm(Reg d, int32_t imm) {
    const uint8_t dr = ar(d);
    // The wide field is signed and the only encoding here that holds a negative: @xref{constants-wider-than-a-byte-and-negative-ones|why falling through is silently wrong}. Outside every short encoding, the full value is built a byte at a time, absolute so it survives the block's copy to its final address.
    if (imm < -2048 || imm > 0xffff) {
        movPtr(d, reinterpret_cast<const void*>(static_cast<uintptr_t>(static_cast<uint32_t>(imm))));
        return;
    }
    if (imm < 0) {
        const uint32_t f = static_cast<uint32_t>(imm) & 0xfff;
        const uint8_t b[3] = {uint8_t((dr << 4) | 0x2),
                              uint8_t(0xa0 | ((f >> 8) & 0xf)),
                              uint8_t(f & 0xff)};
        emit(b, 3);                                                      // movi aD, #imm12
        return;
    }
    const uint32_t v = static_cast<uint32_t>(imm);
    if (v <= 0xff) {
        const uint8_t b[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(v)};
        emit(b, 3);
        return;
    }
    static constexpr uint8_t kTmp = 13;                                  // a13 (reserved scratch)
    const uint8_t hi[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(v >> 8)};
    emit(hi, 3);                                                         // movi aD, hi8
    const uint8_t sl[3] = {0x80, uint8_t((dr << 4) | dr), 0x11};
    emit(sl, 3);                                                         // slli aD, aD, 8
    const uint8_t lo[3] = {uint8_t((kTmp << 4) | 0x2), 0xa0, uint8_t(v & 0xff)};
    emit(lo, 3);                                                         // movi a13, lo8
    emit2(uint16_t((dr << 12) | (dr << 8) | (kTmp << 4) | 0xa));         // add.n aD, aD, a13
}
// A full address into a register, a byte at a time, through the reserved scratch so nothing live is disturbed.
// Not a literal pool, which needs a known relative distance while this block is copied to its final address after the bytes are built.
// An absolute materialization survives that move where a relative one would have to be re-based.
void XtensaAssembler::movPtr(Reg d, const void* p) {
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    const uint8_t dr = ar(d);
    static constexpr uint8_t kTmp = 13;
    const uint8_t top[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(addr >> 24)};
    emit(top, 3);                                                        // movi aD, b3
    for (int shift = 16; shift >= 0; shift -= 8) {
        const uint8_t sl[3] = {0x80, uint8_t((dr << 4) | dr), 0x11};
        emit(sl, 3);                                                     // slli aD, aD, 8
        const uint8_t by[3] = {uint8_t((kTmp << 4) | 0x2), 0xa0, uint8_t((addr >> shift) & 0xff)};
        emit(by, 3);                                                     // movi a13, bN
        emit2(uint16_t((dr << 12) | (dr << 8) | (kTmp << 4) | 0xa));     // add.n aD, aD, a13
    }
}

// add.n aD, aA, aB : word (d<<12)|(a<<8)|(b<<4)|0xa
void XtensaAssembler::addReg(Reg d, Reg a, Reg b) {
    emit2(uint16_t((ar(d) << 12) | (ar(a) << 8) | (ar(b) << 4) | 0xa));
}
// mov.n aD, aA : bytes [ (d<<4)|0xd, a ]
void XtensaAssembler::movReg(Reg d, Reg a) {
    const uint8_t b[2] = {uint8_t((ar(d) << 4) | 0xd), ar(a)};
    emit(b, 2);
}

// The convention returns in the register the first virtual one maps to, so this is a move and is free when the value is already there. Emitted before the return, since that rotates the window back and a move after it would write a register the caller does not see.
void XtensaAssembler::retValue(Reg a) {
    if (a == R0) return;                    // already in a2
    movReg(R0, a);
}
// The narrow add: @xref{the-narrow-add-encodes-one-to-fifteen-and-zero-means-minus-one|why adding nothing must emit nothing}.
void XtensaAssembler::addImm(Reg d, Reg a, int32_t imm) {
    if (imm == 0) {
        if (ar(d) != ar(a)) movReg(d, a);      // still a move: d = a + 0
        return;
    }
    if (imm >= 1 && imm <= 15) {
        emit2(uint16_t((ar(d) << 12) | (ar(a) << 8) | ((imm & 0xf) << 4) | 0xb));
        return;
    }
    // addi aD, aA, #imm8 (RRI8, signed -128..127): bytes [ (d<<4)|2, 0xc0|a, imm ].
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), uint8_t(0xc0 | ar(a)),
                          uint8_t(imm & 0xff)};
    emit(b, 3);
}
// mull aD, aA, aB : 24-bit 0x820000 | (d<<12) | (a<<8) | (b<<4)
void XtensaAssembler::mulReg(Reg d, Reg a, Reg b) {
    emit3(0x820000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) | (uint32_t(ar(b)) << 4));
}
// The signed high half of the product, which with the low half gives the fixed-point multiply its middle bits; the instruction is present on both cores. Every encoding here is emitted as a word: @xref{encodings-are-words-not-memory-bytes|why that distinction cost a board}.
void XtensaAssembler::mulhi(Reg d, Reg a, Reg b) {
    emit3(0xb20000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) | (uint32_t(ar(b)) << 4));
}
// slli aD, aA, #n : the field holds 32-n, split across bits 20-23 (high bit) and 4-7 (low nibble). n==0 is unencodable and the lowering never asks.
void XtensaAssembler::shlImm(Reg d, Reg a, uint8_t n) {
    // 1..31 only: the field holds 32-n, so n==0 and n>=32 have no encoding and would emit a shift by some other amount. Refuse, the way shrImm below does.
    if (n == 0 || n >= 32) { overflow_ = true; return; }
    const uint32_t k = 32u - n;
    emit3(((k >> 4) << 20) | 0x010000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) |
          ((k & 0x0fu) << 4));
}
// srai aD, aA, #n : arithmetic, sign-filling. The amount rides bits 8-11 (low nibble) and bit 20 (high bit, folded into the 0x2/0x3 opcode nibble).
void XtensaAssembler::sarImm(Reg d, Reg a, uint8_t n) {
    if (n >= 32) { overflow_ = true; return; }   // the amount field is five bits
    emit3(((0x2u | (uint32_t(n) >> 4)) << 20) | 0x010000u | (uint32_t(ar(d)) << 12) |
          ((uint32_t(n) & 0x0fu) << 8) | (uint32_t(ar(a)) << 4));
}
// The LOGICAL right shift. srli only encodes 1..15; a shift of 16 is spelled extui aD, aA, 16, 16, which extracts the top 16 bits, between them they cover every shift the front end emits.
void XtensaAssembler::shrImm(Reg d, Reg a, uint8_t n) {
    if (n >= 1 && n <= 15) {
        emit3(0x410000u | (uint32_t(ar(d)) << 12) | (uint32_t(n) << 8) | (uint32_t(ar(a)) << 4));
        return;
    }
    // extui's width field caps at 16, so 16 is the only wide shift it can express. Anything else has NO encoding here, and falling through to a shift-by-16 would emit a silently wrong constant, the failure mode movImm above was just fixed for. Fail the compile instead.
    if (n != 16) { overflow_ = true; return; }
    emit3(0xf50000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 4));   // extui aD, aA, 16, 16
}
// a12: the dedicated address scratch, OUTSIDE the R0..R9 -> a2..a11 vreg map, so computing an address into it can never clobber a live virtual register. Shared by every indexed access.
static constexpr uint8_t kAddrScratch = 12;   // a12

// The word-sized slot access in its narrow form, two bytes rather than three: @xref{the-narrow-slot-access-reaches-only-part-of-the-arena|how far it reaches, and what overflowed it}.
static constexpr int32_t kNarrowMax32 = 60;   // 4-bit field * 4-byte words

void XtensaAssembler::load32(Reg d, Reg base, int32_t imm) {
    if (imm >= 0 && imm <= kNarrowMax32 && (imm % 4) == 0) {
        emit2(uint16_t(((uint32_t(imm) / 4) << 12) | (uint32_t(ar(base)) << 8) |
                       (uint32_t(ar(d)) << 4) | 0x8));                       // l32i.n
        return;
    }
    // l32i aD, aBase, #off (RRI8, offset in 4-byte words). The byte layout is the one spillStore uses: {(t << 4) | 0x2, (op << 4) | s, imm8}, where the SECOND byte packs the opcode nibble in its high half and the base register in its low half. spillStore's literal 0x61 is exactly that: opcode 6 (s32i) over base a1. Writing a bare register there drops the opcode.
    if (imm < 0 || imm / 4 > 255 || (imm % 4) != 0) { overflow_ = true; return; }
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), uint8_t((0x2 << 4) | ar(base)),
                          uint8_t(imm / 4)};
    emit(b, 3);
}
void XtensaAssembler::store32(Reg base, int32_t imm, Reg val) {
    if (imm >= 0 && imm <= kNarrowMax32 && (imm % 4) == 0) {
        emit2(uint16_t(((uint32_t(imm) / 4) << 12) | (uint32_t(ar(base)) << 8) |
                       (uint32_t(ar(val)) << 4) | 0x9));                     // s32i.n
        return;
    }
    // s32i aVal, aBase, #off (RRI8): opcode nibble 6 in the second byte's high half, where l32i uses 2. The first byte's low nibble stays 0x2 (the RRI8 instruction group).
    if (imm < 0 || imm / 4 > 255 || (imm % 4) != 0) { overflow_ = true; return; }
    const uint8_t b[3] = {uint8_t((ar(val) << 4) | 0x2), uint8_t((0x6 << 4) | ar(base)),
                          uint8_t(imm / 4)};
    emit(b, 3);
}
// The indexed forms compute the address into a12 first, the same dedicated scratch the byte path uses: it sits outside the R0..R9 vreg map, so it never clobbers a live vreg.
void XtensaAssembler::load32Idx(Reg d, Reg base, Reg off) {
    emit2(uint16_t((kAddrScratch << 12) | (uint32_t(ar(base)) << 8) |
                   (uint32_t(ar(off)) << 4) | 0xa));                      // add.n a12, base, off
    emit2(uint16_t((uint32_t(kAddrScratch) << 8) | (uint32_t(ar(d)) << 4) | 0x8));
}
void XtensaAssembler::store32Idx(Reg base, Reg off, Reg val) {
    emit2(uint16_t((kAddrScratch << 12) | (uint32_t(ar(base)) << 8) |
                   (uint32_t(ar(off)) << 4) | 0xa));                      // add.n a12, base, off
    emit2(uint16_t((uint32_t(kAddrScratch) << 8) | (uint32_t(ar(val)) << 4) | 0x9));
}
// Xtensa s8i only offsets a base by an immediate (no register-offset store), so compute the address into a dedicated scratch a12, OUTSIDE the R0..R9 → a2..a11 vreg map, so it never clobbers a live virtual register, then s8i aVal, a12, 0. add.n a12, aBase, aOff : (12<<12)|(base<<8)|(off<<4)|0xa  ;  s8i aVal, a12, 0 : [(val<<4)|2, 0x40|12, 0]
void XtensaAssembler::store8(Reg base, Reg off, Reg val) {
    emit2(uint16_t((kAddrScratch << 12) | (ar(base) << 8) | (ar(off) << 4) | 0xa));   // add.n a12, base, off
    const uint8_t b[3] = {uint8_t((ar(val) << 4) | 0x2), uint8_t(0x40 | kAddrScratch), 0x00};
    emit(b, 3);                                             // s8i aVal, a12, 0
}
// l8ui aDst, aBase, #imm (0..255) : bytes [ (dst<<4)|2, base, imm ], zero-extended byte load.
void XtensaAssembler::load8(Reg d, Reg base, int32_t imm) {
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), ar(base), uint8_t(imm & 0xff)};
    emit(b, 3);
}


// Xtensa has no register-offset load either. The computed address goes through kAddrScratch, the same temp store8 uses, and the RRI8 offset is 0 so the offset scaling never applies.
void XtensaAssembler::load8Idx(Reg d, Reg base, Reg off) {
    emit2(uint16_t((kAddrScratch << 12) | (ar(base) << 8) | (ar(off) << 4) | 0xa));   // add.n a12, base, off
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), kAddrScratch, 0x00};           // l8ui d, a12, 0
    emit(b, 3);
}

// branchIfZero(a, l): synthesised as `movi a13,0; bgeu a13, a, l`. Unsigned 0 >= a is true IFF a == 0, so this branches exactly when a is zero, using only the verified bgeu 8-bit branch (no separate beqz form / offset width). a13 is a scratch outside the vreg map.
void XtensaAssembler::branchIfZero(Reg a, Label l) {
    static constexpr uint8_t kZero = 13;   // a13
    const uint8_t mv[3] = {uint8_t((kZero << 4) | 0x2), 0xa0, 0x00};   // movi a13, 0
    emit(mv, 3);
    // Same relaxed form as the other branches: bltu a13, a, +3 (the inverse of bgeu) over a `j`.
    const uint8_t br[3] = {uint8_t((ar(a) << 4) | 0x7), uint8_t((0x3 << 4) | kZero), 0x02};
    emit(br, 3);
    addFixup(len_, l);
    const uint8_t j[3] = {0x06, 0x00, 0x00};
    emit(j, 3);
}
// A conditional branch, emitted as the inverted condition over an unconditional jump that reaches anywhere in a script: @xref{conditional-branches-are-relaxed-always|why always, rather than per branch}. The jump keeps its own fixup, and the inverted branch's own displacement is already correct as emitted.
void XtensaAssembler::branchRelaxed(uint8_t condNibble, Reg a, Reg b, Label l) {
    // The inverted condition, skipping the jump that follows; displacements here are measured from a fixed offset past the instruction, so clearing it is the value used.
    // Every condition this is called with is listed, since an unlisted one would take the final branch and emit a WRONG condition rather than failing.
    // That is a program which runs and does the opposite thing.
    const uint8_t inv = condNibble == 0x9 ? 0x1 : condNibble == 0x1 ? 0x9
                      : condNibble == 0xb ? 0x3 : condNibble == 0x3 ? 0xb
                      : condNibble == 0xa ? 0x2 : condNibble == 0x2 ? 0xa
                      : 0xb;
    const uint8_t br[3] = {uint8_t((ar(b) << 4) | 0x7), uint8_t((inv << 4) | ar(a)), 0x02};
    emit(br, 3);
    addFixup(len_, l);
    const uint8_t j[3] = {0x06, 0x00, 0x00};      // j — the 18-bit offset is patched in
    emit(j, 3);
}
// bgeu aA, aB, l  (skip if a >= b, unsigned)
void XtensaAssembler::branchGeU(Reg a, Reg b, Label l) { branchRelaxed(0xb, a, b, l); }
// bge aA, aB, l  (skip if a >= b, SIGNED). Same relaxation, one nibble apart from bgeu.
void XtensaAssembler::branchGeS(Reg a, Reg b, Label l) { branchRelaxed(0xa, a, b, l); }
// bne aA, aB, l
void XtensaAssembler::branchNe(Reg a, Reg b, Label l) { branchRelaxed(0x9, a, b, l); }

// A windowed call to a host built-in. The rotation preserves the lower registers for free, and only the upper few rotate out, which may hold values live across the call.
// So those are saved to the frame around it, mirroring the host backend's full save, on a path taken once per call.
// The target address is built a byte at a time rather than through a literal pool.
void XtensaAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // Save the rotate-out scratch a8, a9, a11 (a10 will carry arg→result).
    auto s32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x61,off4}; emit(enc,3); };
    auto l32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x21,off4}; emit(enc,3); };
    s32i(8, 4); s32i(9, 5); s32i(11, 7);                  // [a1+16]=a8, [a1+20]=a9, [a1+28]=a11
    // a14/a15 are deliberately NOT saved here, because they are no longer vregs (kXtReg): they carry this routine's own return linkage for retw.n, so writing saved copies back into them after the call is what broke every scripted layout. See the Reg enum for the failure that produced.
    s32i(10, 6);                                          // [a1+24]=a10 — a vreg (R8) call8 rotates out

    // The arguments into the rotating registers the callee will read as its own, moved highest first so an earlier write cannot clobber a source a later one still needs. The first goes through the scratch, since one of those registers is itself a virtual one and could BE that argument, which ordering alone does not cover.
    emit2(uint16_t((uint32_t(ar(a)) << 8) | (13 << 4) | 0xd));   // mov a13, argA  (a13 is scratch)
    emit2(uint16_t((uint32_t(ar(c)) << 8) | (12 << 4) | 0xd));   // mov a12, argC
    emit2(uint16_t((uint32_t(ar(b)) << 8) | (11 << 4) | 0xd));   // mov a11, argB
    emit2(uint16_t((13u << 8) | (10 << 4) | 0xd));               // mov a10, a13

    // The address is assembled a byte at a time, which needs a temporary, and the choice is load-bearing: @xref{which-register-builds-the-call-target-is-load-bearing|what the wrong one hands the callee}.
    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    auto moviA8  = [&](uint8_t v){ const uint8_t enc[3]={0x82,0xa0,v}; emit(enc,3); };
    auto moviA13 = [&](uint8_t v){ const uint8_t enc[3]={0xd2,0xa0,v}; emit(enc,3); };
    auto slliA8  = [&]{ const uint8_t enc[3]={0x80,0x88,0x11}; emit(enc,3); };
    auto addA8A13 = [&]{ emit2(0x88dau); };   // add.n a8, a8, a13 = (8<<12)|(8<<8)|(13<<4)|0xa
    moviA8(uint8_t(addr >> 24));
    slliA8(); moviA13(uint8_t(addr >> 16)); addA8A13();
    slliA8(); moviA13(uint8_t(addr >> 8));  addA8A13();
    slliA8(); moviA13(uint8_t(addr));       addA8A13();
    emit3(kCallxOpcode | (8u << 8));                       // callx8 a8  → result in a10
    // Park the result in the FRAME rather than a register: @xref{the-result-is-parked-in-the-frame-not-a-register|which register the rotation overwrote}.
    s32i(10, kResultSlot);                                 // [a1+kResultSlot*4] = result
    l32i(8, 4); l32i(9, 5); l32i(10, 6); l32i(11, 7);
    l32i(ar(d), kResultSlot);                              // dst = the parked result
}

void XtensaAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch, stated rather than left to be derived from a zero count. An overflowed compile is refused afterwards, so patching it is pointless and unsafe: a fixup recorded just before a dropped instruction points past the buffer's end.
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] == kUnbound) continue;                                  // unbound label: leave as-is (overflow_ already failed the compile)
        uint32_t enc = 0;
        if (f.kind == FixKind::Jump) {
            // `j`: the displacement is relative to the byte AFTER the instruction and occupies bits 6..23: eighteen signed bits, so it reaches any script the code buffer can hold. Range-checked: refusing beats silently retargeting a jump.
            const int32_t off = static_cast<int32_t>(labelPos_[f.label]) - (static_cast<int32_t>(f.at) + 4);
            if (off < -131072 || off > 131071) { overflow_ = true; return; }
            enc = 0x06u | ((static_cast<uint32_t>(off) & 0x3ffffu) << 6);
        } else {
            // A rotating call's offset counts whole words from its own address rounded down, which is why it cannot share the jump's arithmetic, and is what buys it a wider reach.
            // The target is always aligned because the prologue is padded, so the check below is an assertion rather than a live path.
            // It stays because the alternative to noticing is a call landing mid-instruction, which the assembler rejects outright.
            // The indirect form has no such rule, which is why a call INTO a block always worked while one inside it did not.
            const int32_t base = static_cast<int32_t>(f.at & ~size_t(3));
            const int32_t byteOff = static_cast<int32_t>(labelPos_[f.label]) - (base + 4);
            if ((byteOff & 3) != 0) { overflow_ = true; return; }
            const int32_t off = byteOff >> 2;
            if (off < -131072 || off > 131071) { overflow_ = true; return; }
            enc = 0x25u | ((static_cast<uint32_t>(off) & 0x3ffffu) << 6);   // 0x25 = op0 5, n 2
        }
        buf_[f.at + 0] = static_cast<uint8_t>(enc);
        buf_[f.at + 1] = static_cast<uint8_t>(enc >> 8);
        buf_[f.at + 2] = static_cast<uint8_t>(enc >> 16);
    }
}


// The two-line binding of core's IR walk to THIS assembler. It lives here rather than in its own file because a template instantiation can only exist where its argument does: `lowerToBytes` for xtensa is not separable from XtensaAssembler.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<XtensaAssembler>(ir, out, cap, squeeze, kRegCount);
}

}  // namespace mm::moonlive

#endif  // __XTENSA__
