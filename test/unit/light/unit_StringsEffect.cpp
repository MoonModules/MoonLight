/// @module StringsEffect

/// A stringed instrument laid out as the real one: the body and its bridge at the left, the fingerboard with frets closing in toward the body, the head at the right naming each open string, and each sounding note lighting its string where a finger stops it.

#include "doctest.h"
#include <cmath>
#include "audio_effect_test.h"
#include "light/effects/StringsEffect.h"

using namespace mm;

namespace {

/// The instrument on a `w` by `h` grid.
struct Strings : EffectOnGrid<StringsEffect> {
    explicit Strings(int w = 64, int h = 32) : EffectOnGrid(w, h) {}
};

// Geometry on a 64x32 grid, mirroring the effect: frame 2, body and head 14 wide, bridge 6, struck at 11, the last fret at 16, nut 47, board rows 4..22, numbers at 24.
int frameOf(int w) { return w / 32 > 2 ? w / 32 : 2; }
int bodyWOf(int w) { int lw = 14; int wide = lw > w / 6 ? lw : w / 6; int fit = (w - 1 - 2 * frameOf(w) - 12) / 2; return std::max(0, std::min(wide, fit)); }   // the strips give way to a 12-column board
int endXOf(int w) { return frameOf(w) + bodyWOf(w); }      // the fingerboard's body end, fret 12
int nutXOf(int w) { return w - 1 - frameOf(w) - bodyWOf(w); }   // the nut at the head, the head as wide as the body
int bridgeXOf(int w) { return frameOf(w) + std::max(1, bodyWOf(w) / 3); }
int pickXOf(int w) { return (bridgeXOf(w) + endXOf(w)) / 2; }   // where the strings are struck or bowed
int stringY(int s, int h, int n) { int top = frameOf(64) + 2, bot = h - 1 - frameOf(64) - 7; return top + (2 * s + 1) * (bot - top) / (2 * n); }
constexpr long kStop[25] = {65536, 61858, 58386, 55109, 52016, 49097, 46341, 43740, 41285, 38968, 36781, 34716, 32768,
                            30929, 29193, 27554, 26008, 24548, 23170, 21870, 20643, 19484, 18390, 17358, 16384};   // 2^(-f/12) in 65536ths
// Fret `fret` on a board of `frets` frets, 19 for the guitar: 1 - 2^(-f/12) of the string from the nut, the last fret at the body.
int fretX(int fret, int w, int frets = 19) { return nutXOf(w) - static_cast<int>((nutXOf(w) - endXOf(w)) * (65536 - kStop[fret]) / (65536 - kStop[frets])); }
int stopX(int fret, int w, int frets = 19) { return fret == 0 ? nutXOf(w) : (fretX(fret - 1, w, frets) + fretX(fret, w, frets)) / 2; }   // the middle of the space behind the fret
constexpr int kGold = 230 + 190 + 90, kBoard = 105 + 62 + 24, kBody = 135 + 82 + 30, kMetal = 610;

}  // namespace

