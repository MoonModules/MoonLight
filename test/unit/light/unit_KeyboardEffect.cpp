/// @module KeyboardEffect

/// A piano keyboard in two halves: the top shows the selected octaves with pressed keys colored, the bottom the same ivory and ebony picture with a small circle per sounding octave on every key, plus note letters and the note names top left and right.

#include "doctest.h"
#include "audio_effect_test.h"
#include "light/effects/KeyboardEffect.h"

using namespace mm;

namespace {

/// The keyboard on a `w` by `h` grid, showing the whole piano unless a test says otherwise.
struct Keys : EffectOnGrid<KeyboardEffect> {
    explicit Keys(int w = 32, int h = 32) : EffectOnGrid(w, h) { e.octave = 0; e.octaves = 7; }
};

}  // namespace

// On a 32x32 grid the top half (rows 0..15) holds the colored keyboard, the bottom half the circle picture.
TEST_CASE("the keyboard rests as a piano in both halves, ivory bright and ebony dark, and dark with ivory at 0") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys;
    keys.e.noteNames = false;
    play(keys, audio, AudioFrame{}, 1000);
    // A4 is key 48, the 29th white key: column 19 on a 32-wide grid, ivory in both halves.
    CHECK(keys.ch(19, 10, 0) == keys.ch(19, 10, 1));
    CHECK(keys.ch(19, 10, 1) == keys.ch(19, 10, 2));
    CHECK(keys.at(19, 10) > 0);
    CHECK(keys.at(19, 31) > 0);                        // bottom half ivory too
    // C#4 is key 40, an ebony key over column 16 at the top half's top: dark, over the ivory below.
    CHECK(keys.ch(16, 0, 0) == keys.ch(16, 0, 1));
    CHECK(keys.at(16, 0) > 0);
    CHECK(keys.at(16, 0) < keys.at(16, 31));
    keys.e.ivory = 0;
    play(keys, audio, AudioFrame{}, 1020);
    CHECK(keys.maxIn(4, 32, 0, 32) == 0);   // the octave labels at the left stay white
    audio.release();
}

TEST_CASE("a struck key takes its palette color across the top half, then fades back") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys;
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    keys.e.decay = 30;   // pin the short glow this test fades past
    play(keys, audio, struck(48), 1000);
    const RGB want = expectedColor(48, 255);
    CHECK(keys.ch(19, 10, 0) == want.r);
    CHECK(keys.ch(19, 10, 1) == want.g);
    CHECK(keys.ch(19, 10, 2) == want.b);
    CHECK(keys.at(19, 0) == keys.at(19, 10));   // the whole top-half key lights
    CHECK(keys.at(19, 31) == 0);                // the bottom half stays the dark picture
    platform::setTestNowMs(1400);   // past the 300 ms glow
    keys.layer.tick();
    CHECK(keys.maxIn(4, 32, 0, 32) == 0);
    audio.release();
}

// C#4 is key 40, an ebony key over the boundary at column 16, drawn over the top half's top two thirds.
TEST_CASE("a struck ebony key lights its own boundary at the top, full color, and stops at half height") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys;
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    play(keys, audio, struck(40), 1000);
    const RGB want = expectedColor(40, 255);
    CHECK(keys.ch(16, 5, 0) == want.r);
    CHECK(keys.ch(16, 5, 1) == want.g);
    CHECK(keys.ch(16, 5, 2) == want.b);
    CHECK(keys.at(16, 9) > 0);
    CHECK(keys.at(16, 10) == 0);    // below the ebony key, two thirds of the half height
    audio.release();
}

TEST_CASE("a sounding key brightens as ivory, and only the strike gives it a color") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys;
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    f.notes[48] = 200;                          // sounds, no hit: ivory, brighter than rest
    play(keys, audio, f, 1000);
    CHECK(keys.ch(19, 10, 0) == keys.ch(19, 10, 1));   // grey, not a palette hue
    CHECK(keys.ch(19, 10, 1) == keys.ch(19, 10, 2));
    CHECK(keys.at(19, 10) > 3 * 100);
    play(keys, audio, struck(48, 200), 2000);   // struck: the palette color
    const RGB want = expectedColor(48, 255);
    CHECK(keys.ch(19, 10, 0) == want.r);
    CHECK(keys.ch(19, 10, 1) == want.g);
    CHECK(keys.ch(19, 10, 2) == want.b);
    audio.release();
}

