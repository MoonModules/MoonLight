/// @module MoonLive
/// @also MoonLiveEffect

/// `RGB` and `Coord3D`, conditions joined by `&&`, `||` and `!`, and the arithmetic builtins, each run on this host's backend and read back from the lights.

#include "doctest.h"
#include "MoonLiveEffectScene.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <vector>

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
/// One frame of a script on a 16x16 grid, and the lights it wrote; a script that does not compile fails here with its error.
const uint8_t* frame(ScriptScene& s, const char* script) {
    std::vector<uint8_t> code(moonlive::codeCapFor(moonlive::countTokens(script)));
    const auto r = moonlive::compileSource(script, moonlive::lightBuiltins(), moonlive::effectSysVars(), code.data(), code.size());
    INFO(std::string(r.error), " at ", r.errorCol);
    REQUIRE(r.ok);
    s.run(script);
    s.layer.tick();
    return s.layer.buffer().data();
}
}  // namespace

TEST_CASE("an RGB value is made, changed and drawn as one color") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  RGB keep;\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    RGB c = RGB(10, 20, 30);\n"
        "    setRGB(0, c);\n"                       // a struct argument fills three positions
        "    c.g = 99;\n"
        "    setRGB(1, c.r, c.g, c.b);\n"
        "    keep = c;\n"                           // whole-struct assignment into a member
        "    keep.b = keep.b + 1;\n"
        "    setRGB(2, keep);\n"
        "    RGB z;\n"                              // a local declared without a value starts at zero
        "    setRGB(3, z);\n"
        "  }\n"
        "}\n");
    CHECK(px[0] == 10);  CHECK(px[1] == 20); CHECK(px[2] == 30);
    CHECK(px[3] == 10);  CHECK(px[4] == 99); CHECK(px[5] == 30);
    CHECK(px[6] == 10);  CHECK(px[7] == 99); CHECK(px[8] == 31);
    CHECK(px[9] == 0);   CHECK(px[10] == 0); CHECK(px[11] == 0);
}

TEST_CASE("an RGB member keeps its value from one frame to the next") {
    ScriptScene s;
    s.run("class T {\n"
          "  RGB acc;\n"
          "  void tick() { acc.r = acc.r + 10; acc.b = 5; setRGB(0, acc); }\n"
          "}\n");
    for (int i = 0; i < 3; i++) s.layer.tick();
    const uint8_t* px = s.layer.buffer().data();
    CHECK(px[0] == 30);   // starts at zero, plus ten per frame
    CHECK(px[1] == 0);
    CHECK(px[2] == 5);
}

TEST_CASE("a Coord3D holds negative and large whole numbers") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  Coord3D far;\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    Coord3D p = Coord3D(-5, 7, -300);\n"
        "    far = p;\n"
        "    far.x = far.x * 100;\n"
        "    setRGB(0, abs(p.x), p.y, 0);\n"
        "    if (p.z < -200) { setRGB(1, 1, 2, 3); }\n"
        "    if (far.x == -500) { setRGB(2, 4, 5, 6); }\n"
        "  }\n"
        "}\n");
    CHECK(px[0] == 5); CHECK(px[1] == 7);
    CHECK(px[3] == 1); CHECK(px[4] == 2); CHECK(px[5] == 3);
    CHECK(px[6] == 4); CHECK(px[7] == 5); CHECK(px[8] == 6);
}

TEST_CASE("a light's color and a palette color read back as one RGB") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    setRGB(0, 10, 20, 30);\n"
        "    setRGB(1, getRGB(0));\n"                  // a struct answer passed straight on
        "    setRGB(2, getRGB(0).g, 0, 0);\n"          // one field of it
        "    RGB c = paletteRGB(100, 200);\n"
        "    setRGB(3, c);\n"
        "    RGB past = getRGB(9999);\n"               // past the end reads as black
        "    setRGB(5, past);\n"
        "  }\n"
        "}\n");
    CHECK(px[3] == 10); CHECK(px[4] == 20); CHECK(px[5] == 30);
    CHECK(px[6] == 20);
    const RGB want = colorFromPalette(*Palettes::active(), 100, 200);              // what a compiled effect reads
    CHECK(px[9] == want.r); CHECK(px[10] == want.g); CHECK(px[11] == want.b);
    CHECK(px[15] == 0); CHECK(px[16] == 0); CHECK(px[17] == 0);
}

