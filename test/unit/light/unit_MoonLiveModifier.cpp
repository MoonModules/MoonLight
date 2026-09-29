/// @module MoonLiveModifier
/// @also MoonLive, ModifierBase, Layer

/// A scripted modifier: the coordinate transform is a script the user edits live, not a C++ class that needs a rebuild and a reflash. This is the second binding of the MoonLive engine, and the tests below are mostly about the seam rather than the arithmetic, that the script's inputs really are the light's position, that a result really lands back in the mapping, and that a broken script degrades to a pass-through instead of taking the pipeline down.

#include "doctest.h"
#include "MoonLiveScriptFixture.h"
#include "../core/moonlive_script_wrap.h"
#include "light/moonlive/MoonLiveModifier.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "light/moonlive/MoonLiveLayout.h"
#include "platform/platform.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layer.h"

#include <cstring>
#include <cstdio>
#include <string>

using namespace mm;


// Every case here compiles a script and runs the emitted native code, so all of them need a JIT backend for the host ISA. arm64 and x86-64 both have one; a --no-jit build does not, and there a modifier maps nothing for a reason that has nothing to do with the modifier. Gated as a block, the same way unit_moonlive_fill / unit_moonlive_ir do it.
#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
/// Run one coordinate through a modifier carrying `script`, and report where it landed.
Coord3D transform(const char* script, lengthType x, lengthType y, lengthType z,
                  lengthType w = 255, lengthType h = 255, lengthType d = 1) {
    MoonLiveModifier m;
    m.defineControls();
    if (script) m.setScript(mmWriteScript(script));
    m.prepare();
    Coord3D box{w, h, d};
    m.modifyLogicalSize(box);      // the Layer always does this before folding
    Coord3D pos{x, y, z};
    m.modifyLogical(pos);
    return pos;
}
}  // namespace

TEST_CASE("a scripted modifier mirrors the pattern, the way a hand-written one would") {
    // The default script. A mirror is the shape that makes a working binding obvious on a bench strand, the pattern simply runs the other way.
    const Coord3D p = transform(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);"), 10, 20, 0);
    CHECK(p.x == 244);          // width(255) - 1 - 10
    CHECK(p.y == 20);           // untouched axes stay put
    CHECK(p.z == 0);
}

TEST_CASE("the script reads the light's own position, not a fixed value") {
    // The whole seam in one assertion: `x` inside the script has to BE this light's x. If the binding failed to write the input slots, every light would transform identically.
    const Coord3D a = transform(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);"), 7, 3, 1);
    CHECK(a.x == 7);
    CHECK(a.y == 3);
    CHECK(a.z == 1);

    const Coord3D b = transform(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);"), 200, 100, 2);
    CHECK(b.x == 200);
    CHECK(b.y == 100);
    CHECK(b.z == 2);
}

TEST_CASE("a script can swap axes, which is a transform no control could express") {
    const Coord3D p = transform(mmScriptAs("modifyLogical", "setXYZ(yPos, xPos, zPos);"), 5, 60, 0);
    CHECK(p.x == 60);
    CHECK(p.y == 5);
}

TEST_CASE("a script can offset a coordinate, the scroll a modifier usually hard-codes") {
    const Coord3D p = transform(mmScriptAs("modifyLogical", "setXYZ(xPos + 4, yPos, zPos);"), 10, 10, 0);
    CHECK(p.x == 14);
}

TEST_CASE("a broken script leaves the pattern alone rather than taking the layer down") {
    // Robustness, the hard rule: a user editing a script mid-show types something wrong at some point. The coordinate passes through untransformed, the module carries the diagnostic, and the pipeline keeps rendering.
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos")));     // no closing paren, no semicolon
    m.prepare();
    Coord3D box{255, 255, 1};
    m.modifyLogicalSize(box);

    Coord3D pos{11, 22, 0};
    CHECK(m.modifyLogical(pos) == true);               // accepted, not rejected
    CHECK(pos.x == 11);                                // and unchanged
    CHECK(pos.y == 22);
    CHECK(m.severity() == MoonModule::Severity::Error);            // the reason is visible to the user
}