TEST_CASE("the instrument rests as the real one: body and bridge at the left, frets closing in toward the body, the nut and the names at the right") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, AudioFrame{}, 1000);
    CHECK(s.at(0, 0) == 0);                                // rounded corner: dark outside the arc
    CHECK(s.at(9, 0) == 200 + 120 + 45);   // the corner arc's rim stays wood
    CHECK(s.at(7, 0) == 0);                // outside the arc is dark
    CHECK(s.at(30, 2) == kBoard);                           // the fingerboard over the frets
    CHECK(s.at(4, 12) == kBody);                            // the body's wood at the left
    CHECK(s.at(36, stringY(0, 32, 6)) == kGold);            // a gold string
    CHECK(s.at(36, stringY(0, 32, 6) + 1) != kGold);        // the guitar's strings are one pixel thick
    CHECK(fretX(1, 64) - fretX(2, 64) > fretX(11, 64) - fretX(12, 64));   // the spaces narrow toward the body
    CHECK(s.at(fretX(6, 64), 10) == kMetal);                // a metal fret off the strings
    CHECK(s.at(fretX(6, 64), stringY(0, 32, 6)) == kGold);  // the string runs over the fret
    CHECK(s.at(stopX(3, 64), 13) == 225 + 220 + 200);       // an inlay dot in space 3, mid-board
    CHECK(s.at(nutXOf(64), 10) == 710);                     // the nut at the right
    CHECK(s.at(bridgeXOf(64) + 1, 10) == 235 + 230 + 210);  // the bridge's bone saddle at the left
    CHECK(s.at(bridgeXOf(64), 10) == 70 + 35 + 15);         // on its rosewood
    CHECK(s.maxIn(stopX(3, 64) - 2, stopX(3, 64) + 3, 24, 30) == 225 + 220 + 200);   // the space number "3" along the bottom
    CHECK(s.maxIn(nutXOf(64) + 2, 61, stringY(0, 32, 6) - 3, stringY(0, 32, 6) + 3) == 3 * 255);   // the open note named on the head
    audio.release();
}

TEST_CASE("under notes all, a sounding note lights every string and fret that plays it") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.notes = AudioNotesBase::kNotesAll;
    play(s, audio, note(57, 200), 1000);   // A3: string 1 (A2) fret 12, string 3 (G3) fret 2
    const RGB want = expectedColor(57 - 21, 200);
    CHECK(s.ch(stopX(12, 64), stringY(1, 32, 6), 0) == want.r);
    CHECK(s.at(stopX(2, 64), stringY(3, 32, 6)) == want.r + want.g + want.b);
    CHECK(s.at(fretX(10, 64), stringY(0, 32, 6) - 2) == kMetal);   // the low E does not play A3: fret metal, no dot
    audio.release();
}

// `all` is the raw view: a key heard for a single frame shows, where the other levels wait for it to outlast an attack.
TEST_CASE("under notes all a weak one-frame strike shows, under no overtones it waits for a second frame") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings raw;
    raw.e.notes = AudioNotesBase::kNotesAll;
    play(raw, audio, note(45, 10, true), 1000);   // A2, weak, one frame: string 1 open, at the nut
    CHECK(raw.at(nutXOf(64), stringY(1, 32, 6)) == [] { const RGB c = expectedColor(45 - 21, 255); return c.r + c.g + c.b; }());
    Strings sieved;
    sieved.e.notes = AudioNotesBase::kNotesNoOvertones;
    play(sieved, audio, note(45, 10, true), 1000);
    CHECK(sieved.at(nutXOf(64), stringY(1, 32, 6)) != raw.at(nutXOf(64), stringY(1, 32, 6)));
    audio.release();
}

// Under hands the fretting hand plays a note at one place: A3 within the first position, string 3 at fret 2, not also string 1 at fret 12.
TEST_CASE("under notes hands, a sounding note lights at the one place the hand plays it") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, note(57, 200), 1000);
    const RGB want = expectedColor(57 - 21, 200);
    CHECK(s.at(stopX(2, 64), stringY(3, 32, 6)) == want.r + want.g + want.b);
    CHECK(s.ch(stopX(12, 64), stringY(1, 32, 6), 0) != want.r);
    audio.release();
}

TEST_CASE("an open string lights at the nut, and a struck note flashes at full glow then fades") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.decay = 30;   // pin the short glow this test fades past
    play(s, audio, note(45, 200, true), 1000);   // A2 struck: string 1 open, at the nut
    const RGB want = expectedColor(45 - 21, 255);
    CHECK(s.at(nutXOf(64), stringY(1, 32, 6)) == want.r + want.g + want.b);
    CHECK(s.at(nutXOf(64) - 1, stringY(1, 32, 6)) == want.r + want.g + want.b);   // the dot's edge over the string
    platform::setTestNowMs(1400);   // past the 300 ms glow
    s.layer.tick();
    CHECK(s.at(nutXOf(64) - 1, stringY(1, 32, 6)) == kGold);   // faded back to the resting string
    audio.release();
}