// The render loop runs faster than the audio blocks, so two ticks often read one frame: that is one hit, the glow fading on.
TEST_CASE("a frame two ticks read is one hit: the glow fades on instead of starting again") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys;
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    keys.e.decay = 30;   // pin the 300 ms glow this test reads 80 ms into
    audio.frameForTest() = struck(48);
    audio.frameForTest().seq = 7;
    platform::setTestNowMs(1000);
    keys.layer.tick();
    const RGB full = expectedColor(48, 255);
    const int first = keys.at(19, 10);
    CHECK(first == full.r + full.g + full.b);
    platform::setTestNowMs(1080);   // the same frame, read again: 237 of 255 left on the quadratic fade
    keys.layer.tick();
    const RGB faded = expectedColor(48, 237);
    const int again = keys.at(19, 10);
    CHECK(again == faded.r + faded.g + faded.b);
    CHECK(again < first);           // fading, not started again
    audio.frameForTest() = AudioFrame{};
    audio.release();
}

// The default glow lasts a second (decay 100), so a struck note stays colored instead of flashing out.
TEST_CASE("the default glow lasts a second") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys;
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    play(keys, audio, struck(48), 1000);
    platform::setTestNowMs(1500);   // 500 ms into the 1000 ms glow: 192 of 255 left on the quadratic fade
    keys.layer.tick();
    const RGB half = expectedColor(48, 192);
    CHECK(keys.at(19, 10) == half.r + half.g + half.b);
    audio.release();
}

// With octaves at 2 on a 128x128 grid: top half rows 0..63, bottom half rows 64..127, circle slots of 9 rows, first center row 68.
TEST_CASE("the bottom half draws a small circle per sounding octave on fixed octave rows, smaller than the key, labeled at the left") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys(128, 128);
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    keys.e.octaves = 2;
    play(keys, audio, struck(48), 1000);   // A4, octave 4: a circle on key 0 (A0) at octave row 4
    const RGB want48 = expectedColor(48, 255);
    CHECK(keys.at(4, 104) == want48.r + want48.g + want48.b);   // cy = 64 + 4*9 + 4 = 104, cx 4, r 2
    CHECK(keys.at(7, 104) == 0);                                // the circle drops on the 8-wide key, not past it
    CHECK(keys.at(4, 30) == 0);                                 // the top half key is not struck
    AudioFrame line;
    line.notesCount = kAudioNoteKeys;
    line.notesHits[48 >> 3] = static_cast<uint8_t>(1u << (48 & 7));
    line.notesHits[49 >> 3] = static_cast<uint8_t>(1u << (49 & 7));   // A#4: same octave row, on the ebony key
    play(keys, audio, line, 1000);
    const RGB want49 = expectedColor(49, 255);
    CHECK(keys.at(4, 104) == want48.r + want48.g + want48.b);   // one horizontal line per octave
    CHECK(keys.at(9, 104) == want49.r + want49.g + want49.b);
    play(keys, audio, struck(1), 1000);    // A#0, an ebony key, octave 0
    const RGB want1 = expectedColor(1, 255);
    CHECK(keys.at(9, 68) == want1.r + want1.g + want1.b);       // cy = 64 + 0*9 + 4 = 68
    play(keys, audio, struck(0), 1000);    // A0, a shown key: colored on top AND a circle below
    const RGB want0 = expectedColor(0, 255);
    CHECK(keys.at(4, 10) == want0.r + want0.g + want0.b);
    CHECK(keys.at(4, 68) == want0.r + want0.g + want0.b);
    AudioFrame sound;
    sound.notesCount = kAudioNoteKeys;
    sound.notes[36] = 200;                 // sounds, no hit: still a palette-colored circle
    play(keys, audio, sound, 1000);
    const RGB want36 = expectedColor(36, 200);
    CHECK(keys.at(4, 95) == want36.r + want36.g + want36.b);   // octave row 3: cy = 64 + 3*9 + 4 = 95
    audio.release();
}

// With auto on, the detected key's root in the middle octave becomes the first shown key.
TEST_CASE("auto picks the shown window from the detected key") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys(128, 128);
    keys.e.autoKey = true;
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    keys.e.octaves = 2;
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    f.key = 13;                            // C minor: root C, the 4th key of an octave A-based: window starts at key 39
    play(keys, audio, f, 1000);
    CHECK(keys.maxIn(4, 128, 6, 26) == 0);               // nothing struck yet: top half dark
    f.notesHits[39 >> 3] = static_cast<uint8_t>(1u << (39 & 7));
    play(keys, audio, f, 1000);
    const RGB want39 = expectedColor(39, 255);
    CHECK(keys.at(4, 10) == want39.r + want39.g + want39.b);   // key 39 is the first shown key, columns 0..7
    f.notesHits[39 >> 3] = 0;
    f.notesHits[0] = 1;                                        // A0, not in the window: a circle on the shown A key 48
    play(keys, audio, f, 1000);
    const RGB want0 = expectedColor(0, 255);
    CHECK(keys.at(49, 68) == want0.r + want0.g + want0.b);     // key 48: k=5, x0 45, cx 49, octave row 0
    audio.release();
}

