// The checks a DEVICE backend's emitted code must pass, written once and run per ISA.
//
// Why this exists: only the host (arm64) backend is ever EXECUTED by tests, so an Xtensa or RISC-V
// codegen defect used to ship silently and be debugged by flashing a board. That is what happened —
// a scripted layout reset every S3, and five plausible theories read off the source were each
// falsified by the device before the disassembler found the real cause. The backends are ordinary
// C++ behind a target guard, so defining the guard and compiling the sources runs the REAL emitter
// on the development machine, and a defect becomes a failing test instead of a bench session.
//
// ONE TRANSLATION UNIT PER ISA, and this file is the body they share. Two backends cannot live in
// one TU: they both define `Reg`, `Label` and an assembler class, and the include guards would give
// the second one the first one's types. Each ISA's .cpp defines MM_ISA_NAME + the target macro,
// includes its backend and the front end above it, then includes this — so the unqualified
// `lowerToBytes` inside compileSource resolves to that backend. The firmware build never sees these
// files, so a device image still holds exactly one backend definition. Same mechanism as
// moondeck/moonlive/emit_xtensa.cpp, which reads encodings without a flash.
//
// Expects, from the including TU: MM_ISA_NAME (a string literal) and the backend + compiler already
// included in the enclosing namespace.

#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "core/moonlive/MoonLiveScriptFile.h"   // the role extensions the sweep filters on

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <cstdint>
#include <vector>

// Any role extension: one language, and the sweep compiles every script whatever role its name
// claims. Delegates to the one definition (MoonLiveScriptFile.h) rather than listing the extensions
// again: two copies of this list is exactly how a new role went missing from one sweep.
inline bool mmIsScript(const std::filesystem::path& p) {
    return mm::moonlive::isScriptExt(p.extension().string().c_str());
}


namespace {

// The shipped default layout: a nested loop whose body calls addLight. The script that crashed
// every S3, so the one worth pinning hardest.
const char* kGridLayout =
    "class GridLayout {\n"
    "  byte cols = 16;\n"
    "  byte rows = 16;\n"
    "  void tick() {\n"
    "    for (int y = 0; y < rows; y = y + 1) {\n"
    "      for (int x = 0; x < cols; x = x + 1) {\n"
    "        addLight(x, y, 0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

// The two shapes an effect takes: straight-line, and a loop over the grid. Spelled out in full
// rather than wrapped by a helper: these pin the EXACT bytes a shipped script emits, so the source
// they are compiled from has to be the source a user would write.
const char* kEffectSimple =
    "class SimpleEffect {\n  void tick() { setRGB(0, 255, 0, 0); }\n}\n";
const char* kEffectLoop =
    "class LoopEffect {\n"
    "  void tick() { for (int i = 0; i < 64; i = i + 1) { setRGB(i, i, 255 - i, 60); } }\n"
    "}\n";

/// Compile for THIS TU's backend. `sysvars` is the ONE light vocabulary; the role-named
/// accessors are aliases of it, so which one a caller passes is documentation rather than a
/// behavioural choice (pinned by "the three roles are handed the same table").
std::vector<uint8_t> emitBytes(const char* src, const mm::moonlive::SysVarTable& sysvars, bool& ok) {
    // Sized the way production sizes it, from the script's own token count — a test that always
    // allocated the sanity bound would pass while a right-sized caller overflowed.
    std::vector<uint8_t> out(mm::moonlive::codeCapFor(mm::moonlive::countTokens(src)));
    // A string pool, as the engine supplies one: a script with a literal (every `addControl` name)
    // interns into it, and the emitted code carries pointers there. Static so those pointers stay
    // valid for as long as a test might look at the bytes.
    static char strings[mm::moonlive::CompileResult::kMaxStrings];
    auto r = mm::moonlive::compileSource(src, mm::moonlive::lightBuiltins(), sysvars,
                                         out.data(), out.size(), nullptr, MM_ISA_LOWER,
                                         strings, sizeof(strings));
    ok = r.ok;
    out.resize(r.ok ? r.len : 0);
    return out;
}

size_t emitLen(const char* src, const mm::moonlive::SysVarTable& sysvars, bool& ok) {
    return emitBytes(src, sysvars, ok).size();
}

/// FNV-1a over the emitted bytes — the project's idiom for "did this change".
///
/// Only meaningful for a script with NO host call. A `Call` bakes the builtin's ADDRESS into the
/// instruction stream and ASLR moves that every process, so the bytes of a calling script genuinely
/// differ run to run and no golden value can pin them. Those scripts are covered by the structural
/// checks and by their emitted LENGTH instead, which is stable.
uint32_t emitHash(const char* src, const mm::moonlive::SysVarTable& sysvars, bool& ok) {
    const auto code = emitBytes(src, sysvars, ok);
    uint32_t h = 2166136261u;
    for (uint8_t byte : code) { h ^= byte; h *= 16777619u; }
    return h;
}

}  // namespace

