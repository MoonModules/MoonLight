/// @module MoonLiveLayout
/// @also MoonLive, LayoutBase, Layouts

/// A scripted layout: where the lights physically are, written as text on a running device instead of compiled in as a C++ class. This is the binding that needed `for`, a modifier transforms one coordinate because the Layer calls it per light, but a layout has to place N lights itself.
///
/// The tests are about the contract a layout owes its container: that lightCount() answers before any coordinate is asked for (the Layer sizes its buffer from it), that the count and the coordinates agree because they come from the same script, that nothing is allocated to achieve it, and that a broken script leaves an empty fixture rather than taking the pipeline down.

#include "doctest.h"
#include "MoonLiveScriptFixture.h"
#include "moonlive_script_files.h"
#include "../core/moonlive_script_wrap.h"
#include "light/moonlive/MoonLiveLayout.h"
#include "light/moonlive/MoonLiveModifier.h"   // the cycle-break case below drives a modifier
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"

#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <thread>

using namespace mm;


// Every case here compiles a script and runs the emitted native code, so all of them need a JIT backend for the host ISA. arm64 and x86-64 both have one; a --no-jit build does not, and there a layout reports zero lights for a reason that has nothing to do with the layout. Gated as a block, the same way unit_moonlive_fill / unit_moonlive_ir do it.
#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
/// Collect what a layout emits, the way the Layer's mapping build does.
std::vector<Coord3D> place(const char* script) {
    MoonLiveLayout l;
    l.defineControls();
    if (script) l.setScript(mmWriteScript(script));
    l.prepare();

    std::vector<Coord3D> out;
    CoordSink sink{
        [](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            static_cast<std::vector<Coord3D>*>(ctx)->push_back({x, y, z});
        },
        nullptr, &out};
    l.placeLights(sink);
    return out;
}

/// The shipped backlight layout run at one wiring, collecting what it places; the wiring is the JSON a device would apply, since the script declares its own controls.
std::vector<Coord3D> placeBacklight(const char* wiring) {
    const std::string src = readText(scriptRoot() / "layouts" / "backlight.mll");
    REQUIRE_FALSE(src.empty());

    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(src.c_str()));
    l.prepare();
    auto& cs = l.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (cs[i].name) applyControlValue(cs[i], wiring, cs[i].name, ApplyPolicy::Clamp);
    l.prepare();   // the controls size the walk, so the layout is rebuilt against them

    std::vector<Coord3D> out;
    CoordSink sink{
        [](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            static_cast<std::vector<Coord3D>*>(ctx)->push_back({x, y, z});
        },
        nullptr, &out};
    l.placeLights(sink);
    return out;
}

/// The distinct cells a walk visits, for the shape-is-invariant checks below.
std::set<std::pair<int, int>> cellsOf(const std::vector<Coord3D>& p) {
    std::set<std::pair<int, int>> out;
    for (const auto& c : p) out.insert({c.x, c.y});
    return out;
}
}  // namespace

TEST_CASE("the default script lays out a grid, one light per cell") {
    // The shape almost every panel is, and the script that ships: a nested loop calling addLight.
    const std::vector<Coord3D> p = place(
        mmScriptAs("placeLights", "byte cols = 4;\n"
        "byte rows = 2;\n"
        "for (int yy = 0; yy < rows; yy = yy + 1) {"
        "  for (int xx = 0; xx < cols; xx = xx + 1) { addLight(xx, yy, 0); } }"));
    REQUIRE(p.size() == 8);
    CHECK(p[0] == Coord3D{0, 0, 0});
    CHECK(p[3] == Coord3D{3, 0, 0});
    CHECK(p[4] == Coord3D{0, 1, 0});       // the second row starts over at x=0
    CHECK(p[7] == Coord3D{3, 1, 0});
}

TEST_CASE("the light count is known before any coordinate is asked for") {
    // The layout contract: the Layer sizes its buffer from lightCount() and only then walks placeLights. A count that came from the walk would arrive too late to be useful.
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "byte cols = 5;\n"
                "byte rows = 3;\n"
                "for (int yy = 0; yy < rows; yy = yy + 1) {"
                "  for (int xx = 0; xx < cols; xx = xx + 1) { addLight(xx, yy, 0); } }")));
    l.prepare();
    CHECK(l.lightCount() == 15);           // answered without anyone calling placeLights
}

TEST_CASE("the count and the coordinates always agree, because one script produces both") {
    // The property SphereLayout names: count and emit run the same code, so they cannot drift.
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 7; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();

    std::vector<Coord3D> seen;
    CoordSink sink{[](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
                       static_cast<std::vector<Coord3D>*>(ctx)->push_back({x, y, z});
                   }, nullptr, &seen};
    l.placeLights(sink);
    CHECK(l.lightCount() == static_cast<nrOfLightsType>(seen.size()));
}

TEST_CASE("a scripted layout allocates nothing, like every other layout") {
    // A 16k-light fixture staged as coordinates would be 48 KB, memory a classic ESP32 does not have. The script calls out per light instead, so the only heap here is the compiled program.
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4096; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 4096);
    // dynamicBytes is the JIT'd program only, no coordinate storage grows with the light count.
    CHECK(l.dynamicBytes() < 1024);
}

TEST_CASE("a script places lights wherever it likes, which is the point of scripting one") {
    // A strand that runs right to left: one line here, a new C++ class otherwise.
    const std::vector<Coord3D> p = place(
        mmScriptAs("placeLights", "byte cols = 4;\n"
        "for (int i = 0; i < cols; i = i + 1) { addLight(cols - 1 - i, 0, 0); }"));
    REQUIRE(p.size() == 4);
    CHECK(p[0] == Coord3D{3, 0, 0});
    CHECK(p[3] == Coord3D{0, 0, 0});
}

TEST_CASE("a script can place a shape no rectangular layout can express") {
    // A diagonal, light i at (i, i).
    const std::vector<Coord3D> p = place(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, i, 0); }"));
    REQUIRE(p.size() == 4);
    CHECK(p[0] == Coord3D{0, 0, 0});
    CHECK(p[3] == Coord3D{3, 3, 0});
}

TEST_CASE("a broken script leaves an empty fixture rather than taking the pipeline down") {
    // Robustness, the hard rule: someone editing a layout mid-show types something wrong. The fixture reports no lights, the module carries the diagnostic, and the device keeps running.
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, i")));   // unclosed
    l.prepare();
    CHECK(l.lightCount() == 0);
    CHECK(l.severity() == MoonModule::Severity::Error);
}

TEST_CASE("editing the script changes the fixture") {
    // The live-edit loop: the same module, a new script, a different physical shape.
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 4);

    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 2);
}