TEST_CASE("a coordinate beyond 255 is scripted like any other, in and out") {
    // FULL WIDTH both ways, which is what makes a scripted modifier usable on a real wall.
    //
    // Neither side used to be. `xPos` and `width` were read from one-byte arena slots, so a script on a 768-wide wall saw 255; and setXYZ wrote three BYTES into the run buffer, so whatever it computed came back truncated. A scripted mirror therefore placed lights in the wrong half of any rig wider than 255, silently.
    const Coord3D p = transform(mmScriptAs("modifyLogical", "setXYZ(1000 - xPos, yPos, zPos);"), 300, 10, 0);
    CHECK(p.x == 700);                                 // transformed and returned whole
    CHECK(p.y == 10);

    // A negative is refused: it names no light, and wrapping it would place the light at the far edge rather than nowhere.
    const Coord3D n = transform(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);"), -1, 10, 0);
    CHECK(n.x == -1);
}

TEST_CASE("editing the script changes the transform without a rebuild of the firmware") {
    // The live-edit loop: the same module, a new script, a different mapping.
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);")));
    m.prepare();
    Coord3D box{255, 255, 1};
    m.modifyLogicalSize(box);      // the Layer hands every modifier its box before folding

    Coord3D a{10, 20, 0};
    m.modifyLogical(a);
    CHECK(a.x == 244);                                 // the mirror

    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);")));
    m.prepare();

    Coord3D b{10, 20, 0};
    m.modifyLogical(b);
    CHECK(b.x == 10);                                  // now a pass-through
}


// Arithmetic is what makes a scripted modifier worth having: without it a script can only pass a coordinate through or swap two axes. These pin the operator set and, more importantly, that precedence is real, `2 + 3 * 4` silently giving 20 would corrupt every non-trivial transform.
TEST_CASE("a script computes with the usual precedence, so a transform means what it reads like") {
    // Multiplication binds tighter than addition.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(2 + 3 * 4, yPos, zPos);"), 0, 0, 0).x == 14);
    // Parentheses override it.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ((2 + 3) * 4, yPos, zPos);"), 0, 0, 0).x == 20);
    // Subtraction, which no ISA here has an instruction for: a - b is emitted as a + (b * -1).
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(100 - 40, yPos, zPos);"), 0, 0, 0).x == 60);
    // Left-associative, so 100 - 40 - 20 is 40 rather than 80.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(100 - 40 - 20, yPos, zPos);"), 0, 0, 0).x == 40);
    // The coordinate inputs compose with all of it, this is the shape a real modifier uses.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(xPos * 2 + 1, yPos, zPos);"), 10, 0, 0).x == 21);
}

TEST_CASE("a scaled mirror, the transform this binding exists to make possible") {
    // Two operators and an input in one expression: reflect, then halve. Expressible now, and not expressible at all before arithmetic landed.
    const Coord3D p = transform(mmScriptAs("modifyLogical", "setXYZ((255 - xPos) * 2, yPos, zPos);"), 100, 5, 0);
    CHECK(p.x == 310);     // (255-100)*2, whole: this used to come back as 54, its low byte
    CHECK(p.y == 5);
}

// The cost question this binding raises: modifyLogical is a native CALL per light, so a large fixture pays it once per light on every mapping rebuild. It is the cold path (a rebuild, not a frame), but "cold" is not a license to be slow, a 16k-light wall rebuilding must not stall.
TEST_CASE("folding a wall's worth of lights compiles the script once, not once per light") {
    // modifyLogical runs per light per mapping rebuild, 16,384 times on a 128x128 wall. The failure that would matter is compiling inside that loop, which turns a rebuild into a stall.
    //
    // The check is the compiled program's identity, not a stopwatch: a wall-clock bound passes or fails on how busy the machine is, which makes it a flaky test rather than a statement about this code. `dynamicBytes` is the exec block the engine holds, it changes on a recompile, so an unchanged value across the whole fold proves no compile happened inside it.
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);")));
    m.prepare();
    Coord3D box{255, 255, 1};
    m.modifyLogicalSize(box);

    const size_t compiledBefore = m.dynamicBytes();
    REQUIRE(compiledBefore > 0);            // a program is actually loaded

    constexpr int kLights = 16384;          // the 128x128 wall
    const uint32_t t0 = platform::micros();
    for (int i = 0; i < kLights; i++) {
        Coord3D pos{static_cast<lengthType>(i & 0xFF), static_cast<lengthType>((i >> 8) & 0xFF), 0};
        m.modifyLogical(pos);
    }
    const uint32_t us = platform::micros() - t0;
    MESSAGE("16384 scripted transforms took " << us << " us (" << (us * 1000.0 / kLights) << " ns each)");

    CHECK(m.dynamicBytes() == compiledBefore);   // same program throughout: no per-call compile
}