TEST_CASE("a script function takes and answers a struct") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  RGB warm(int k) { return RGB(k, k / 2, 10); }\n"
        "  void paint(int i, RGB c, int boost) { setRGB(i, c.r, c.g + boost, c.b); }\n"
        "  Coord3D move(Coord3D p, Coord3D d) { return Coord3D(p.x + d.x, p.y + d.y, p.z + d.z); }\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    paint(0, warm(200), 1);\n"
        "    Coord3D q = move(Coord3D(1, 2, 3), Coord3D(10, 20, 30));\n"
        "    setRGB(1, q.x, q.y, q.z);\n"
        "    setRGB(2, warm(60).g, 0, 0);\n"
        "  }\n"
        "}\n");
    CHECK(px[0] == 200); CHECK(px[1] == 101); CHECK(px[2] == 10);
    CHECK(px[3] == 11);  CHECK(px[4] == 22);  CHECK(px[5] == 33);
    CHECK(px[6] == 30);
}

TEST_CASE("conditions join with && and ||, negate with !, and stop at the first answer") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  int hits = 0;\n"
        "  int touch() { hits = hits + 1; return 1; }\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    hits = 0;\n"
        "    if (1 < 2 && 2 < 3) { setRGB(0, 1, 0, 0); }\n"
        "    if (1 < 2 && 3 < 2) { setRGB(1, 1, 0, 0); }\n"
        "    if (3 < 2 || 1 < 2) { setRGB(2, 1, 0, 0); }\n"
        "    if (!(3 < 2)) { setRGB(3, 1, 0, 0); }\n"
        "    if (0 || 1 && 1) { setRGB(4, 1, 0, 0); }\n"       // && binds tighter than ||
        "    if ((0 || 1) && 0) { setRGB(5, 1, 0, 0); }\n"     // parentheses group a condition
        "    if (0 && touch()) { }\n"                          // never asked
        "    if (1 || touch()) { }\n"                          // never asked
        "    if (touch() && touch()) { setRGB(6, hits, 0, 0); }\n"
        "    if (7) { setRGB(7, 1, 0, 0); }\n"                 // a bare value is true when it is not zero
        "    if (!0 && !touch() || 1) { setRGB(8, hits, 0, 0); }\n"
        "  }\n"
        "}\n");
    CHECK(px[0] == 1);
    CHECK(px[3] == 0);
    CHECK(px[6] == 1);
    CHECK(px[9] == 1);
    CHECK(px[12] == 1);
    CHECK(px[15] == 0);
    CHECK(px[18] == 2);   // only the last condition's two calls ran
    CHECK(px[21] == 1);
    CHECK(px[24] == 3);
}

TEST_CASE("abs, min, max and clamp work on whole and fixed values") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    setRGB(0, abs(-7), min(3, -2) + 10, max(-1, 4));\n"
        "    setRGB(1, clamp(300, 0, 255), clamp(-5, 0, 255), clamp(40, 0, 255));\n"
        "    fixed f = -1.5;\n"
        "    setRGB(2, toInt(abs(f) * toFixed(4)), toInt(max(f, toFixed(2))), toInt(min(f, toFixed(2)) + toFixed(3)));\n"
        "  }\n"
        "}\n");
    CHECK(px[0] == 7); CHECK(px[1] == 8);   CHECK(px[2] == 4);
    CHECK(px[3] == 255); CHECK(px[4] == 0); CHECK(px[5] == 40);
    CHECK(px[6] == 6); CHECK(px[7] == 2);   CHECK(px[8] == 1);   // the fixed results stay fixed: 6.0, 2.0 and 1.5
}

// A same-type call as another one's argument: the outer call's literal must still adopt the fixed type of the inner result, or max(1, 0.5) reads its 1 as 1/65536.
TEST_CASE("a same-type call nested in another keeps the outer call's literal in the right type") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    fixed f = 0.5;\n"
        "    fixed g = 0.25;\n"
        "    setRGB(0, toInt(max(1, min(f, g)) * toFixed(10)), toInt(clamp(min(f, g), 0, 2) * toFixed(100)), 0);\n"
        "  }\n"
        "}\n");
    CHECK(px[0] == 10);   // max(1.0, 0.25) * 10
    CHECK(px[1] == 25);   // clamp(0.25, 0.0, 2.0) * 100
}

// hpwit's parser freed a variable declared before a loop at the loop, so a read after it saw whatever reused the register.
TEST_CASE("a variable declared before a loop, changed in it, reads right after it") {
    ScriptScene s;
    const uint8_t* px = frame(s,
        "class T {\n"
        "  void tick() {\n"
        "    fill(0, 0, 0);\n"
        "    int k = 5;\n"
        "    RGB c = RGB(1, 2, 3);\n"
        "    for (int i = 0; i < 4; i = i + 1) { setRGB(i, k, c.g, 0); k = k + 1; c.g = c.g + 1; }\n"
        "    setRGB(10, k, c.g, c.b);\n"
        "  }\n"
        "}\n");
    for (int i = 0; i < 4; i++) { CHECK(px[i * 3] == 5 + i); CHECK(px[i * 3 + 1] == 2 + i); }
    CHECK(px[30] == 9); CHECK(px[31] == 6); CHECK(px[32] == 3);
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