// Every layout example in MoonLiveEffect.md must actually compile. A doc that shows a call the language does not have sends the reader to a parse error on their first attempt, and it happened here: an early draft advertised cos8/sin8, which are not registered built-ins.
TEST_CASE("the scripts the documentation shows all compile") {
    const char* fromDocs[] = {
        // the default
        mmScriptAs("placeLights", "byte cols = 16;\n"
        "byte rows = 16;\n"
        "for (int yy = 0; yy < rows; yy = yy + 1) {"
        "  for (int xx = 0; xx < cols; xx = xx + 1) { addLight(xx, yy, 0); } }"),
        // right to left
        mmScriptAs("placeLights", "byte cols = 8;\n"
        "for (int i = 0; i < cols; i = i + 1) { addLight(cols - 1 - i, 0, 0); }"),
        // a diagonal
        mmScriptAs("placeLights", "byte cols = 8;\n"
        "for (int i = 0; i < cols; i = i + 1) { addLight(i, i, 0); }"),
        // two rows, stacked
        mmScriptAs("placeLights", "byte cols = 8;\n"
        "for (int i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); addLight(i, 1, 0); }"),
        // print wrapping an argument
        mmScriptAs("placeLights", "for (int i = 0; i < 2; i = i + 1) { addLight(print(i), 0, 0); }"),
    };
    for (const char* s : fromDocs) {
        MoonLiveLayout l;
        l.defineControls();
        l.setScript(mmWriteScript(s));
        l.prepare();
        INFO("script: " << s);
        CHECK(l.severity() != MoonModule::Severity::Error);
        CHECK(l.lightCount() > 0);
    }
}

// The container asks a layout for its count and its coordinates SEPARATELY, and both must work at any time: not only immediately after prepare(). Layouts::prepare walks placeLights for the bounding box, and the Layer asks lightCount() when it sizes its buffer; a layout that answers only once reports an empty fixture to whichever asks second.
TEST_CASE("a layout answers count and coordinates every time it is asked") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 6; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();

    CHECK(l.lightCount() == 6);
    CHECK(l.lightCount() == 6);            // asking twice must give the same answer

    int emitted = 0;
    CoordSink counting{[](void* ctx, nrOfLightsType, lengthType, lengthType, lengthType) {
                           (*static_cast<int*>(ctx))++;
                       }, nullptr, &emitted};
    l.placeLights(counting);
    CHECK(emitted == 6);

    emitted = 0;
    l.placeLights(counting);
    CHECK(emitted == 6);                   // and walking twice, too
    CHECK(l.lightCount() == 6);            // count still right after a walk
}


// Subtraction has to produce the WHOLE value, not just a byte that happens to look right.
// `a - b` compiles to `a + (b * -1)`, and if -1 is materialized as 65535 (which it was, on two of three targets) the result is correct only modulo 256. A coordinate comparison cannot see that, both the right answer and the widened one truncate to the same byte.
//
// A layout's light INDEX can see it: the count comes from how many times addLight ran, so a loop bound computed by subtraction that came out ~65k places a wildly different number of lights.
TEST_CASE("a subtraction feeding a loop bound produces the whole value") {
    MoonLiveLayout l;
    l.defineControls();
    // 10 - 4 must be 6 lights. A widened -1 makes the bound enormous and the count is not 6.
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 10 - 4; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 6);

    // And a subtraction inside the placement, where the coordinate is the observable.
    std::vector<Coord3D> p = place(mmScriptAs("placeLights", "byte cols = 4;\n"
                                   "for (int i = 0; i < cols; i = i + 1) { addLight(cols - 1 - i, 0, 0); }"));
    REQUIRE(p.size() == 4);
    CHECK(p[0] == Coord3D{3, 0, 0});      // 4 - 1 - 0
    CHECK(p[3] == Coord3D{0, 0, 0});      // 4 - 1 - 3
}

// A slider you moved has to survive editing the script, that is the live-authoring loop, and the control arena delivers it by keeping a slot's value when the control persists across a recompile (MoonLive.h, ensureArena). The consequence, which is easy to be surprised by: the arena matches controls by OFFSET, so a DIFFERENT script whose first control happens to sit at the same offset inherits the value rather than its own initializer. Two scripts that both open with a `cols` are the same slot as far as the engine is concerned.
//
// This pins the behavior so a change to it is a decision rather than an accident. Setting the control after the edit is what makes a script's own extent authoritative.
TEST_CASE("a scripted control keeps its live value when the script is edited") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "byte cols = 16;\n"
                "for (int i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 16);

    // A second script declaring cols at the same offset inherits the live 16, not its own 8.
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "byte cols = 8;\n"
                "for (int i = 0; i < cols; i = i + 1) { addLight(i, 1, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 16);

    // A member INSERTED ABOVE cols shifts cols to the next arena byte, so the byte cols used to own now belongs to `pad`. Identity is the name at an offset, not the declaration position: pad must take its own 4 rather than inherit the 16 the user had dialed into cols.
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "byte pad = 4;\n"
                "byte cols = 7;\n"
                "for (int i = 0; i < pad; i = i + 1) { addLight(i, 2, 0); }")));
    l.prepare();
    CHECK(l.lightCount() == 4);

    // A script whose first control is a NEW slot gets its own initializer: nothing to inherit.
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "byte cols = 16;\n"
                "byte rows = 3;\n"
                "for (int yy = 0; yy < rows; yy = yy + 1) {"
                "  for (int xx = 0; xx < cols; xx = xx + 1) { addLight(xx, yy, 0); } }")));
    l.prepare();
    CHECK(l.lightCount() == 48);          // 16 inherited, rows 3 its own
}

// A layout script never fills, so it should not pay for the scratch registers a fill needs. The backends reserved them unconditionally, and on the smallest register file (Xtensa, 12) that one register was the difference between a nested loop compiling and being refused outright, the shipped default script is a nested loop, so the module's own default did not compile there.
//
// This is a register-budget property, and the budget differs per target, so what it pins portably is the behavior: a nested loop places every light of the grid it describes.
TEST_CASE("a nested loop lays out a full grid, on every target's register budget") {
    const std::vector<Coord3D> p = place(
        mmScriptAs("placeLights", "for (int yy = 0; yy < 3; yy = yy + 1) {"
        "  for (int xx = 0; xx < 5; xx = xx + 1) { addLight(xx, yy, 0); } }"));
    REQUIRE(p.size() == 15);               // 3 rows x 5 columns, none dropped
    CHECK(p[0]  == Coord3D{0, 0, 0});
    CHECK(p[4]  == Coord3D{4, 0, 0});      // end of the first row
    CHECK(p[5]  == Coord3D{0, 1, 0});      // the inner counter restarted
    CHECK(p[14] == Coord3D{4, 2, 0});
}