TEST_CASE("the instrument select changes strings, tuning, bridge and body: bass has four thick strings, a pickup, E A D G") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.instrument = StringsEffect::kBass;
    play(s, audio, note(33, 200), 1000);   // A1: bass string 1 open, at the nut
    const RGB want = expectedColor(33 - 21, 200);
    CHECK(s.at(nutXOf(64), stringY(1, 32, 4)) == want.r + want.g + want.b);
    CHECK(s.at(fretX(1, 64, 20), stringY(0, 32, 4) - 2) == kMetal);   // the low E does not play A1, on the bass's 20 frets
    CHECK(s.at(36, stringY(0, 32, 4)) == kGold);       // a gold string
    CHECK(s.at(36, stringY(0, 32, 4) - 1) == kGold);   // two pixels thick on bass
    CHECK(s.at(bridgeXOf(64) + 1, stringY(0, 32, 4) - 2) == 235 + 230 + 210);   // the saddle reaches past the outer strings
    CHECK(s.at(pickXOf(64) - 1, stringY(0, 32, 4) - 2) == 75);   // the black pickup under the strings where they are plucked
    audio.release();
}

TEST_CASE("the guitar's body has a dark sound hole with its rosette where the strings are strummed") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, AudioFrame{}, 1000);
    const int cy = (stringY(0, 32, 6) + stringY(5, 32, 6)) / 2;   // 12, between the strings
    CHECK(s.at(pickXOf(64), cy - 3) == 215 + 150 + 80);   // the rosette, radius 3, clear of the saddle
    CHECK(s.at(pickXOf(64) + 1, cy + 1) == 0);           // the hole, dark between the strings
    CHECK(s.at(pickXOf(64), stringY(2, 32, 6)) == kGold);   // the strings run over it
    audio.release();
}

// On a 128 grid the body is 21 wide: the bridge at 11, the hole at 18, the head from 104.
TEST_CASE("the body and the head scale on a 128 grid") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s(128, 128);
    play(s, audio, AudioFrame{}, 1000);
    CHECK(s.at(18, 61) == 0);             // the sound hole, mid strings
    CHECK(s.at(6, 63) == kBody);          // the body's wood left of the bridge
    CHECK(s.at(112, 63) == kBoard);       // the head between two names
    audio.release();
}

// A note dot shrinks with its level, so the dim tail of the glow is a small dot rather than a big black one.
TEST_CASE("a note dot shrinks as its glow decays") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s(128, 128);
    s.e.decay = 30;
    play(s, audio, note(45, 200, true), 1000);   // A2 struck: the open-string dot at the nut, (102, 33), radius 2, as wide as the first fret's space allows
    const RGB full = expectedColor(45 - 21, 255);
    CHECK(s.at(102, 35) == full.r + full.g + full.b);   // full glow: the dot reaches 2 px out
    platform::setTestNowMs(1160);   // 160 ms into the 300 ms glow (ticks are 4 ms): 183 of 255 left, the dot shrinks to 1 px
    s.layer.tick();
    const RGB faded = expectedColor(45 - 21, 183);
    CHECK(s.at(102, 33) == faded.r + faded.g + faded.b);   // the center still shows the faded color
    CHECK(s.at(102, 35) != full.r + full.g + full.b);      // the edge pixel is gone
    audio.release();
}

// The render loop runs faster than the audio blocks: a frame two ticks read is one hit.
TEST_CASE("a frame two ticks read is one hit: the glow fades on instead of starting again") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    audio.frameForTest() = note(45, 200, true);
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

// The default glow lasts a second (decay 100), so a struck note stays colored instead of flashing out.
TEST_CASE("the default glow lasts a second") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    play(s, audio, note(45, 200, true), 1000);   // A2 struck: string 1 open, at the nut
    platform::setTestNowMs(1500);   // 500 ms into the 1000 ms glow: 192 of 255 left on the quadratic fade
    s.layer.tick();
    const RGB half = expectedColor(45 - 21, 192);
    CHECK(s.at(nutXOf(64), stringY(1, 32, 6)) == half.r + half.g + half.b);
    audio.release();
}

