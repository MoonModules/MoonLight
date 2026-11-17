/// @module MoonLive

/// The arm64 (Apple Silicon host) host backend's new signed-value encodings, checked BYTE for BYTE.
///
/// Twin of unit_moonlive_codegen_x86_64.cpp for the same reason it exists: an encoding bug in JIT-emitted bytes surfaces as a fault inside anonymous executable memory, and a pinned byte sequence turns that into "this word is wrong".
///
/// Runs only on arm64 hosts, where HostAssembler compiles as the arm64 branch of the platform backend; skipped elsewhere.
///
/// Scoped to what this file adds (branchGeS, the Q16.16 primitives, 32-bit slot access): the other arm64 encodings are covered by every compile-through-run test on this host, which executes them for real.

#include "doctest.h"

#if defined(__aarch64__) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

#include "platform/host/moonlive_asm_host.h"

#include <cstdint>

using namespace mm::moonlive;

namespace {
uint32_t word(const HostAssembler& a, size_t i) {
    return uint32_t(a.bytes()[i]) | (uint32_t(a.bytes()[i + 1]) << 8)
         | (uint32_t(a.bytes()[i + 2]) << 16) | (uint32_t(a.bytes()[i + 3]) << 24);
}
}  // namespace


// b.ge (cond 0xA) against b.hs (cond 0x2): the condition nibble is what decides whether a negative compares below zero or above everything. retValue parks a script's `return` value where the ABI hands it back. Byte-checked because the register differs per ISA and a wrong one is SILENT: the host reads a plausible number, so dimensions() would answer with whatever that register happened to hold rather than crashing.
TEST_CASE("arm64: retValue moves the value into x0, the AAPCS64 return register") {
    HostAssembler a; a.retValue(R1); a.finalize();
    REQUIRE(a.size() == 4);
    // mov x0, x1 == orr x0, xzr, x1: 0xaa0003e0 | (Rm << 16). Full 64 bits, so a returned POINTER (tags() hands back a string) keeps its top half.
    CHECK(word(a, 0) == 0xaa0103e0u);

    // R0 already IS x0 (it is the buf argument), so returning it emits the no-op move rather than a different instruction: correct either way, and free.
    HostAssembler z; z.retValue(R0); z.finalize();
    REQUIRE(z.size() == 4);
    CHECK(word(z, 0) == 0xaa0003e0u);
}

TEST_CASE("arm64: branchGeS branches on GE where branchGeU branches on HS") {
    HostAssembler u; { auto l = u.newLabel(); u.branchGeU(R0, R1, l); u.bind(l); u.finalize(); }
    HostAssembler s; { auto l = s.newLabel(); s.branchGeS(R0, R1, l); s.bind(l); s.finalize(); }
    REQUIRE(u.size() == 8);                             // cmp + b.cond
    REQUIRE(s.size() == 8);
    CHECK((word(u, 0) & 0xFFE0001Fu) == 0x6B00001Fu);   // cmp wA, wB (subs wzr): 32-bit, matching
                                                        // the x86-64 compare width
    CHECK((word(u, 4) & 0xFF00000Fu) == 0x54000002u);   // b.hs
    CHECK((word(s, 4) & 0xFF00000Fu) == 0x5400000Au);   // b.ge, SIGNED
}



// The Q16.16 primitives. smull+lsr is the arm64 spelling of "the signed high 32 bits": a 32-bit vreg pair widened to 64, then the top word taken. Checked against clang's own encodings.
TEST_CASE("arm64: mulhi widens to 64 bits before taking the high word") {
    HostAssembler a; a.mulhi(R0, R1, R2); a.finalize();
    REQUIRE(a.size() == 8);
    CHECK(word(a, 0) == 0x9b227c20u);   // smull x0, w1, w2
    CHECK(word(a, 4) == 0xd360fc00u);   // lsr   x0, x0, #32
}

// asr fills from the sign bit and lsl does not: the pair is what int <-> fixed conversion is, and using the logical shift for the down-conversion would turn every negative coordinate into a large positive one.
TEST_CASE("arm64: shlImm and sarImm emit lsl and the ARITHMETIC asr") {
    HostAssembler l; l.shlImm(R3, R4, 16); l.finalize();
    HostAssembler r; r.sarImm(R3, R4, 16); r.finalize();
    REQUIRE(l.size() == 4);
    REQUIRE(r.size() == 4);
    CHECK(word(l, 0) == 0x53103c83u);   // lsl w3, w4, #16
    CHECK(word(r, 0) == 0x13107c83u);   // asr w3, w4, #16
}

// The 4-byte slot access every scalar now uses. The immediate is scaled by 4, so the encoded field is offset/4, reading it as a raw byte offset would address four times too far and walk off the 64-byte arena into the system variables.
TEST_CASE("arm64: load32 and store32 scale their immediate by four") {
    HostAssembler l; l.load32(R0, R1, 16); l.finalize();
    HostAssembler s; s.store32(R1, 16, R0); s.finalize();
    REQUIRE(l.size() == 4);
    REQUIRE(s.size() == 4);
    CHECK(word(l, 0) == 0xb9401020u);            // ldr w0, [x1, #16]
    CHECK(word(s, 0) == 0xb9001020u);            // str w0, [x1, #16]
    CHECK(((word(l, 0) >> 10) & 0xfffu) == 4u);  // 16 bytes = element 4
}

// The indexed forms, which is how an int[] or fixed[] element is reached once the lowering has scaled the index.
TEST_CASE("arm64: the indexed 32-bit forms address base plus a register offset") {
    HostAssembler l; l.load32Idx(R0, R1, R2); l.finalize();
    HostAssembler s; s.store32Idx(R1, R2, R0); s.finalize();
    CHECK(word(l, 0) == 0xb8626820u);            // ldr w0, [x1, x2]
    CHECK(word(s, 0) == 0xb8226820u);            // str w0, [x1, x2]
}

#else
TEST_CASE("arm64 codegen: skipped (not an arm64 host)") { CHECK(true); }
#endif