// A `for` counter must survive whatever the body does to it. The device backends built a light's byte address by multiplying the index register IN PLACE, fine when the index is a throwaway temp, wrong when it is the loop counter, which the step and the loop test read again afterwards. A gradient (`for (i…) { setRGB(i, …) }`) therefore ran the wrong number of times on Xtensa and RISC-V while being correct on the desktop host, which used a scratch register instead.
//
// Pinned through a LAYOUT because the count is the observable: the host executes this test, and the arithmetic the backends share is the same. addLight's index is likewise the counter.
TEST_CASE("a loop counter survives the body that uses it") {
    SUBCASE("through a call — addLight") {
        MoonLiveLayout l;
        l.defineControls();
        l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 6; i = i + 1) { addLight(i, i, 0); }")));
        l.prepare();
        CHECK(l.lightCount() == 6);      // a clobbered counter gives some other number
    }
    SUBCASE("through an inline store — setRGB") {
        // The case the bug was actually in: StoreElem folded the byte address into the index register, which for `setRGB(i, …)` is the counter itself. The count above cannot see that, addLight is a Call and takes a different path, so this drives the emitted code and checks every light was written, which is what a wrong counter changes.
        uint8_t code[4096];
        auto r = moonlive::compileSource(mmScriptAs("placeLights", "for (int i = 0; i < 6; i = i + 1) { setRGB(i, 200, 0, 0); }"),
                                         moonlive::lightBuiltins(), moonlive::modifierSysVars(), code, sizeof(code));
        REQUIRE(r.ok);
        void* blk = platform::allocExec(r.len);
        REQUIRE(blk);
        platform::writeExec(blk, code, r.len);
        uint8_t buf[6 * 3] = {};
        uint8_t arena[moonlive::kArenaBytes] = {};
        reinterpret_cast<moonlive::CtrlFn>(blk)(buf, 6, 3, 0, arena);
        for (int i = 0; i < 6; i++) {
            INFO("light " << i);
            CHECK(buf[i * 3] == 200);    // every one of the six written, none skipped or repeated
        }
        platform::freeExec(blk, r.len);
    }
}

// A malformed script must produce a diagnostic, never a hang. The `for` header's step expression is scanned by a small loop that skips to the closing paren; a lexer ERROR is not the END token, and the lexer does not move past the offending character, so the scan spun forever on a stray symbol. A device compiling that script would wedge with no message at all.
TEST_CASE("a stray character in a for header is rejected, not spun on") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i @ 1) { addLight(i, 0, 0); }")));
    l.prepare();                                    // must return — a hang fails by timeout
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.lightCount() == 0);
}

// A scripted layout is asked for its lights from more than one thread: the HTTP task runs the script when a control is edited, while the render task walks the same layout for the frame. The sink the script calls out through used to be one process-wide pair, so one thread cleared it while the other was mid-run, the built-in then called a live function pointer with a null context and the render core took a null dereference. It presented as an intermittent crash while resizing on an S3.
//
// Two threads walking their own layouts concurrently must each see their own sink.
TEST_CASE("two threads can run scripts at once without stealing each other's sink") {
    auto place = [](int cols, int reps) {
        MoonLiveLayout l;
        l.defineControls();
        char src[128];
        std::snprintf(src, sizeof(src), mmScriptAs("placeLights", "for (int i = 0; i < %d; i = i + 1) { addLight(i, 0, 0); }"), cols);
        l.setScript(mmWriteScript(src));
        l.prepare();
        for (int r = 0; r < reps; r++)
            if (l.lightCount() != static_cast<nrOfLightsType>(cols)) return false;
        return true;
    };

    bool aOk = false, bOk = false;
    std::thread a([&] { aOk = place(7, 400); });
    std::thread b([&] { bOk = place(23, 400); });
    a.join();
    b.join();
    CHECK(aOk);      // each thread counted ITS OWN script every time
    CHECK(bOk);
}

// The card's memory figure has to be the memory the module actually holds, or it is not worth reading. A scripted module owns two heap blocks, the emitted code and the control-values arena, and dynamicBytes counted only the first, so every scripted card under-reported. It also read 0 whenever the script failed to compile, while the arena was still allocated. ...and hands it all back when disabled. MoonLive::free() drops the exec block but does not touch the owner's counter, so a binding that forgets to report the release leaves a disabled module's card claiming memory nobody holds. All three scripted bindings share one helper for this.
TEST_CASE("a disabled scripted layout stops reporting the memory it freed") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    REQUIRE(l.dynamicBytes() > 0);
    l.release();
    CHECK(l.dynamicBytes() == 0);
}

TEST_CASE("a scripted layout reports every heap byte it holds, compiled or not") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "byte cols = 4;\n"
                "for (int i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); }")));
    l.prepare();
    const size_t compiled = l.dynamicBytes();
    CHECK(compiled > 0);
    CHECK(l.lightCount() == 4);

    // A broken script frees the code but keeps the arena, so the figure drops without reaching zero.
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, i")));   // unclosed
    l.prepare();
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.dynamicBytes() < compiled);      // the code block is gone
    CHECK(l.dynamicBytes() > 0);             // the arena is not
}