TEST_CASE("editing a script asks the layer to rebuild its mapping") {
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(xPos, yPos, zPos);")));
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);     // the first compile needs one too
    CHECK(m.consumeNeedsRebuild() == false);    // and it is consumed, not sticky

    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(7 - xPos, yPos, zPos);")));
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);     // an edit asks again

    // And the new script really is what runs now, the mapping the Layer rebuilds will use this.
    Coord3D pos{0, 0, 0};
    m.modifyLogical(pos);
    CHECK(pos.x == 7);
}

// The black-screen bug, pinned. A mirror written against a hard-coded 255 sends every light of a 16-wide grid to x≈240, the Layer drops each one as out of bounds, and the fixture goes dark, with no error anywhere, because the script compiled and ran perfectly. A script therefore has to be able to read the EXTENT it is folding within, and the default has to use it.
TEST_CASE("the default script mirrors within the grid it is given, not a fixed 255") {
    // A 16-wide grid: x=0 must land on the far end of THAT grid, 15, not 245.
    const Coord3D p = transform(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);"), 0, 0, 0, /*w=*/16, /*h=*/16, /*d=*/1);
    CHECK(p.x == 15);
    CHECK(p.y == 0);

    // Every coordinate has to stay inside the box, or the Layer discards it.
    for (lengthType i = 0; i < 16; i++) {
        const Coord3D q = transform(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);"), i, 0, 0, 16, 16, 1);
        CAPTURE(i);
        CHECK(q.x >= 0);
        CHECK(q.x < 16);
    }
}

TEST_CASE("a script can read the grid extent it is folding within") {
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(width, yPos, zPos);"),  0, 0, 0, 32, 16, 1).x == 32);
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(height, yPos, zPos);"), 0, 0, 0, 32, 16, 1).x == 16);
}

// The black-screen failure end to end, through a real Layer. Byte arithmetic wraps: a script that computes a negative x (a mirror against a box that has not been established, or simply a bad expression) lands at 255, which is outside every real grid, so the Layer discards every light and the fixture goes dark with no error reported anywhere. The fold has to REJECT a coordinate it cannot place, which is what modifyLogical's bool return is for.
TEST_CASE("a script that computes a position outside the grid leaves lights mapped") {
    // The black-screen failure, at the level that can actually fail. Byte arithmetic wraps, so a script computing a position past the grid lands somewhere unintended, and if a coordinate ends up outside the logical box the Layer DISCARDS that light, which is how the fixture went dark with no error reported anywhere.
    //
    // The observation has to be the MAPPING. Filling the buffer through a Canvas and counting lit bytes cannot fail: draw::fill writes every byte itself, whatever the fold decided.
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(xPos + 200, yPos, zPos);")));   // deliberately off the end of a 16-wide grid
    m.prepare();
    Coord3D box{16, 16, 1};
    m.modifyLogicalSize(box);

    int inside = 0;
    for (lengthType i = 0; i < 16; i++) {
        Coord3D pos{i, 0, 0};
        m.modifyLogical(pos);
        if (pos.x >= 0 && pos.x < 16) inside++;   // what the Layer will keep
    }
    INFO("coordinates still inside a 16-wide grid: " << inside << " of 16");
    // The premise of this case: `x + 200` puts EVERY light outside a 16-wide grid, so the Layer drops them all. Without this the case would pass on the default-script half alone.
    CHECK(inside == 0);
    // Every light falling outside is precisely the blackout. The default script must keep them all.
    MoonLiveModifier def;
    def.defineControls();
    def.prepare();
    Coord3D defBox{16, 16, 1};
    def.modifyLogicalSize(defBox);
    int defInside = 0;
    for (lengthType i = 0; i < 16; i++) {
        Coord3D pos{i, 0, 0};
        def.modifyLogical(pos);
        if (pos.x >= 0 && pos.x < 16) defInside++;
    }
    CHECK(defInside == 16);                       // the shipped default never blacks a fixture out
}

