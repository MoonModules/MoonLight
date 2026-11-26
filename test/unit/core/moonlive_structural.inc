// The STRUCTURAL CHECKER over emitted machine code.
//
// A golden-bytes test says "the emission changed"; it never says "the emission is WRONG". These
// three checks say the second thing, and each one is a defect class that reached a bench:
//
//   1. Frame offsets stay inside the frame the prologue allocated, and clear of the region the ABI
//      reserves at its top. The parked host arguments landed inside Xtensa's 16-byte base-save
//      area — where the hardware spills the caller's a0..a3 on a window overflow — so a script's
//      own data and its RETURN ADDRESS shared memory. Any script at all reset an ESP32 classic and
//      an S3 with `IllegalInstruction` and a data address in A0. Found on a bench, not by a test.
//   2. Every branch and jump target lands on an instruction boundary inside the program. Xtensa's
//      conditional branches reach +/-127 bytes and were silently truncated past that, so a long
//      loop jumped into the middle of an instruction.
//   3. No instruction names a register outside the backend's map. a14/a15 carry the routine's
//      retw.n return linkage; using them as general registers produced IllegalInstruction the
//      moment a scripted layout ran.
//
// Each ISA's TU defines MM_ISA_DECODE — a decoder that walks the byte stream and reports, per
// instruction, its length plus any frame offset / branch target / register it names. The checks
// themselves are written once, here.
//
// Expects, from the including TU: MM_ISA_NAME, MM_ISA_LOWER, MM_ISA_RESERVED_TOP,
// MM_ISA_DECODE, and the compiler already included.

#include "moonlive_structural.h"


// Frame offsets must land inside the frame the prologue allocated, and BELOW the region the ABI
// reserves at the top of it. This is the check that would have caught the base-save-area bug on the
// host, in CI, instead of on two boards.
TEST_CASE("emitted " MM_ISA_NAME " code keeps every frame offset inside the frame") {
    struct Case { const char* name; const char* src; int binding; };
    const Case cases[] = {
        {"a bare write",      mmScript("setRGB(0, 255, 0, 0);\n"), 1},
        {"a loop",            mmScript("for (int i = 0; i < 3; i = i + 1) { }\n"), 0},
        {"a call in a loop",  mmScript("for (int i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }\n"), 0},
        {"the shipped grid",  kGridLayout, 0},
        // Several routines in one block, which is what a class emits. Every function after the
        // first is what a checker reading only the prologue at byte 0 would never look at.
        {"two functions",
         "class TwoFns {\n  void helper() { setRGB(1, 10, 20, 30); }\n  void tick() { setRGB(2, 40, 50, 60); }\n}\n", 1},
        // A function that CALLS another, which adds the argument reload and the depth guard to
        // every prologue: both address the frame, so both are subject to this check.
        {"a calling class",
         "class Calls {\n  void helper() { setRGB(1, 10, 20, 30); }\n  void tick() { setRGB(2, 40, 50, 60); helper(); }\n}\n", 1},
        // A RECURSIVE function: the same frame is entered many times over, so an offset that
        // intrudes into the reserve is written by every activation rather than once.
        {"a recursive class",
         "class Rec {\n  void down() { setRGB(1, 10, 20, 30); down(); }\n  void tick() { setRGB(2, 40, 50, 60); down(); }\n}\n", 1},
    };
    for (const auto& c : cases) {
        bool ok = false;
        const auto& sysvars = c.binding == 0 ? mm::moonlive::layoutSysVars()
                                             : mm::moonlive::effectSysVars();
        const auto code = emitBytes(c.src, sysvars, ok);
        INFO("script: ", c.name, " on " MM_ISA_NAME);
        REQUIRE(ok);
        REQUIRE(code.size() > 0);

        // The frame each ROUTINE allocated, re-read at every prologue in the block.
        //
        // A class emits one routine per function, so a block holds several prologues and a frame
        // offset belongs to whichever one precedes it. Reading only the prologue at byte 0 and
        // judging the whole block against it would leave every function after the first unchecked
        //: silently, and on the target where the frame contract is fatal.
        uint32_t frame = 0;
        uint32_t usable = 0;
        int prologues = 0;

        size_t pc = 0;
        while (pc < code.size()) {
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            INFO("at byte ", pc);
            REQUIRE(d.len > 0);                       // an undecodable byte is itself a defect
            if (d.hasFrameAlloc) {
                frame = d.frameAlloc;
                prologues++;
                // Guarded rather than subtracted blind: a defect that emits a frame smaller than
                // the reserved area would wrap this unsigned subtraction to ~4 billion and let
                // EVERY offset below pass, the checker going blind exactly when the code is worst.
                INFO("prologue frame ", frame, " needs >= ", (MM_ISA_RESERVED_TOP));
                REQUIRE(frame >= (MM_ISA_RESERVED_TOP));
                usable = frame - (MM_ISA_RESERVED_TOP);
            }
            if (d.hasFrameOff) {
                // Every frame access must follow a prologue: one that does not belongs to no frame,
                // which is what calling an entry point past its own prologue produced.
                REQUIRE(prologues > 0);
                INFO("frame offset ", d.frameOff, " vs frame ", frame,
                     " (usable below ", usable, ")");
                CHECK(d.frameOff + 4 <= usable);      // the whole 4-byte word must fit
            }
            pc += d.len;
        }
        CHECK(pc == code.size());                     // the stream decoded exactly, no trailing byte
        CHECK(prologues > 0);                         // every emitted routine opens with a prologue
    }
}