// The layer builds its mapping in two passes over the layouts: pass A counts destinations, pass B scatters driver indices into an array sized by that count. Both passes call placeLights: and a scripted layout COMPILES lazily inside placeLights, so a control edited between the two passes makes pass B emit more lights than pass A counted. The scatter then ran past its array and corrupted the heap; the crash surfaced later inside an unrelated allocation, which is why resizing a scripted layout failed at random rather than pointing anywhere near the layer.
//
// A layout that grows mid-build must cost a dropped destination, never memory.
TEST_CASE("a layout that changes size mid-build cannot overrun the mapping") {
    MoonLiveLayout layout;
    layout.defineControls();
    // `cols` is a CONTROL here, because the test drives it: the loop below sets it and expects the layout to resize. A member alone would not appear on the module, so this one is surfaced.
    layout.setScript(mmWriteScript(
        "class GrowLayout {\n"
        "  byte cols = 4;\n"
        "  void defineControls() { addControl(\"cols\", cols, 1, 64); }\n"
        "  void placeLights() { for (int i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); } }\n"
        "}\n"));
    layout.prepare();
    // The script's own controls (`cols`) exist only once it has COMPILED, and a module starts with no script now, so the control list has to be rebuilt after prepare() for setWidth to find it.
    layout.rebuildControls();

    mm::Layouts group;
    group.addChild(&layout);
    mm::Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    layer.defineControls();
    group.applyState();
    layer.applyState();

    auto setWidth = [&](uint8_t v) {
        const auto& cs = layout.controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (cs[i].name && std::strcmp(cs[i].name, "cols") == 0)
                *static_cast<uint8_t*>(cs[i].ptr) = v;
    };

    // Grow and shrink repeatedly, rebuilding each time, the resize loop a slider drives.
    for (uint8_t w : {4, 32, 8, 48, 16, 64, 2, 24}) {
        setWidth(w);
        group.applyState();
        layer.applyState();
        INFO("cols " << (int)w);
        CHECK(layout.lightCount() == w);
        REQUIRE(layer.buffer().data() != nullptr);
        CHECK(layer.buffer().count() >= w);
    }
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A control write lands directly in the module's buffer, addText binds it, so setScript() is NOT called. Nothing then cleared the compiled-hash, and compile()'s early-return kept the OLD program running under the new name. Found by review; the same class of bug hardware found in the effect. Needs a backend: without one BOTH counts are zero and the test passes without proving the swap.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("naming a different script through the control actually swaps the program") {
    MoonLiveLayout l;
    l.defineControls();
    const char* four = mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }"));
    l.setScript(four);
    l.prepare();
    REQUIRE(l.lightCount() == 4);

    // Write the OTHER script the way the API does: straight into the bound control buffer.
    const char* nine = mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 9; i = i + 1) { addLight(i, 0, 0); }"));
    const auto& cs = l.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (cs[i].name && std::strcmp(cs[i].name, "script") == 0)
            std::snprintf(static_cast<char*>(cs[i].ptr), 32, "%s", nine);
    l.onControlChanged("script");
    l.prepare();
    CHECK(l.lightCount() == 9);      // the new file, not the cached program
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A layout that cannot compile must stay quiet, not keep trying.
//
// The pipeline asks a layout for its size and then walks it, and BOTH ask it to compile first, so a failure that leaves "nothing is compiled" looks exactly like "not compiled yet" and every ask re-reads the file. On an ESP32 one attempt is two LittleFS operations (~5 ms), and the repeated asks during a single rebuild starved the task until the 12-second watchdog reset the board: a missing script took the whole device down rather than showing an error. The behavior to pin is that a failed layout still places no lights however many times it is asked, and says so.
TEST_CASE("a layout whose script is missing reports it without retrying forever") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript("definitely-not-there.mll");
    l.prepare();
    CHECK(l.severity() == MoonModule::Severity::Error);
    // Every ask the pipeline could make, several times over. Each one used to re-read the file.
    int placed = 0;
    CoordSink sink{[](void* ctx, nrOfLightsType, lengthType, lengthType, lengthType) {
        (*static_cast<int*>(ctx))++;
    }, nullptr, &placed};
    for (int i = 0; i < 50; i++) {
        CHECK(l.lightCount() == 0);
        l.placeLights(sink);
    }
    CHECK(placed == 0);
    CHECK(l.severity() == MoonModule::Severity::Error);   // and it still says what is wrong

    // A working script after a failed one must still compile, the give-up is per script name, not permanent, or fixing a typo would need a reboot.
    const char* good = mmScriptAs("placeLights", "for (int i = 0; i < 5; i = i + 1) { addLight(i, 0, 0); }");
    l.setScript(mmWriteScript(good));
    l.prepare();
    // The COUNT needs an emitting backend; the give-up-is-per-name behavior above does not, so only this line is gated and the rest of the case still runs on x86_64 (where CI runs).
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(l.lightCount() == 5);
#endif
}

// A layout that starts with NO script must still compile the first real one it is given.
//
// Every device boots a fresh layout card with an empty script control, so the very first compile always fails with "no script, set the script name". When the give-up flag was a bare bool that failure latched, and the card then reported "no script" forever however many valid names were set afterwards: the render loop asks for the light count long before a control write can clear a flag, so the guard re-armed itself on every tick. Bench-caught on an S3, the host never saw it because a test constructs a fresh layout per case and never boots one empty.
TEST_CASE("a layout that starts empty still compiles the first script it is given") {
    MoonLiveLayout l;
    l.defineControls();
    l.prepare();                                  // the empty-script boot: fails, as it should
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.lightCount() == 0);

    // The RENDER LOOP keeps asking while no script is set, this is the step that re-armed the flag on device and that a straight prepare/setScript sequence never reproduces.
    for (int i = 0; i < 5; i++) CHECK(l.lightCount() == 0);

    // Write the control the way the UI does, straight into the bound buffer, then onControlChanged, because addText binds `script_` directly and setScript() is NOT called on that path. That is exactly how a device sets a script, and where the latch survived.
    const char* name = mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 6; i = i + 1) { addLight(i, 0, 0); }"));
    auto& cs = l.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (cs[i].name && std::strcmp(cs[i].name, "script") == 0)
            std::snprintf(static_cast<char*>(cs[i].ptr), 32, "%s", name);
    l.onControlChanged("script");
    l.prepare();
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(l.lightCount() == 6);                   // the give-up must not have latched
#endif
}

// The fixed script directory is a boundary: a module names a file inside it, and cannot address the filesystem. Without this, a control value of "../.config/NetworkModule.json" reads the device's saved WiFi credentials as if they were a script.
TEST_CASE("a script name cannot escape the script folder") {
    MoonLiveLayout l;
    l.defineControls();
    for (const char* bad : {"../.config/NetworkModule.json", "..", "sub/dir.mll", "grid.txt"}) {
        INFO(bad);
        l.setScript(bad);
        l.prepare();
        CHECK(l.severity() == MoonModule::Severity::Error);
        CHECK(l.lightCount() == 0);
    }
}

// A script that STOPS being valid must take its lights with it. Every check in the loader returns before the compile, and the compile is what releases the previous program, so a rename, a delete or an emptied file used to leave the old code executing while the card reported the error: the fixture kept rendering a script the user had removed. The one state a user can never debug is a device that disagrees with its own status line.
TEST_CASE("a script that disappears takes its lights with it") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "addLight(1, 1, 0); addLight(2, 2, 0);")));
    l.prepare();
#if MM_MOONLIVE_HAS_HOST_JIT
    REQUIRE(l.lightCount() == 2);                      // a working script first
    CHECK(l.severity() != MoonModule::Severity::Error);
#endif

    l.setScript("gone.mll");                           // never written, so the loader rejects it
    l.prepare();
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.lightCount() == 0);                        // the old program is gone, not just unreported
}

// The name the LOADER accepts and the name the CONTROL can hold must be the same length. They were not: the control held 31 characters while the loader accepted 40, so a longer valid name was silently truncated on its way in, and truncation can cut the extension off, turning a real script into a name the loader then rejects. The user sees an extension complaint for a file that has one.
TEST_CASE("a script name at the accepted length survives the control it is stored in") {
    // A name exactly at the limit: filler + a role extension, written so the file really exists.
    std::string longName(mm::moonlive::kMaxScriptName - 4, 'a');
    longName += mm::moonlive::kLayoutExt;
    REQUIRE(longName.size() == mm::moonlive::kMaxScriptName);

    // Write a real script under that name, then name it. If the control clipped it, the loader would see a truncated name (possibly without its extension) and report an error instead.
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, longName.c_str());
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    const char* body = mmScriptAs("placeLights", "addLight(3, 3, 0);");
    mm::platform::fsWriteAtomic(path, body, std::strlen(body));
    mmScriptRegistry().push_back(path);

    MoonLiveLayout l;
    l.defineControls();
    l.setScript(longName.c_str());
    l.prepare();
    // The name reached the loader intact: a clipped one is rejected for its missing extension, so the status would name the NAME rather than anything about the script's contents. Asserted this way because a host without a MoonLive backend (x86-64) fails every compile by design, and this test is about the control buffer, not about codegen.
    if (l.severity() == MoonModule::Severity::Error)
        CHECK(std::string(l.status()).find(".mll") == std::string::npos);
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(l.severity() != MoonModule::Severity::Error);
    CHECK(l.lightCount() == 1);