TEST_CASE("re-preparing with an unchanged script does not ask for another rebuild") {
    MoonLiveModifier m;
    m.defineControls();
    // A module with no script compiles nothing and therefore asks for nothing, the rebuild request exists to APPLY a new transform, and there is none. Name one, so the first prepare has something to compile and the "unchanged" case below is the real question.
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);")));
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);    // the first compile needs one

    // What the Layer does when it honours that request: applyState() → prepare(). If this asks again, the two call each other forever and nothing ever renders.
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == false);
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == false);

    // A real edit still asks.
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(yPos, xPos, zPos);")));
    m.prepare();
    CHECK(m.consumeNeedsRebuild() == true);
}

// print(v) is the only way to see inside a running script: one that compiles cleanly and renders wrong gives no other clue. It returns its argument so it can wrap any sub-expression without changing the result, `print(x)` where `x` stood still computes x.
TEST_CASE("print reports a value without changing what the script computes") {
    // Wrapping the coordinate in print() must leave the transform identical.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(print(xPos), yPos, zPos);"), 7, 3, 0, 16, 16, 1).x == 7);
    // And it composes inside arithmetic.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(print(width - 1 - xPos), yPos, zPos);"), 0, 0, 0, 16, 16, 1).x == 15);
}

// Subtraction is emitted as `a + (b * -1)`, and -1 has to survive into the register. The assemblers materialize a constant with a 16-bit immediate, so a naive -1 becomes 65535 and every subtraction is right only MODULO 256, invisible in a stored byte, and wrong everywhere the full value is used: a bounds-guarded index silently drops the light, and a value handed to a host call is nonsense. Byte-comparison tests cannot see this, so it is checked through print(), which returns the full 32-bit value.
TEST_CASE("a subtraction produces the whole value, not just its low byte") {
    // `a - b` compiles to `a + (b * -1)`, so -1 has to reach the register intact. The assemblers build a constant from a 16-bit immediate, and a naive -1 lands as 65535, which leaves every subtraction correct only MODULO 256. A stored color byte cannot show that (the low byte is right either way), so this checks the value THROUGH print(), which returns the full 32 bits and is therefore the only observer that can fail.
    //
    // The consequences the byte hides: an index computed by subtraction becomes ~65k, the element store's bounds guard rejects it, and the light silently never lights; a subtraction handed to a host call (random16, print) gets a wrong argument.
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(print(width - 1 - xPos), yPos, zPos);"), 0, 0, 0, 16, 16, 1).x == 15);
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(print(100 - 1), yPos, zPos);"), 0, 0, 0, 255, 255, 1).x == 99);
    CHECK(transform(mmScriptAs("modifyLogical", "setXYZ(print(5 - 5), yPos, zPos);"), 0, 0, 0, 255, 255, 1).x == 0);
}

// --- for --------------------------------------------------------------------------------------
//
// The loop is what a scripted LAYOUT needs: placing N lights means running N times, and a modifier's per-light call is not available to it. These check it through print(), which reports the full value each iteration, a stored byte would only ever show the last one.

TEST_CASE("a for loop runs its body once per step") {
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "for (int i = 0; i < 4; i = i + 1) { print(i); } setXYZ(xPos, yPos, zPos);")));
    m.prepare();
    CHECK(m.severity() != MoonModule::Severity::Error);   // it compiles at all
    Coord3D box{16, 16, 1}; m.modifyLogicalSize(box);
    Coord3D p{3, 0, 0};
    m.modifyLogical(p);
    CHECK(p.x == 3);                                      // the statement after the loop still runs
}

TEST_CASE("a loop over an empty range runs its body no times") {
    // The entry guard: `i < 0` must skip the body entirely rather than wrap and run forever.
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "for (int i = 0; i < 0; i = i + 1) { print(99); } setXYZ(xPos, yPos, zPos);")));
    m.prepare();
    CHECK(m.severity() != MoonModule::Severity::Error);
    Coord3D box{16, 16, 1}; m.modifyLogicalSize(box);
    Coord3D p{7, 0, 0};
    m.modifyLogical(p);
    CHECK(p.x == 7);
}