TEST_CASE("noteNames writes the strongest pitch classes top left, and nothing when off") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    f.chroma[0] = 200; f.chroma[4] = 180; f.chroma[7] = 220;   // C E G
    f.key = 1;
    f.tuning = 12;
    play(s, audio, f, 1000);
    const std::vector<int> cols = s.whiteColumnsIn(0, 45, 0, 5);   // left of the names on the head
    REQUIRE(!cols.empty());
    CHECK(cols.front() < 10);
    s.e.noteNames = false;
    play(s, audio, f, 2000);
    CHECK(s.whiteColumnsIn(0, 45, 0, 5).empty());
    audio.release();
}

TEST_CASE("violin gets thin dark pseudo-frets, a maple bridge and a tailpiece") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.instrument = StringsEffect::kViolin;
    play(s, audio, AudioFrame{}, 1000);
    CHECK(s.at(fretX(6, 64, 24), 8) == 120 + 80 + 40);      // a 1px pseudo-fret, on the violin's two octaves
    CHECK(s.at(fretX(6, 64, 24) + 1, 8) != 120 + 80 + 40);   // only one pixel wide
    CHECK(s.at(bridgeXOf(64), 8) == 225 + 185 + 120);        // a thin maple bridge, between two strings
    CHECK(s.at(frameOf(64) + 1, stringY(0, 32, 4)) == 25 + 18 + 12);   // the strings end in the ebony tailpiece
    audio.release();
}

// A 32-wide panel is a common HUB75 size: the side strips give way so the fingerboard keeps a column a fret.
TEST_CASE("on a narrow grid the strips give way to the fingerboard") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s(32, 32);
    play(s, audio, AudioFrame{}, 1000);
    REQUIRE(nutXOf(32) - endXOf(32) >= 12);                 // a column a fret
    CHECK(s.at(fretX(2, 32), 10) == kMetal);                 // a metal fret off the strings, the first left out as it would touch the nut
    CHECK(s.at(fretX(2, 32), stringY(0, 32, 6)) == kGold);   // the string runs over it
    audio.release();
}

// Too small for any fingerboard, the fret math would run negative through unsigned arithmetic and scatter frets and dots.
TEST_CASE("a grid too small for a fingerboard draws the enclosure alone") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    struct Size { int w, h; };
    for (const Size sz : {Size{5, 32}, Size{64, 8}}) {   // too narrow; too low
        CAPTURE(sz.w);
        Strings s(sz.w, sz.h);
        play(s, audio, allNotes(), 1000);
        int metal = 0, dots = 0;
        for (int y = 0; y < sz.h; y++)
            for (int x = 0; x < sz.w; x++) {
                if (s.at(x, y) == 610) metal++;
                if (s.at(x, y) == 225 + 220 + 200) dots++;
            }
        CHECK(metal == 0);
        CHECK(dots == 0);
    }
    audio.release();
}

TEST_CASE("the instrument survives a 1x1 and a 0x0 grid with every note sounding") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    survivesTinyGrids<Strings>(audio);
    audio.release();
}

// Under hands with chord, a strummed C major lights the open C shape, x32010, instead of the notes one by one.
TEST_CASE("under hands with chord a C major lights the open C shape") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.chord = true;
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    for (uint8_t k : {27, 31, 34, 39, 43}) f.notes[k] = 200;   // C3 E3 G3 C4 E4
    f.levelSmoothed = 200;
    f.chordRoot = 0;
    f.chordQuality = kChordMajor;
    play(s, audio, f, 1000);
    CHECK(s.e.fretForTest(0) == -1);
    CHECK(s.e.fretForTest(1) == 3);
    CHECK(s.e.fretForTest(2) == 2);
    CHECK(s.e.fretForTest(3) == 0);
    CHECK(s.e.fretForTest(4) == 1);
    CHECK(s.e.fretForTest(5) == 0);
    audio.release();
}