#endif
}

#if MM_MOONLIVE_HAS_HOST_JIT
// A SERPENTINE over an arbitrary number of rows: every other row reversed. This was the standing example of what the language could not express, because it needs a per-row decision and there was no `if`. It is also the most common real panel wiring, so it is worth pinning as a layout rather than only as a compiler test.
TEST_CASE("a serpentine layout places every light exactly once") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights",
        "byte cols = 4;\n"
        "byte rows = 3;\n"
        "byte odd = 0;\n"
        "for (int y = 0; y < rows; y = y + 1) {\n"
        "  for (int x = 0; x < cols; x = x + 1) {\n"
        "    if (odd == 0) { addLight(x, y, 0); }\n"
        "    else { addLight(cols - 1 - x, y, 0); }\n"
        "  }\n"
        "  if (odd == 0) { odd = 1; } else { odd = 0; }\n"
        "}")));
    l.prepare();
    CHECK(l.lightCount() == 12);   // 4 x 3, every cell placed once and none twice
}

// --- editing a script's CONTENTS recompiles it -------------------------------------------------
//
// The gap this closes: a binding keyed its recompile on the script's NAME, so saving new text into the same file changed nothing. The module kept running the program built from the PREVIOUS text, and the only way to make it notice was to rename the file. That is why editing a script on its own card could not work, and it is what a file write now triggers tree-wide.
TEST_CASE("editing a script's text recompiles it, without renaming the file") {
    MoonLiveLayout l;
    l.defineControls();
    const char* name = mmWriteScript(mmScriptAs("placeLights",
        "for (int i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"));
    l.setScript(name);
    l.prepare();
    CHECK(l.lightCount() == 3);

    // Rewrite THE SAME FILE, exactly as a save from the editor does.
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    const std::string edited = mmScriptAs("placeLights",
        "for (int i = 0; i < 7; i = i + 1) { addLight(i, 0, 0); }");
    REQUIRE(mm::platform::fsWriteAtomic(path, edited.c_str(), edited.size()));

    l.prepare();
    CHECK(l.lightCount() == 7);
}

// The other half of the same rule, and the one a modifier depends on: an unchanged file must be RECOGNIZED as unchanged. A modifier turns "a new program was installed" into "ask the Layer to rebuild", and the Layer's rebuild calls prepare() again, so answering "changed" every time makes the two call each other forever and the fixture renders nothing at all.
TEST_CASE("preparing an unchanged script installs no new program") {
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);")));

    m.prepare();
    CHECK(m.consumeNeedsRebuild());     // the first compile is a real change

    m.prepare();
    CHECK_FALSE(m.consumeNeedsRebuild());   // nothing changed, so the Layer is not asked again
    m.prepare();
    CHECK_FALSE(m.consumeNeedsRebuild());
}

// A broken script that is FIXED IN PLACE compiles, without being renamed. This is the failure the editor makes routine: type a typo, see the parse error, correct it, save. Keyed on the name alone (which is what the bindings did before) the corrected script stays refused until it is renamed.
//
// NOT pinned here: that a broken script is tried ONCE rather than on every ask. The latch exists because each retry is two LittleFS reads (~5 ms on an S3) and the pipeline asks repeatedly while sizing a fixture, so the retries starve the render task until the watchdog resets the device. On the host a re-read costs microseconds and nothing observable differs, which four attempts at a test confirmed: removing the latch entirely leaves every assertion passing. Backlogged rather than papered over with a test that cannot fail.
TEST_CASE("a broken script fixed in place compiles, without being renamed") {
    MoonLiveLayout l;
    l.defineControls();
    const char* name = mmWriteScript("class T { this is not a script }\n");
    l.setScript(name);
    l.prepare();
    CHECK(l.lightCount() == 0);          // refused, and the card carries the parse error
    REQUIRE_FALSE(std::string(l.status()).empty());

    // Fix it IN PLACE, under the same name, exactly as saving from the editor does.
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    const std::string fixed = mmScriptAs("placeLights",
        "for (int i = 0; i < 5; i = i + 1) { addLight(i, 0, 0); }");
    REQUIRE(mm::platform::fsWriteAtomic(path, fixed.c_str(), fixed.size()));

    CHECK(l.lightCount() == 5);          // the content moved, so the failure latch released
}




// A script's ROLE is its file extension: `.mle` an effect, `.mll` a layout, `.mlm` a modifier. It is stated by the author rather than derived from what the class defines, so that adding (say) a per-frame tick() to modifiers later cannot silently start listing them in effect pickers.
//
// The LOADER is role-blind and accepts all three, exactly as the engine is: which picker offered a file is the binding's business, and a class may serve several moments. What the extension decides is which card offers the file, not what the engine will do with it.
TEST_CASE("the loader accepts any role extension, and nothing else") {
    MoonLiveLayout l;
    l.defineControls();
    for (const char* ext : {".mle", ".mll", ".mlm"}) {
        std::string name = std::string("roletest") + ext;
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name.c_str());
        mm::platform::fsMkdir(mm::moonlive::kScriptDir);
        const char* body = mmScriptAs("placeLights", "addLight(1, 1, 0);");
        REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
        mmScriptRegistry().push_back(path);

        l.setScript(name.c_str());
        l.prepare();
        INFO("extension " << ext);
        CHECK(std::string(l.status()).find("must end in") == std::string::npos);
    }
    // Anything else is refused with the three it accepts, rather than a bare "bad name".
    l.setScript("notascript.txt");
    l.prepare();
    CHECK(std::string(l.status()).find("must end in") != std::string::npos);
}