// The shipped default layout has to COMPILE for every target. It stopped compiling on Xtensa when
// the register file shrank to the ten registers the windowed ABI actually leaves free, and a script
// that is refused reads to a user exactly like one that is broken.
TEST_CASE("the shipped grid layout compiles for " MM_ISA_NAME) {
    bool ok = false;
    CHECK(emitLen(kGridLayout, mm::moonlive::layoutSysVars(), ok) > 0);
    CHECK(ok);
}

TEST_CASE("a scripted effect compiles for " MM_ISA_NAME ", straight-line and looped") {
    bool ok = false;
    CHECK(emitLen(kEffectSimple, mm::moonlive::effectSysVars(), ok) > 0);
    CHECK(ok);
    // A LOOPED effect is the case that did not fit Xtensa's ten registers until call arguments were
    // staged through the frame: a four-operand setRGB used to need four registers live at once, plus
    // whatever its operands took to compute, and the allocator ran out. Now only one argument holds
    // a register at a time.
    CHECK(emitLen(kEffectLoop, mm::moonlive::effectSysVars(), ok) > 0);
    CHECK(ok);
}

// `fill` plus a loop was the shape that did NOT fit the smallest register file: FillElems needs two
// scratch registers above the program's, and on Xtensa that left eight where the five fixed ABI
// vregs plus four reload temps needed nine — one short, so the golden here was 0 (REFUSED). Moving
// the host arguments into frame slots freed those five, and it now emits on every backend.
// Still pinned by length: it is the shape most sensitive to the register budget, so a regression
// that pushes any backend back over the edge shows up here first.
TEST_CASE("fill plus a loop on " MM_ISA_NAME " emits what it currently can") {
    bool ok = false;
    CHECK(emitLen(mmScript("fill(0,0,0);\nfor (int y = 0; y < height; y = y + 1) { setRGB(y, 255, 0, 0); }"),
                  mm::moonlive::effectSysVars(), ok) == MM_GOLD_FILLLOOP_LEN);
}

// A scripted MODIFIER is the third binding, and the one handed a per-light coordinate.
TEST_CASE("a scripted modifier compiles for " MM_ISA_NAME) {
    bool ok = false;
    CHECK(emitLen(mmScript("setXYZ(xPos, yPos, zPos);\n"), mm::moonlive::modifierSysVars(), ok) > 0);
    CHECK(ok);
}