// A real guitar's neck runs past the twelfth fret: a note at fret 17 has its place, in a space narrower than the first.
TEST_CASE("the guitar's board runs to fret 19, so a solo's high notes have their place") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s(128, 64);
    s.e.notes = AudioNotesBase::kNotesAll;
    play(s, audio, note(81, 200), 1000);   // A5: the high E string at fret 17
    const RGB want = expectedColor(81 - 21, 200);
    const int nut = 128 - 1 - 4 - 21, end = 4 + 21;
    auto fx = [&](int f) { return nut - static_cast<int>((nut - end) * (65536L - kStop[f]) / (65536L - kStop[19])); };
    const int top = 4 + 2, bot = 64 - 1 - 4 - 7;
    CHECK(s.at((fx(16) + fx(17)) / 2, top + 11 * (bot - top) / 12) == want.r + want.g + want.b);
    CHECK(fx(16) - fx(17) < fx(0) - fx(1));   // the spaces narrow toward the body
    audio.release();
}

// A solo over a named chord keeps its notes: the chord's shape takes over only once three strings or more sound.
TEST_CASE("under hands with chord a single note keeps its place, and a chord of three strings or more takes the shape") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.chord = true;
    AudioFrame f = note(69, 200, true);   // A4 alone
    f.levelSmoothed = 200;
    f.chordRoot = 0;
    f.chordQuality = kChordMajor;         // C named by the band
    play(s, audio, f, 1000);
    int strings = 0;
    for (uint8_t str = 0; str < 6; str++) strings += s.e.fretForTest(str) >= 0;
    CHECK(strings == 1);                  // the A4 alone, not the five-string C shape
    audio.release();
}

// Frets are thin wires: one pixel each, and where they crowd toward the body on a small grid, one that would touch the last drawn is left out rather than merging into a block of metal.
TEST_CASE("frets are one pixel wide, and none touches the next") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    for (const int w : {64, 128}) {
        CAPTURE(w);
        Strings s(w, 32);
        play(s, audio, AudioFrame{}, 1000);
        int metal = 0, touching = 0;
        for (int x = endXOf(w); x < nutXOf(w); x++) {
            const bool here = s.at(x, 10) == kMetal, next = s.at(x + 1, 10) == kMetal;
            metal += here;
            touching += here && next;
        }
        CHECK(touching == 0);
        CHECK(metal >= 10);   // most of the 19 frets still show
        if (w == 128) CHECK(metal == 19);   // on a wide grid every fret has room
    }
    audio.release();
}

TEST_CASE("under hands a picked string lights cyan from the bridge to its fret, the length that vibrates, and fades back to gold") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.decay = 30;                               // a 300 ms glow
    play(s, audio, note(57, 200, true), 1000);    // A3 picked: string 3 at fret 2
    const int y = stringY(3, 32, 6);
    CHECK(s.ch(30, y, 2) > s.ch(30, y, 0));       // cyan between the bridge and the fret
    CHECK(s.at(fretX(1, 64) + 1, y) == kGold);    // past the stopping finger, toward the nut, the string rests
    CHECK(s.at(30, stringY(4, 32, 6)) == kGold);  // an unpicked string stays gold
    platform::setTestNowMs(1400);                 // past the glow
    s.layer.tick();
    CHECK(s.at(30, y) == kGold);
    audio.release();
}

TEST_CASE("under hands the violin's bow crosses the strings by the bridge, and a bowed string lights") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings s;
    s.e.instrument = StringsEffect::kViolin;
    const int hair = pickXOf(64);
    const int y = stringY(2, 32, 4);              // the A string, A4 open
    play(s, audio, AudioFrame{}, 1000);
    CHECK(s.at(hair, y) == 3 * 255);              // the white hair over the string
    for (int r = 8; r <= 16; r++) CHECK(s.ch(hair - 2, r, 0) == 255);   // the red stick, one straight line
    CHECK(s.ch(hair - 2, 12, 2) == 0);
    CHECK(s.at(hair - 1, 12) != s.at(hair - 2, 12));   // apart from the hair
    CHECK(s.at(30, y) == kGold);
    for (uint32_t t = 1000; t <= 1350; t += 50) play(s, audio, note(69, 200, t == 1000), t);   // struck, then held: bowed
    CHECK(s.ch(30, y, 2) > s.ch(30, y, 0));       // the bowed string lights cyan
    audio.release();
}