// A disabled scripted module must not publish controls bound into a freed control arena.
// release() frees the engine's arena; the control descriptors registered by defineControls() hold raw pointers into it, so re-publishing them would read (and a UI write would WRITE) freed heap.
// Symptom on hardware: reading a disabled MoonLive card returned a different value every read.
TEST_CASE("a disabled scripted module publishes no controls bound to freed memory") {
    MoonLiveLayout l;
    l.rebuildControls();
    // A script with its OWN control, which is what binds a pointer into the engine's arena.
    l.setScript(mmWriteScript(
        "class T {\n"
        "  byte cols = 7;\n"
        "  void defineControls() { addControl(\"cols\", cols, 1, 64); }\n"
        "  void placeLights() { for (int x = 0; x < cols; x = x + 1) { addLight(x, 0, 0); } }\n"
        "}\n"));
    l.prepare();

    // Compiled: the script's own control is published alongside "script".
    l.rebuildControls();
    const uint8_t live = l.controls().count();
    CHECK(live > 1);
    bool sawCols = false;
    for (uint8_t i = 0; i < live; i++)
        if (std::strcmp(l.controls()[i].name, "cols") == 0) sawCols = true;
    CHECK(sawCols);

    // Disabled: the arena is gone, so the scripted control must go with it.
    l.release();
    l.rebuildControls();
    for (uint8_t i = 0; i < l.controls().count(); i++) {
        INFO("control " << i << " = " << l.controls()[i].name);
        CHECK(std::strcmp(l.controls()[i].name, "cols") != 0);
    }
    CHECK(l.controls().count() < live);
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A control write lands directly in the module's buffer, addText binds it, so setScript() is NOT called. Nothing then cleared the compiled-hash, and compile()'s early-return kept the OLD program running under the new name. Found by review; the same class of bug hardware found in the effect. Needs a backend: without one BOTH counts are zero and the test passes without proving the swap.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("naming a different script through the control actually swaps the program") {
    MoonLiveLayout l;
    l.defineControls();
    const char* four = mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }"));
    l.setScript(four);
    l.prepare();
    REQUIRE(l.lightCount() == 4);

    // Write the OTHER script the way the API does: straight into the bound control buffer.
    const char* nine = mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 9; i = i + 1) { addLight(i, 0, 0); }"));
    const auto& cs = l.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (cs[i].name && std::strcmp(cs[i].name, "script") == 0)
            std::snprintf(static_cast<char*>(cs[i].ptr), 32, "%s", nine);
    l.onControlChanged("script");
    l.prepare();
    CHECK(l.lightCount() == 9);      // the new file, not the cached program
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A layout that cannot compile must stay quiet, not keep trying.
//
// The pipeline asks a layout for its size and then walks it, and BOTH ask it to compile first, so a failure that leaves "nothing is compiled" looks exactly like "not compiled yet" and every ask re-reads the file. On an ESP32 one attempt is two LittleFS operations (~5 ms), and the repeated asks during a single rebuild starved the task until the 12-second watchdog reset the board: a missing script took the whole device down rather than showing an error. The behavior to pin is that a failed layout still places no lights however many times it is asked, and says so.
TEST_CASE("a layout whose script is missing reports it without retrying forever") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript("definitely-not-there.mll");
    l.prepare();
    CHECK(l.severity() == MoonModule::Severity::Error);
    // Every ask the pipeline could make, several times over. Each one used to re-read the file.
    int placed = 0;
    CoordSink sink{[](void* ctx, nrOfLightsType, lengthType, lengthType, lengthType) {
        (*static_cast<int*>(ctx))++;
    }, nullptr, &placed};
    for (int i = 0; i < 50; i++) {
        CHECK(l.lightCount() == 0);
        l.placeLights(sink);
    }
    CHECK(placed == 0);
    CHECK(l.severity() == MoonModule::Severity::Error);   // and it still says what is wrong

    // A working script after a failed one must still compile, the give-up is per script name, not permanent, or fixing a typo would need a reboot.
    const char* good = mmScriptAs("placeLights", "for (int i = 0; i < 5; i = i + 1) { addLight(i, 0, 0); }");
    l.setScript(mmWriteScript(good));
    l.prepare();
    // The COUNT needs an emitting backend; the give-up-is-per-name behavior above does not, so only this line is gated and the rest of the case still runs on x86_64 (where CI runs).
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(l.lightCount() == 5);
#endif
}

// A layout that starts with NO script must still compile the first real one it is given.
//
// Every device boots a fresh layout card with an empty script control, so the very first compile always fails with "no script, set the script name". When the give-up flag was a bare bool that failure latched, and the card then reported "no script" forever however many valid names were set afterwards: the render loop asks for the light count long before a control write can clear a flag, so the guard re-armed itself on every tick. Bench-caught on an S3, the host never saw it because a test constructs a fresh layout per case and never boots one empty.
TEST_CASE("a layout that starts empty still compiles the first script it is given") {
    MoonLiveLayout l;
    l.defineControls();
    l.prepare();                                  // the empty-script boot: fails, as it should
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.lightCount() == 0);

    // The RENDER LOOP keeps asking while no script is set, this is the step that re-armed the flag on device and that a straight prepare/setScript sequence never reproduces.
    for (int i = 0; i < 5; i++) CHECK(l.lightCount() == 0);

    // Write the control the way the UI does, straight into the bound buffer, then onControlChanged, because addText binds `script_` directly and setScript() is NOT called on that path. That is exactly how a device sets a script, and where the latch survived.
    const char* name = mmWriteScript(mmScriptAs("placeLights", "for (int i = 0; i < 6; i = i + 1) { addLight(i, 0, 0); }"));
    auto& cs = l.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (cs[i].name && std::strcmp(cs[i].name, "script") == 0)
            std::snprintf(static_cast<char*>(cs[i].ptr), 32, "%s", name);
    l.onControlChanged("script");
    l.prepare();
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(l.lightCount() == 6);                   // the give-up must not have latched
#endif
}

// The fixed script directory is a boundary: a module names a file inside it, and cannot address the filesystem. Without this, a control value of "../.config/NetworkModule.json" reads the device's saved WiFi credentials as if they were a script.
TEST_CASE("a script name cannot escape the script folder") {
    MoonLiveLayout l;
    l.defineControls();
    for (const char* bad : {"../.config/NetworkModule.json", "..", "sub/dir.mll", "grid.txt"}) {
        INFO(bad);
        l.setScript(bad);
        l.prepare();
        CHECK(l.severity() == MoonModule::Severity::Error);
        CHECK(l.lightCount() == 0);
    }
}

// A script that STOPS being valid must take its lights with it. Every check in the loader returns before the compile, and the compile is what releases the previous program, so a rename, a delete or an emptied file used to leave the old code executing while the card reported the error: the fixture kept rendering a script the user had removed. The one state a user can never debug is a device that disagrees with its own status line.
TEST_CASE("a script that disappears takes its lights with it") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights", "addLight(1, 1, 0); addLight(2, 2, 0);")));
    l.prepare();
#if MM_MOONLIVE_HAS_HOST_JIT
    REQUIRE(l.lightCount() == 2);                      // a working script first
    CHECK(l.severity() != MoonModule::Severity::Error);
#endif

    l.setScript("gone.mll");                           // never written, so the loader rejects it
    l.prepare();
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.lightCount() == 0);                        // the old program is gone, not just unreported
}