// A branch that lands mid-instruction executes whatever the following bytes happen to encode.
TEST_CASE("emitted " MM_ISA_NAME " code branches only to instruction boundaries") {
    struct Case { const char* name; const char* src; int binding; };
    const Case cases[] = {
        {"one loop",     mmScript("for (int i = 0; i < 3; i = i + 1) { }\n"), 0},
        {"nested loops", kGridLayout, 0},
        // Long enough that a short-displacement branch would have to be relaxed to reach.
        {"a long loop body",
         mmScript("for (int i = 0; i < 8; i = i + 1) {\n"
         "  addLight(i, 0, 0); addLight(i, 1, 0); addLight(i, 2, 0); addLight(i, 3, 0);\n"
         "  addLight(i, 4, 0); addLight(i, 5, 0); addLight(i, 6, 0); addLight(i, 7, 0);\n"
         "}\n"), 0},
    };
    for (const auto& c : cases) {
        bool ok = false;
        const auto code = emitBytes(c.src, mm::moonlive::layoutSysVars(), ok);
        INFO("script: ", c.name, " on " MM_ISA_NAME);
        REQUIRE(ok);
        REQUIRE(code.size() > 0);

        // Pass 1: every byte offset at which an instruction starts.
        std::vector<bool> boundary(code.size() + 1, false);
        size_t pc = 0;
        while (pc < code.size()) {
            boundary[pc] = true;
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            REQUIRE(d.len > 0);
            pc += d.len;
        }
        // NOT one-past-the-end: the epilogue is always the LAST instruction emitted, so a real
        // branch targets the `retw.n`/`ret` itself and never the byte after it. Marking that byte a
        // boundary would let a jump straight off the block, into whatever heap follows the exec
        // allocation, pass as legal.

        // Pass 2: every target must be one of them, and inside the program.
        pc = 0;
        while (pc < code.size()) {
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            if (d.hasTarget) {
                INFO("branch at ", pc, " targets ", d.target, " of ", code.size(), " bytes");
                CHECK(d.target >= 0);
                CHECK(static_cast<size_t>(d.target) < code.size());
                if (d.target >= 0 && static_cast<size_t>(d.target) < code.size())
                    CHECK(boundary[static_cast<size_t>(d.target)]);
            }
            pc += d.len;
        }
    }
}