// A player changes bow on a new note and draws it faster for a louder one; between bowed notes the bow waits.
TEST_CASE("the bow strokes while a string is bowed, faster the louder, turning at each new note") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto bowed = [&](uint8_t lvl) {               // the bow's travel over 200 ms of a held A4 at `lvl`
        Strings s;
        s.e.instrument = StringsEffect::kViolin;
        for (uint32_t t = 1000; t <= 1300; t += 20) play(s, audio, note(69, lvl, t == 1000), t);
        const float from = s.e.bowForTest();
        for (uint32_t t = 1320; t <= 1500; t += 20) play(s, audio, note(69, lvl), t);
        return std::fabs(s.e.bowForTest() - from);
    };
    CHECK(bowed(250) > 1.5f * bowed(80));         // louder, faster
    CHECK(bowed(80) != 0.0f);

    Strings s;
    s.e.instrument = StringsEffect::kViolin;
    const float rest = s.e.bowForTest();
    for (uint32_t t = 1000; t <= 1200; t += 20) play(s, audio, AudioFrame{}, t);
    CHECK(s.e.bowForTest() == rest);              // nothing bowed: the bow waits
    for (uint32_t t = 1200; t <= 1600; t += 20) play(s, audio, note(69, 200, t == 1200), t);
    const float down = s.e.bowForTest();
    CHECK(down != rest);
    AudioFrame both = note(69, 200);              // the A held, the E struck beside it: a new stroke
    both.notes[76 - 21] = 200;
    both.notesHits[(76 - 21) >> 3] = static_cast<uint8_t>(1u << ((76 - 21) & 7));
    play(s, audio, both, 1620);
    AudioFrame held = note(69, 200);
    held.notes[76 - 21] = 200;
    for (uint32_t t = 1640; t <= 1800; t += 20) play(s, audio, held, t);
    CHECK((s.e.bowForTest() - down) * (down - rest) < 0.0f);   // the bow turned
    audio.release();
}

// The bow is as long as the instrument is high and slides along its length: it turns before either end passes an outer string, so a lit string is always under the hair.
TEST_CASE("every string stays under the bow's hair however long it strokes") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    for (const uint8_t instrument : {StringsEffect::kViolin, StringsEffect::kCello}) {
        CAPTURE(instrument);
        Strings s;
        s.e.instrument = instrument;
        const uint8_t open = instrument == StringsEffect::kViolin ? 69 : 50;   // the third string, open
        for (uint32_t t = 1000; t <= 6000; t += 20) {
            play(s, audio, note(open, 250, t == 1000 || t % 1500 == 0), t);   // a loud held note, a new stroke now and then
            for (int str = 0; str < 4; str++) REQUIRE(s.at(pickXOf(64), stringY(str, 32, 4)) == 3 * 255);
        }
    }
    audio.release();
}

TEST_CASE("the bow shows only on violin and cello under hands") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Strings guitar;
    play(guitar, audio, AudioFrame{}, 1000);
    Strings violinAll;
    violinAll.e.instrument = StringsEffect::kViolin;
    violinAll.e.notes = AudioNotesBase::kNotesAll;
    play(violinAll, audio, AudioFrame{}, 1000);
    int white = 0;
    for (int y = 0; y < 32; y++) white += (guitar.at(pickXOf(64), y) == 3 * 255) + (violinAll.at(pickXOf(64), y) == 3 * 255);
    CHECK(white == 0);                            // no hair
    audio.release();
}