// The name the LOADER accepts and the name the CONTROL can hold must be the same length. They were not: the control held 31 characters while the loader accepted 40, so a longer valid name was silently truncated on its way in, and truncation can cut the extension off, turning a real script into a name the loader then rejects. The user sees an extension complaint for a file that has one.
TEST_CASE("a script name at the accepted length survives the control it is stored in") {
    // A name exactly at the limit: filler + a role extension, written so the file really exists.
    std::string longName(mm::moonlive::kMaxScriptName - 4, 'a');
    longName += mm::moonlive::kLayoutExt;
    REQUIRE(longName.size() == mm::moonlive::kMaxScriptName);

    // Write a real script under that name, then name it. If the control clipped it, the loader would see a truncated name (possibly without its extension) and report an error instead.
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, longName.c_str());
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    const char* body = mmScriptAs("placeLights", "addLight(3, 3, 0);");
    mm::platform::fsWriteAtomic(path, body, std::strlen(body));
    mmScriptRegistry().push_back(path);

    MoonLiveLayout l;
    l.defineControls();
    l.setScript(longName.c_str());
    l.prepare();
    // The name reached the loader intact: a clipped one is rejected for its missing extension, so the status would name the NAME rather than anything about the script's contents. Asserted this way because a host without a MoonLive backend (x86-64) fails every compile by design, and this test is about the control buffer, not about codegen.
    if (l.severity() == MoonModule::Severity::Error)
        CHECK(std::string(l.status()).find(".mll") == std::string::npos);
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(l.severity() != MoonModule::Severity::Error);
    CHECK(l.lightCount() == 1);
#endif
}

#if MM_MOONLIVE_HAS_HOST_JIT
// A SERPENTINE over an arbitrary number of rows: every other row reversed. This was the standing example of what the language could not express, because it needs a per-row decision and there was no `if`. It is also the most common real panel wiring, so it is worth pinning as a layout rather than only as a compiler test.
TEST_CASE("a serpentine layout places every light exactly once") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(mmScriptAs("placeLights",
        "byte cols = 4;\n"
        "byte rows = 3;\n"
        "byte odd = 0;\n"
        "for (int y = 0; y < rows; y = y + 1) {\n"
        "  for (int x = 0; x < cols; x = x + 1) {\n"
        "    if (odd == 0) { addLight(x, y, 0); }\n"
        "    else { addLight(cols - 1 - x, y, 0); }\n"
        "  }\n"
        "  if (odd == 0) { odd = 1; } else { odd = 0; }\n"
        "}")));
    l.prepare();
    CHECK(l.lightCount() == 12);   // 4 x 3, every cell placed once and none twice
}

// --- editing a script's CONTENTS recompiles it -------------------------------------------------
//
// The gap this closes: a binding keyed its recompile on the script's NAME, so saving new text into the same file changed nothing. The module kept running the program built from the PREVIOUS text, and the only way to make it notice was to rename the file. That is why editing a script on its own card could not work, and it is what a file write now triggers tree-wide.
TEST_CASE("editing a script's text recompiles it, without renaming the file") {
    MoonLiveLayout l;
    l.defineControls();
    const char* name = mmWriteScript(mmScriptAs("placeLights",
        "for (int i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"));
    l.setScript(name);
    l.prepare();
    CHECK(l.lightCount() == 3);

    // Rewrite THE SAME FILE, exactly as a save from the editor does.
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    const std::string edited = mmScriptAs("placeLights",
        "for (int i = 0; i < 7; i = i + 1) { addLight(i, 0, 0); }");
    REQUIRE(mm::platform::fsWriteAtomic(path, edited.c_str(), edited.size()));

    l.prepare();
    CHECK(l.lightCount() == 7);
}

// The other half of the same rule, and the one a modifier depends on: an unchanged file must be RECOGNIZED as unchanged. A modifier turns "a new program was installed" into "ask the Layer to rebuild", and the Layer's rebuild calls prepare() again, so answering "changed" every time makes the two call each other forever and the fixture renders nothing at all.
TEST_CASE("preparing an unchanged script installs no new program") {
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);")));

    m.prepare();
    CHECK(m.consumeNeedsRebuild());     // the first compile is a real change

    m.prepare();
    CHECK_FALSE(m.consumeNeedsRebuild());   // nothing changed, so the Layer is not asked again
    m.prepare();
    CHECK_FALSE(m.consumeNeedsRebuild());
}

// A broken script that is FIXED IN PLACE compiles, without being renamed. This is the failure the editor makes routine: type a typo, see the parse error, correct it, save. Keyed on the name alone (which is what the bindings did before) the corrected script stays refused until it is renamed.
//
// NOT pinned here: that a broken script is tried ONCE rather than on every ask. The latch exists because each retry is two LittleFS reads (~5 ms on an S3) and the pipeline asks repeatedly while sizing a fixture, so the retries starve the render task until the watchdog resets the device. On the host a re-read costs microseconds and nothing observable differs, which four attempts at a test confirmed: removing the latch entirely leaves every assertion passing. Backlogged rather than papered over with a test that cannot fail.
TEST_CASE("a broken script fixed in place compiles, without being renamed") {
    MoonLiveLayout l;
    l.defineControls();
    const char* name = mmWriteScript("class T { this is not a script }\n");
    l.setScript(name);
    l.prepare();
    CHECK(l.lightCount() == 0);          // refused, and the card carries the parse error
    REQUIRE_FALSE(std::string(l.status()).empty());

    // Fix it IN PLACE, under the same name, exactly as saving from the editor does.
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    const std::string fixed = mmScriptAs("placeLights",
        "for (int i = 0; i < 5; i = i + 1) { addLight(i, 0, 0); }");
    REQUIRE(mm::platform::fsWriteAtomic(path, fixed.c_str(), fixed.size()));

    CHECK(l.lightCount() == 5);          // the content moved, so the failure latch released
}




// A script's ROLE is its file extension: `.mle` an effect, `.mll` a layout, `.mlm` a modifier. It is stated by the author rather than derived from what the class defines, so that adding (say) a per-frame tick() to modifiers later cannot silently start listing them in effect pickers.
//
// The LOADER is role-blind and accepts all three, exactly as the engine is: which picker offered a file is the binding's business, and a class may serve several moments. What the extension decides is which card offers the file, not what the engine will do with it.
TEST_CASE("the loader accepts any role extension, and nothing else") {
    MoonLiveLayout l;
    l.defineControls();
    for (const char* ext : {".mle", ".mll", ".mlm"}) {
        std::string name = std::string("roletest") + ext;
        char path[128];
        std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name.c_str());
        mm::platform::fsMkdir(mm::moonlive::kScriptDir);
        const char* body = mmScriptAs("placeLights", "addLight(1, 1, 0);");
        REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
        mmScriptRegistry().push_back(path);

        l.setScript(name.c_str());
        l.prepare();
        INFO("extension " << ext);
        CHECK(std::string(l.status()).find("must end in") == std::string::npos);
    }
    // Anything else is refused with the three it accepts, rather than a bare "bad name".
    l.setScript("notascript.txt");
    l.prepare();
    CHECK(std::string(l.status()).find("must end in") != std::string::npos);
}