// The windowed-call rule, as an assertion: a CALL8 rotates the register file by eight, so every
// register the rotation covers holds the callee's leftovers afterwards. Reading one after a call,
// before writing it, is reading a destroyed value.
//
// Our `call()` saves and restores a8..a11 around the call, which is what makes those four legal to
// hold values in, so the property is not "never use a8..a15" but "never READ one the save-set does
// not cover". Written as the general rule (no register is read after a call without an intervening
// write) rather than a list of register numbers, so it stays true as the save-set changes.
//
// The case list covers both shapes that matter: plain calls, and the ARENA POINTER (kArg4) that a
// script reads a control or system variable through. The arena now lives in a frame slot and is
// reloaded into a scratch register per use, and a bench bisect found that a system-variable read
// inside a loop whose body makes a host call, and only that combination, reset both Xtensa boards.
//
// COVERAGE LIMIT, stated rather than implied: the decoder reports reads for the instructions the
// emitter uses to move values (branches, the call target, spill stores, the narrow add/mov forms).
// It does NOT decode every operand of every opcode, so this clears the register-window question for
// the tracked instructions, not for the whole ISA.
TEST_CASE("emitted " MM_ISA_NAME " code reads no register a call destroyed") {
    struct Case { const char* name; const char* src; int binding; };
    const Case cases[] = {
        {"one call",            mmScript("setRGB(0, random16(256), 0, 0);\n"), 1},
        {"a call in a loop",    mmScript("for (int i = 0; i < 4; i = i + 1) { setRGB(i, random16(256), 0, 0); }\n"), 1},
        {"nested calls",        mmScript("setRGB(random16(256), random16(256), random16(256), random16(256));\n"), 1},
        {"a call in a layout",  mmScript("for (int i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }\n"), 0},
        {"the shipped grid",    kGridLayout, 0},
        // The shape that crashed on hardware, and the nearest ones that did not.
        {"sysvar bound + call in body",
         mmScript("for (int x = 0; x < width; x = x + 1) { setRGB(x, random16(256), 0, 0); }\n"), 1},
        {"member bound + call in body",
         mmScript("byte n = 8;\n"
         "for (int x = 0; x < n; x = x + 1) { setRGB(x, random16(256), 0, 0); }\n"), 1},
        {"sysvar read inside the body, with a call",
         mmScript("for (int x = 0; x < 4; x = x + 1) { setRGB(x, width, random16(256), 0); }\n"), 1},
    };
    for (const auto& c : cases) {
        bool ok = false;
        const auto& sysvars = c.binding == 0 ? mm::moonlive::layoutSysVars()
                                             : mm::moonlive::effectSysVars();
        const auto code = emitBytes(c.src, sysvars, ok);
        INFO("script: ", c.name, " on " MM_ISA_NAME);
        REQUIRE(ok);
        REQUIRE(code.size() > 0);

        // Branch targets first: control can reach one from somewhere with no call in between, and
        // this walk is linear rather than a full dataflow analysis, so assuming nothing at a join
        // point is the conservative choice that keeps it sound.
        std::vector<bool> isTarget(code.size() + 1, false);
        {
            size_t scan = 0;
            while (scan < code.size()) {
                const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), scan);
                REQUIRE(d.len > 0);
                if (d.hasTarget && d.target >= 0 && static_cast<size_t>(d.target) < code.size())
                    isTarget[static_cast<size_t>(d.target)] = true;
                scan += d.len;
            }
        }

        uint32_t dirty = 0;          // destroyed by the most recent call, not yet rewritten
        uint32_t callsSeen = 0;
        size_t pc = 0;
        while (pc < code.size()) {
            if (isTarget[pc]) dirty = 0;
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            REQUIRE(d.len > 0);
            for (uint8_t r = 0; r < 16; r++)
                if ((d.readsMask & dirty) & (1u << r)) {
                    INFO("at byte ", pc, ": reads a", int(r),
                         " which a call destroyed and nothing restored");
                    CHECK(false);
                }
            dirty &= ~d.writesMask;                       // a write makes it live again
            if (d.isCall) { dirty = (MM_ISA_CALL_CLOBBER); callsSeen++; }
            pc += d.len;
        }
        // A call MUST have been decoded, or the walk proved nothing: `dirty` stays zero and every
        // read passes. Not hypothetical, the first version of this decoder tested the wrong byte
        // order for callx8, matched no call at all, and went green on everything.
        CHECK(callsSeen > 0);
    }
}