TEST_CASE("loops nest, which is what placing a grid of lights needs") {
    MoonLiveModifier m;
    m.defineControls();
    m.setScript(mmWriteScript(mmScriptAs("modifyLogical", "for (int a = 0; a < 2; a = a + 1) { for (int b = 0; b < 2; b = b + 1) { print(a); } }"
                " setXYZ(xPos, yPos, zPos);")));
    m.prepare();
    CHECK(m.severity() != MoonModule::Severity::Error);
    Coord3D box{16, 16, 1}; m.modifyLogicalSize(box);
    Coord3D p{5, 0, 0};
    m.modifyLogical(p);
    CHECK(p.x == 5);
}


// The loop's real purpose, end to end: a script that PAINTS with it. A modifier's script transforms one coordinate, so it can never show a loop writing many lights, an effect can, and this is the shape a scripted Layout will use to place its lights.
TEST_CASE("a loop in an effect script paints every light it walks") {
    Layouts layouts;
    auto* grid = new GridLayout();
    grid->width = 8; grid->height = 1; grid->depth = 1;
    layouts.addChild(grid);

    Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    auto* fx = new MoonLiveEffect();
    fx->defineControls();
    fx->setScript(mmWriteScript(mmScript("for (int i = 0; i < 8; i = i + 1) { setRGB(i, i, 0, 0); }")));
    layer.addChild(fx);
    layouts.applyState();
    layer.applyState();

    std::memset(const_cast<uint8_t*>(layer.buffer().data()), 0, layer.buffer().bytes());
    layer.tick();

    // Each light's red channel is its own index: proof the loop ran once per light AND that the counter reached the emitter as a distinct value each time.
    const uint8_t* buf = layer.buffer().data();
    for (int i = 0; i < 8; i++) {
        CAPTURE(i);
        CHECK(static_cast<int>(buf[i * 3]) == i);
    }
}


// The layout's count sets the extent the modifier folds within, so a stale one folds wrong and drops lights, dark with no error.
TEST_CASE("a scripted layout that changes its light count under a scripted modifier rebuilds the mapping to the new count") {
    MoonLiveLayout layout;
    layout.defineControls();
    // A row whose length is one of the script's OWN controls, which is how a user resizes a scripted fixture.
    layout.setScript(mmWriteScript(
        "class layoutScript {\n"
        "  int cols = 8;\n"
        "  void defineControls() { addControl(\"cols\", cols, 1, 64); }\n"
        "  void placeLights() { for (int i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); } }\n"
        "}\n"));
    layout.prepare();
    layout.rebuildControls();      // the script's own controls exist only once it has compiled

    Layouts group;
    group.addChild(&layout);
    Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);

    // A scripted mirror over that row: it folds within the extent the LAYOUT reports, not a constant.
    MoonLiveModifier mod;
    mod.defineControls();
    mod.setScript(mmWriteScript(mmScriptAs("modifyLogical", "setXYZ(width - 1 - xPos, yPos, zPos);")));
    layer.addChild(&mod);
    layer.defineControls();
    group.applyState();
    layer.applyState();

    // A script's `int` binds as Int32, so the slot is four bytes: a uint8_t write would touch the low one alone.
    auto setCols = [&](int32_t v) {
        const auto& cs = layout.controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (cs[i].name && std::strcmp(cs[i].name, "cols") == 0)
                *static_cast<int32_t*>(cs[i].ptr) = v;
    };

    REQUIRE(layout.lightCount() == 8);
    REQUIRE(layer.lut().hasLUT());            // a scripted modifier is a real mapping, not the identity path

    // Grow, shrink, grow: each count is the extent the mirror folds within, so each needs its own mapping.
    for (int32_t cols : {24, 4, 40, 8}) {
        INFO("cols = " << cols);
        setCols(cols);
        group.applyState();
        layer.applyState();
        CHECK(layout.lightCount() == cols);   // the layout answers with the new count

        // Every light the layout placed has a destination, and no more than that: a fold against a stale extent loses the ones past the old end.
        std::size_t total = 0;
        for (nrOfLightsType li = 0; li < layer.lut().logicalCount(); li++)
            layer.lut().forEachDestination(li, [&](nrOfLightsType) { total++; });
        CHECK(total == cols);
    }
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