// --- The shipped backlight layout: its controls choose an ORDER, not a shape ------------------

TEST_CASE("backlight.mll: each edge contributes its own count, each corner once") {
    const auto p = placeBacklight("{\"wallCols\":60,\"wallRows\":34,\"top\":60,\"right\":34,\"bottom\":60,\"left\":34}");
    CHECK(p.size() == 184);   // 59 + 33 + 59 + 33: every edge one short of its own count, the corners shared

    const auto cells = cellsOf(p);
    CHECK(cells.size() == p.size());   // no cell twice, so no corner counted by both its sides
    // On the edges and filling the box: a light off the perimeter would light the wall's middle.
    for (const auto& c : p) {
        CHECK(c.z == 0);
        CHECK((c.x == 0 || c.x == 59 || c.y == 0 || c.y == 33));
    }
    CHECK(cells.count({0, 0}) == 1);
    CHECK(cells.count({59, 0}) == 1);
    CHECK(cells.count({59, 33}) == 1);
    CHECK(cells.count({0, 33}) == 1);
}

// Written out, not compared to each other: "the eight differ" passes on a walk visiting the right cells in the wrong order. A 4x3 wall is ten lights, so each wiring is one line.
TEST_CASE("backlight.mll: each wiring starts at its corner and runs its own way round") {
    struct Wiring { int corner; int reverse; const char* walk; };
    // Coordinates as "xy", clockwise meaning x rising along the top, as the canonical walk does.
    const Wiring cases[8] = {
        {0, 0, "00 10 20 30 31 32 22 12 02 01"},   // top-left, forward: the shipped default
        {0, 1, "00 01 02 12 22 32 31 30 20 10"},   // and the same corner reversed
        {1, 0, "30 31 32 22 12 02 01 00 10 20"},
        {1, 1, "30 20 10 00 01 02 12 22 32 31"},
        {2, 0, "32 22 12 02 01 00 10 20 30 31"},
        {2, 1, "32 31 30 20 10 00 01 02 12 22"},
        {3, 0, "02 01 00 10 20 30 31 32 22 12"},
        {3, 1, "02 12 22 32 31 30 20 10 00 01"},
    };
    for (const auto& c : cases) {
        char wiring[160];
        std::snprintf(wiring, sizeof(wiring),
                      "{\"wallCols\":4,\"wallRows\":3,\"top\":4,\"right\":3,\"bottom\":4,\"left\":3,"
                      "\"startCorner\":%d,\"reverse\":%d}", c.corner, c.reverse);
        const auto p = placeBacklight(wiring);
        REQUIRE(p.size() == 10);
        std::string got;
        for (const auto& k : p) {
            if (!got.empty()) got += ' ';
            got += static_cast<char>('0' + k.x);
            got += static_cast<char>('0' + k.y);
        }
        CHECK(got == std::string(c.walk));
    }
}

TEST_CASE("backlight.mll: offset slides the strand along itself, whichever way it runs") {
    const auto at0 = placeBacklight("{\"wallCols\":60,\"wallRows\":34,\"top\":60,\"right\":34,\"bottom\":60,\"left\":34}");
    const auto at2 = placeBacklight("{\"wallCols\":60,\"wallRows\":34,\"top\":60,\"right\":34,\"bottom\":60,\"left\":34,\"position\":2}");
    REQUIRE(at0.size() == at2.size());
    CHECK(cellsOf(at0) == cellsOf(at2));

    // A ROTATION by two: light i of the offset walk is light i+2 of the plain one.
    for (size_t i = 0; i < at0.size(); i++) {
        const auto& expect = at0[(i + 2) % at0.size()];
        CHECK(at2[i].x == expect.x);
        CHECK(at2[i].y == expect.y);
    }

    // Along the STRAND, so a reversed one moves the other way round the frame. Counted clockwise regardless, an offset would send a reversed strand backwards past its own corner.
    const auto ccw0 = placeBacklight("{\"wallCols\":4,\"wallRows\":3,\"top\":4,\"right\":3,\"bottom\":4,\"left\":3,\"reverse\":1}");
    const auto ccw2 = placeBacklight("{\"wallCols\":4,\"wallRows\":3,\"top\":4,\"right\":3,\"bottom\":4,\"left\":3,\"reverse\":1,\"position\":2}");
    REQUIRE(ccw0.size() == 10);
    REQUIRE(ccw2.size() == 10);
    for (size_t i = 0; i < ccw0.size(); i++) {
        const auto& expect = ccw0[(i + 2) % ccw0.size()];
        CHECK(ccw2[i].x == expect.x);
        CHECK(ccw2[i].y == expect.y);
    }
}

// Hyperion's gap: the lights a strand does not have. A length and a place in the strand, not a dark edge, so the stretch a TV stand covers can sit mid-edge.
TEST_CASE("backlight.mll: a gap removes a run of lights and leaves the rest where they were") {
    const char* full = "{\"wallCols\":8,\"wallRows\":6,\"top\":8,\"right\":4,\"bottom\":8,\"left\":4}";
    const char* gapped = "{\"wallCols\":8,\"wallRows\":6,\"top\":8,\"right\":4,\"bottom\":8,\"left\":4,"
                         "\"gapStart\":11,\"gapLength\":3}";
    const auto whole = placeBacklight(full);
    const auto p = placeBacklight(gapped);
    REQUIRE(whole.size() == 20);
    REQUIRE(p.size() == 17);   // the three it does not have

    std::string got;
    for (const auto& k : p) {
        if (!got.empty()) got += ' ';
        got += static_cast<char>('0' + k.x);
        got += static_cast<char>('0' + k.y);
    }
    // The bottom loses its middle three; every other light sits exactly where it did.
    CHECK(got == std::string("00 10 20 30 40 50 60 70 71 73 75 35 25 15 05 04 02"));

    // A gap REMOVES, it does not move: each remaining cell is one the whole strand also had.
    const auto had = cellsOf(whole);
    for (const auto& k : p) CHECK(had.count({k.x, k.y}) == 1);
}

// A strand with fewer lights than the edge has pixels gets them spread along it, so one script serves a 30-light strip on a 128-pixel wall. The corners still land on the box.
TEST_CASE("backlight.mll: a strand coarser than the wall still spans its edges") {
    const auto p = placeBacklight("{\"wallCols\":128,\"wallRows\":128,\"top\":30,\"right\":20,\"bottom\":30,\"left\":20}");
    CHECK(p.size() == 2 * 29 + 2 * 19);

    int maxX = 0, maxY = 0;
    for (const auto& c : p) {
        maxX = c.x > maxX ? c.x : maxX;
        maxY = c.y > maxY ? c.y : maxY;
    }
    // The bounding box stays the wall's, so the strip sits on its edge pixels rather than inside them.
    CHECK(maxX == 127);
    CHECK(maxY == 127);
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT: the script must COMPILE for the count to mean anything.