// The GOLDEN check: the exact machine code for the shipped scripts.
//
// A change DETECTOR, not a correctness claim. It fails on any emission change so the diff gets read
// deliberately, instead of the change being discovered by flashing a board and watching it reset.
// When a change is intended, update the constants and say why in the commit.
//
// Hashed rather than length-checked: altering a frame offset or a register field changes what
// EXECUTES without changing how many bytes it takes — which is precisely the defect class this file
// exists to catch, and a length alone sails straight past it.
TEST_CASE("the " MM_ISA_NAME " machine code for the shipped scripts is what it was") {
    bool ok = false;
    // The grid CALLS addLight, so its bytes carry a host address that moves each process — length
    // only. The effect is pure inline code, so it is pinned byte for byte.
    CHECK(emitLen(kGridLayout, mm::moonlive::layoutSysVars(), ok) == MM_GOLD_GRID_LEN);
    CHECK(emitLen(kEffectSimple, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FX_LEN);
    CHECK(emitHash(kEffectSimple, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FX_HASH);
    CHECK(emitLen(kEffectLoop, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FXLOOP_LEN);
    CHECK(emitHash(kEffectLoop, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FXLOOP_HASH);

    // KNOWN GAP: no golden pins a CALL SEQUENCE. The goldens above are inline-only code, so the
    // call path (the code the windowed ABI makes subtle) is covered by the structural checks and
    // the frame-offset test, not by a byte hash. A raw byte-mask golden was tried and removed: a
    // masked scan cannot tell a movi immediate from a coincidental byte, so its hash flapped
    // between builds. A real one needs the decoder, not a mask.
}

// Degrade, never miscompile: a script the backend cannot encode must fail cleanly. The emitters
// signal that by returning zero bytes, which compileSource turns into a diagnostic: the device
// then shows the error and keeps rendering, instead of executing whatever the truncation produced.
TEST_CASE("an unencodable script is refused, not truncated, on " MM_ISA_NAME) {
    bool ok = true;
    // A loop nest far past what any label/fixup table holds.
    std::string deep;
    for (int i = 0; i < 60; i++) deep += "for (i" + std::to_string(i) + " = 0; i" +
                                        std::to_string(i) + " < 2; i" + std::to_string(i) +
                                        " = i" + std::to_string(i) + " + 1) {";
    deep += "addLight(0, 0, 0);";
    for (int i = 0; i < 60; i++) deep += "}";
    const size_t n = emitLen(deep.c_str(), mm::moonlive::layoutSysVars(), ok);
    CHECK(n == 0);      // refused
    CHECK_FALSE(ok);    // and said so
}

// The language additions expand into the scalar operations every backend already emits, so each compiles here with nothing new in the backend.
TEST_CASE("structs, joined conditions and the arithmetic builtins compile for " MM_ISA_NAME) {
    const char* src =
        "class T {\n"
        "  RGB keep;\n"
        "  Coord3D at;\n"
        "  RGB warm(int k) { return RGB(k, k / 2, 10); }\n"
        "  Coord3D move(Coord3D p, Coord3D d) { return Coord3D(p.x + d.x, p.y + d.y, p.z + d.z); }\n"
        "  void tick() {\n"
        "    int k = 5;\n"
        "    RGB c = paletteRGB(k, 255);\n"
        "    at = move(at, Coord3D(1, -1, 0));\n"
        "    for (int i = 0; i < 8; i = i + 1) {\n"
        "      if (i > 2 && !(i == 5) || abs(at.x) > 100) { setRGB(i, c); } else { setRGB(i, warm(clamp(k, 0, 255))); }\n"
        "      k = min(k + 1, max(k, 9));\n"
        "    }\n"
        "    keep = getRGB(0);\n"
        "    setRGB(8, keep.r, getRGB(1).g, k);\n"
        "    byte b = k + 1;\n"                       // a byte narrows by a shift of 24, which Xtensa once could not encode
        "    setRGB(9, b, 0, 0);\n"
        "  }\n"
        "}\n";
    std::vector<uint8_t> out(mm::moonlive::codeCapFor(mm::moonlive::countTokens(src)));
    static char strings[mm::moonlive::CompileResult::kMaxStrings];
    const auto r = mm::moonlive::compileSource(src, mm::moonlive::lightBuiltins(), mm::moonlive::effectSysVars(),
                                               out.data(), out.size(), nullptr, MM_ISA_LOWER, strings, sizeof(strings));
    INFO(std::string(r.error));
    CHECK(r.ok);
    CHECK(r.len > 0);
}

// lowerWith builds the assembler in a heap block for the length of one compile, so its tables cost heap on the board that compiles, never a task's stack.
// A label is a two-byte code position and a fixup four bytes (position, label, kind), the same on the host as on a device, so sizeof() measures what a board allocates.
TEST_CASE("the " MM_ISA_NAME " assembler's tables fit a small heap block") {
    const size_t tableBytes = mm::moonlive::kAsmLabels * sizeof(mm::moonlive::CodePos) + mm::moonlive::kAsmFixups * 4;
    INFO(MM_ISA_NAME " assembler = " << sizeof(MM_ISA_ASM) << " bytes, tables " << tableBytes);
    CHECK(tableBytes <= 1536);
    CHECK(sizeof(MM_ISA_ASM) <= tableBytes + 128);   // the tables are the assembler: a wider fixup shows here first
}

// A class whose functions call each other has to place each function where a call can REACH it.
//
// Xtensa entry points must sit on a 4-byte boundary: the toolchain rejects an unaligned `entry`
// outright ("unaligned entry instruction"), and CALLn encodes its target as a count of 4-byte units,
// so an unaligned callee is not expressible either. Xtensa instructions are 2 or 3 bytes, so a
// function following another lands wherever the previous one ended and the alignment is not free.
// crosshair.mle is the script that found this: three functions, and the compile was refused with the
// generic "too large" while the same script built fine on RISC-V, whose instructions are all 4 bytes.
//
// Checked on every backend rather than only Xtensa, because the requirement is a property of the
// emitted block, and a backend that gains a compressed encoding would need the same pad.
TEST_CASE("every function in a class starts where a call can reach it, on " MM_ISA_NAME) {
    const char* src =
        "class T {\n"
        "  void first()  { setRGB(1, 7, 8, 9); }\n"
        "  void second() { setRGB(2, 1, 2, 3); first(); }\n"
        "  void tick()   { setRGB(0, 4, 5, 6); first(); second(); }\n"
        "}\n";
    std::vector<uint8_t> out(mm::moonlive::codeCapFor(mm::moonlive::countTokens(src)));
    auto r = mm::moonlive::compileSource(src, mm::moonlive::lightBuiltins(),
                                         mm::moonlive::effectSysVars(), out.data(), out.size(),
                                         nullptr, MM_ISA_LOWER);
    INFO("compile error: ", r.error);
    REQUIRE(r.ok);                     // the refusal this test exists for
    REQUIRE(r.entryCount == 3);
    for (uint8_t f = 0; f < r.entryCount; f++) {
        INFO("function ", f, " at offset ", r.entries[f].offset);
        CHECK((r.entries[f].offset & 3u) == 0u);
        CHECK(r.entries[f].offset < r.len);       // inside the block it was emitted into
    }
    // Distinct entry points: two functions sharing an offset means one never got its own prologue,
    // which is how the boundary bug presented: the block looked structurally plausible and the
    // second function's frame was simply never established.
    CHECK(r.entries[0].offset != r.entries[1].offset);
    CHECK(r.entries[1].offset != r.entries[2].offset);
}

// A `return <value>` must park its value in THIS ISA's return register. The register differs per
// ISA and a wrong one is SILENT: the host reads a plausible number, so `dimensions()` would answer
// with whatever that register held rather than failing. Nothing else executes these backends, so
// the encoding is checked by finding the instruction in the emitted bytes.
//
// The predicate matches the DESTINATION only. Which register holds the value is the allocator's
// choice and may change; that it is moved into the ABI's return register is the contract.
// The CALLER side of the same contract, and the half nothing else reaches. Taking a value means
// stashing the callee's result before the vreg pool is restored over it: the restore reloads the
// very register the value arrived in. Host backends prove the SEMANTICS (unit_moonlive_spill and
// unit_moonlive_compiler run the code), but these two ISAs are never executed here, so a wrong
// stash is silent, and a value-returning call is exactly where an encoding slip reads as a
// plausible number rather than a crash.
TEST_CASE("taking a script function's value stashes it past the pool restore on " MM_ISA_NAME) {
    bool ok = false;
    // The value must be USED after a second call, so the pool restore genuinely runs over it.
    const auto code = emitBytes("class T {\n"
                                "  int a() { return 40; }\n"
                                "  int b() { return 5; }\n"
                                "  void tick() { setRGB(0, a() + b(), 0, 0); }\n"
                                "}\n",
                                mm::moonlive::effectSysVars(), ok);
    REQUIRE(ok);
    REQUIRE(code.size() > 4);
    bool found = false;
    for (size_t i = 0; i + 4 <= code.size(); i += MM_ISA_RET_STRIDE)
        if (MM_ISA_STASHES_RESULT(&code[i])) { found = true; break; }
    CHECK(found);
}

TEST_CASE("a returned value reaches the return register on " MM_ISA_NAME) {
    bool ok = false;
    // A function that returns a COMPUTED value: a literal could be materialized straight into the
    // return register by a smarter path and would not prove the move happens.
    const auto code = emitBytes("class T {\n"
                                "  int answer() { return 6 * 7; }\n"
                                "  void tick() { setRGB(0, 1, 2, 3); }\n"
                                "}\n",
                                mm::moonlive::effectSysVars(), ok);
    REQUIRE(ok);
    REQUIRE(code.size() > 4);
    bool found = false;
    for (size_t i = 0; i + 4 <= code.size(); i += MM_ISA_RET_STRIDE)
        if (MM_ISA_RET_WRITES_RETREG(&code[i])) { found = true; break; }
    CHECK(found);
}

// Every script we SHIP has to compile on every backend, or a user's board choice silently decides
// which effects exist. `plasma.mle` is why this test is here: it ran on an S3 and on desktop and was
// refused on an S31, because one `kCodeCap` constant was sized to the densest ISA while RISC-V emits
// up to 1.9x more for identical source. The buffer is now sized per script, and this pins that every
// shipped script still fits on every backend.
//
// Read from moonlive/ rather than copied inline: a copy is a second source of truth that drifts the
// first time a script is edited, and it is exactly the widest scripts — the ones worth checking —
// that would silently stop being the ones under test.
TEST_CASE("every shipped script compiles for " MM_ISA_NAME) {
    int checked = 0;
    {
        const std::filesystem::path dir =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "moonlive";
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file() || !mmIsScript(entry.path())) continue;
            // The light roles, by extension, wherever they sit: a service or a palette compiles against another table.
            const std::string ext = entry.path().extension().string();
            if (ext != mm::moonlive::kEffectExt && ext != mm::moonlive::kLayoutExt && ext != mm::moonlive::kModifierExt) continue;
            std::ifstream f(entry.path());
            std::ostringstream ss;
            ss << f.rdbuf();
            const std::string src = ss.str();
            // Per role, by extension, so this reads as "compile it the way its binding does" if the
            // roles' tables ever diverge again; all three are the same table.
            const auto& sysvars = ext == mm::moonlive::kLayoutExt   ? mm::moonlive::layoutSysVars()
                                : ext == mm::moonlive::kModifierExt ? mm::moonlive::modifierSysVars()
                                                                    : mm::moonlive::effectSysVars();
            bool ok = false;
            const size_t n = emitLen(src.c_str(), sysvars, ok);
            INFO("script: ", entry.path().filename().string(), " on " MM_ISA_NAME);
            CHECK(ok);
            CHECK(n > 0);
            // And it fits its own SIZING ESTIMATE with room to spare. codeCapFor allocates from a
            // token count before a byte is emitted, so the constant is only right while the widest
            // real script stays well inside it. This is what fails if a change makes codegen denser
            // (a wider call sequence, a new per-statement guard) or if the constant is trimmed too
            // far: 1.5x leaves margin for the next script, which the shipped set cannot predict.
            const size_t budget = mm::moonlive::codeCapFor(mm::moonlive::countTokens(src.c_str()));
            INFO("emitted ", n, " of a ", budget, "-byte budget");
            // A script whose estimate reaches the cap gets the whole buffer, so the buffer is its limit.
            if (budget == mm::moonlive::kCodeCap) CHECK(n <= budget);
            else CHECK(n * 3 <= budget * 2);
            checked++;
        }
    }
    CHECK(checked > 0);   // a silently empty folder would pass without this
}

