/// @module StringsEffect

/// A stringed instrument in its wooden enclosure across the grid: fingerboard with metal frets and position dots, one snare per string at the bridge with its open note named, a per-instrument emblem on the body, and each sounding note lighting its string at the fret it is played.

#include "doctest.h"
#include "audio_effect_test.h"
#include "light/effects/StringsEffect.h"

using namespace mm;

namespace {

/// The instrument on a `w` by `h` grid.
struct Strings : EffectOnGrid<StringsEffect> {
    explicit Strings(int w = 64, int h = 32) : EffectOnGrid(w, h) {}
};

// Geometry on a 64x32 grid, mirroring the effect: frame 2, nut 7, body 14 wide, bridge 47, board rows 4..22, numbers at 24.
int frameOf(int w) { return w / 32 > 2 ? w / 32 : 2; }
int nutXOf(int w) { return frameOf(w) + (w / 12 > 2 ? w / 12 : 2); }
int bridgeXOf(int w) { int lw = 14, bw = lw > w / 6 ? lw : w / 6; return w - 1 - frameOf(w) - bw; }
int stringY(int s, int h, int n) { int top = frameOf(64) + 2, bot = h - 1 - frameOf(64) - 7; return top + (2 * s + 1) * (bot - top) / (2 * n); }
int fretX(int fret, int w) { return nutXOf(w) + fret * (bridgeXOf(w) - nutXOf(w)) / 12; }

}  // namespace

TEST_CASE("the instrument rests in its wooden enclosure: frame, fingerboard, strings, metal frets, position dots, snares") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, AudioFrame{}, 1000);
    CHECK(s.at(0, 0) == 0);                                // rounded corner: dark outside the arc
    CHECK(s.at(7, 0) == 200 + 120 + 45);   // the bigger corner arc, up to the nut, stays wood
    CHECK(s.at(10, 2) == 105 + 62 + 24);                    // the fingerboard
    CHECK(s.at(35, stringY(0, 32, 6)) == 230 + 190 + 90);   // a gold string
    CHECK(s.at(35, stringY(0, 32, 6) + 1) != 230 + 190 + 90);   // the guitar's strings are one pixel thick
    CHECK(s.at(fretX(3, 64), 13) == 225 + 220 + 200);      // a position dot at fret 3, mid-board
    CHECK(s.at(fretX(6, 64), 10) == 610);   // a 2px metal fret off the strings
    CHECK(s.at(fretX(6, 64), stringY(0, 32, 6)) == 230 + 190 + 90);   // the string runs over the fret
    CHECK(s.at(fretX(6, 64) + 1, 10) == 610);
    CHECK(s.at(nutXOf(64), 10) == 710);                    // the nut
    CHECK(s.at(bridgeXOf(64), stringY(0, 32, 6)) == 610);  // a snare tick over the frets
    CHECK(s.at(bridgeXOf(64), stringY(0, 32, 6) + 2) == 610);
    CHECK(s.maxIn(14, 21, 24, 30) == 225 + 220 + 200);     // the fret number "3" along the bottom
    CHECK(s.maxIn(50, 58, stringY(0, 32, 6), stringY(0, 32, 6) + 6) == 3 * 255);   // the open note name, mid body strip
    audio.release();
}

TEST_CASE("a sounding note lights every string and fret that plays it") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, note(57, 200), 1000);   // A3: string 1 (A2) fret 12, string 3 (G3) fret 2
    const RGB want = expectedColor(57 - 21, 200);
    CHECK(s.ch(fretX(12, 64), stringY(1, 32, 6), 0) == want.r);
    CHECK(s.at(fretX(2, 64), stringY(3, 32, 6)) == want.r + want.g + want.b);
    CHECK(s.at(fretX(10, 64), stringY(0, 32, 6) - 2) == 610);   // the low E does not play A3: fret metal, no dot
    audio.release();
}