// octave picks the first shown octave: with octave 2 a struck key below the window colors nothing on top but circles on the bottom.
TEST_CASE("octave selects which octaves the top half shows") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys(128, 128);
    keys.e.ivory = 0;
    keys.e.noteNames = false;
    keys.e.octave = 2;
    keys.e.octaves = 2;   // shown keys 24..47
    play(keys, audio, struck(0), 1000);    // A0, octave 0: not shown on top, but its pitch circles below
    CHECK(keys.maxIn(4, 128, 6, 26) == 0);   // top half dark, past the letter strip and labels
    const RGB want0 = expectedColor(0, 255);
    CHECK(keys.at(4, 68) == want0.r + want0.g + want0.b);    // on the shown A key 24
    CHECK(keys.at(68, 68) == want0.r + want0.g + want0.b);   // and on the shown A key 36
    play(keys, audio, struck(24), 2000);   // key 24 is shown: colored on top
    const RGB want24 = expectedColor(24, 255);
    CHECK(keys.at(4, 10) == want24.r + want24.g + want24.b);
    audio.release();
}

// With octaves at 2 on a 128-wide grid: ivory letters along the bottom edge, no ebony strip on top, left octave labels in black.
TEST_CASE("ivory letters run along the bottom edge and the left octave labels are white") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys(128, 32);
    keys.e.noteNames = false;
    keys.e.octaves = 2;   // ivory at its default 128
    play(keys, audio, AudioFrame{}, 1000);
    CHECK(keys.at(1, 10) > 300);                       // key 0 (A0) rests bright ivory
    CHECK(keys.maxIn(0, 128, 0, 5) < 3 * 255);         // no white text along the top edge above the label
    CHECK(keys.minIn(2, 6, 26, 32) == 0);              // key 0's "A" letter, dark on the ivory bottom edge
    CHECK(keys.at(9, 7) < keys.at(1, 10));             // key 1 (A#0) rests as ebony, a quarter of the ivory
    CHECK(keys.maxIn(0, 4, 16, 22) == 3 * 255);        // the bottom row label "0", white
    CHECK(keys.maxIn(0, 4, 5, 11) == 3 * 255);         // the selected octave label on the top half, white too
    audio.release();
}

TEST_CASE("noteNames writes the strongest pitch classes top left and the key with its tuning top right, and nothing when off") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys keys(64, 32);
    keys.e.ivory = 0;
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    f.chroma[0] = 200; f.chroma[4] = 180; f.chroma[7] = 220;   // C E G
    f.key = 1;                                                  // C major
    f.tuning = 12;
    play(keys, audio, f, 1000);
    const std::vector<int> cols = keys.whiteColumns(0, 5);
    REQUIRE(!cols.empty());
    CHECK(cols.front() < 10);                    // "C E G" at the left
    CHECK(cols.back() > 64 - 12);                // "C +12" at the right
    keys.e.noteNames = false;
    play(keys, audio, f, 2000);
    CHECK(keys.whiteColumns(0, 5).empty());
    audio.release();
}

TEST_CASE("a single row lights the struck key's own span, and the keyboard survives 1x1 and 0x0 with every key struck") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Keys row(32, 1);
    row.e.ivory = 0;
    play(row, audio, struck(49), 1000);
    const RGB want = expectedColor(49, 255);
    CHECK(row.at(19, 0) == want.r + want.g + want.b);   // key 49 spans columns 19 up to 20 on 32 lights
    CHECK(row.at(18, 0) == 0);
    survivesTinyGrids<Keys>(audio);
    audio.release();
}

// A key off the shown window is never drawn, so nothing polls its flash: the hit state must still end it, or the tick wraps and it flashes again.
TEST_CASE("a struck note's flash ends on time, polled late or never polled") {
    AudioFrame hit = struck(10);
    hit.seq = 1;   // a new frame, as a source numbers it
    // Polled late: 1040 ms after the hit is past a 1000 ms span, never a wrapped 16 ms.
    NoteHits late;
    late.note(hit, 0, 1000);
    CHECK(late.glow(10, 1040, 1000) == 0);
    // Never polled: note ends the flash on its own, so a poll a whole wrap later finds nothing.
    NoteHits idle;
    idle.note(hit, 0, 1000);
    AudioFrame quiet;
    quiet.seq = 2;   // the next frame, with no hit
    idle.note(quiet, 1010, 1000);
    CHECK(idle.glow(10, 262144 + 500, 1000) == 0);   // 2^16 ticks of 4 ms on: the age would read 500
}