TEST_CASE("an open string lights at the nut, and a struck note flashes at full glow then fades") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, note(45, 0, true), 1000);   // A2 struck: string 1 open, at the nut
    const RGB want = expectedColor(45 - 21, 255);
    CHECK(s.at(nutXOf(64), stringY(1, 32, 6)) == want.r + want.g + want.b);
    CHECK(s.at(nutXOf(64) - 1, stringY(1, 32, 6)) == want.r + want.g + want.b);   // the dot's edge over the board
    platform::setTestNowMs(1400);   // past the 300 ms glow
    s.layer.tick();
    CHECK(s.at(nutXOf(64) - 1, stringY(1, 32, 6)) == 105 + 62 + 24);   // faded back to the fingerboard
    audio.release();
}

TEST_CASE("the instrument select changes strings, tuning, snares and emblem: bass has four strings, a pickup, E A D G") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.instrument = StringsEffect::kBass;
    play(s, audio, note(33, 200), 1000);   // A1: bass string 1 open, at the nut
    const RGB want = expectedColor(33 - 21, 200);
    CHECK(s.at(nutXOf(64), stringY(1, 32, 4)) == want.r + want.g + want.b);
    CHECK(s.at(fretX(10, 64), stringY(0, 32, 4) - 2) == 610);   // the low E does not play A1
    CHECK(s.at(35, stringY(0, 32, 4)) == 230 + 190 + 90);   // a gold string
    CHECK(s.at(35, stringY(0, 32, 4) - 1) == 230 + 190 + 90);   // two pixels thick on bass
    CHECK(s.at(bridgeXOf(64), stringY(0, 32, 4)) == 610);   // four snare ticks, one per string
    CHECK(s.at(bridgeXOf(64) + 14 / 2, 12) == 215 + 150 + 80);   // the pickup emblem on the body
    audio.release();
}

TEST_CASE("the guitar's body carries a round sound hole") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, AudioFrame{}, 1000);
    const int bx = bridgeXOf(64) + 14 / 2;
    CHECK(s.at(bx, 9) == 215 + 150 + 80);    // top of the sound-hole circle
    CHECK(s.at(bx, 13) == 135 + 82 + 30);     // inside the circle stays body wood
    audio.release();
}

// The render loop runs faster than the audio blocks: a frame two ticks read is one hit.
TEST_CASE("a frame two ticks read is one hit: the glow fades on instead of starting again") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    audio.frameForTest() = note(45, 0, true);
    audio.frameForTest().seq = 7;
    platform::setTestNowMs(1000);
    s.layer.tick();
    const RGB full = expectedColor(45 - 21, 255);
    const int first = s.at(nutXOf(64), stringY(1, 32, 6));
    CHECK(first == full.r + full.g + full.b);
    platform::setTestNowMs(1080);
    s.layer.tick();
    CHECK(s.at(nutXOf(64), stringY(1, 32, 6)) < first);
    CHECK(s.at(nutXOf(64), stringY(1, 32, 6)) > 0);
    audio.frameForTest() = AudioFrame{};
    audio.release();
}

TEST_CASE("noteNames writes the strongest pitch classes top left, and nothing when off") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    AudioFrame f;
    f.notesCount = 79;
    f.chroma[0] = 200; f.chroma[4] = 180; f.chroma[7] = 220;   // C E G
    f.key = 1;
    f.tuning = 12;
    play(s, audio, f, 1000);
    const std::vector<int> cols = s.whiteColumnsIn(0, 45, 0, 5);   // left of the snare labels
    REQUIRE(!cols.empty());
    CHECK(cols.front() < 10);
    s.e.noteNames = false;
    play(s, audio, f, 2000);
    CHECK(s.whiteColumnsIn(0, 45, 0, 5).empty());
    audio.release();
}

TEST_CASE("violin gets thin dark pseudo-frets instead of metal ones") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.instrument = StringsEffect::kViolin;
    play(s, audio, AudioFrame{}, 1000);
    CHECK(s.at(fretX(6, 64), 8) == 120 + 80 + 40);          // a 1px pseudo-fret
    CHECK(s.at(fretX(6, 64) + 1, 8) != 120 + 80 + 40);       // only one pixel wide
    CHECK(s.at(bridgeXOf(64), stringY(0, 32, 4)) == 610);    // snares still the fret color
    audio.release();
}

TEST_CASE("the instrument survives a 1x1 and a 0x0 grid with every note sounding") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    survivesTinyGrids<Strings>(audio);
    audio.release();
}
