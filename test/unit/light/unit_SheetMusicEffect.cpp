/// @module SheetMusicEffect

/// Pins the sheet music: on the staff a note sits on its line or space, spelled in its key, ledger lines beyond the staves; in the piano roll a key's row is its pitch; a note appears at the right edge at once, the sheet rolls at one pace at any render rate and faster as notes come, no two chords overlap, and any grid survives.

#include "doctest.h"
#include "audio_effect_test.h"
#include "light/effects/SheetMusicEffect.h"

#include <memory>

using namespace mm;

namespace {

constexpr int kLineSum = 14;   ///< a channel of the faint line on each C, which a lit row outshines

/// The piano roll on a `w` by `h` grid, A2 up over four octaves, every key heard; 16 columns drift 4 a second.
struct Roll : EffectOnGrid<SheetMusicEffect> {
    explicit Roll(int w = 16, int h = 48) : EffectOnGrid(w, h) { e.view = SheetMusicEffect::kRoll; e.notes = AudioNotesBase::kNotesAll; e.noteNames = false; e.octave = 2; e.octaves = 4; }
};

/// The staff on a 16 by 52 grid, one row a step, middle C on row 26, every key heard; a chord's room is 5 columns (a head of 2, its marks 2, air 1), and the sheet drifts 4 columns a second.
struct Staff : EffectOnGrid<SheetMusicEffect> {
    Staff() : EffectOnGrid(16, 52) { e.notes = AudioNotesBase::kNotesAll; e.noteNames = false; }
};


/// Whether (`x`, `y`) shows a staff or ledger line's light.
template <typename F>
bool isLine(const F& fx, int x, int y) { return fx.ch(x, y, 0) == 160 && fx.ch(x, y, 1) == 160 && fx.ch(x, y, 2) == 160; }

/// E4 (row 24) struck at 1000 ms and G4 (row 22) at `gMs`.
template <typename F>
void eThenG(F& st, AudioService& audio, uint32_t gMs) {
    play(st, audio, struck(64 - 21), 1000);
    play(st, audio, struck(67 - 21), gMs);
}

/// The rightmost column of row `y` a note lights, past any staff line; -1 for none.
template <typename F>
int noteColumn(const F& fx, int y) {
    for (int x = fx.grid.width - 1; x >= 0; x--) if (fx.at(x, y) > 0 && !isLine(fx, x, y)) return x;
    return -1;
}

/// The rows lit in column `x`.
std::vector<int> litRows(const Roll& r, int x) {
    std::vector<int> rows;
    for (int y = 0; y < r.grid.height; y++) if (r.at(x, y) > 3 * kLineSum) rows.push_back(y);
    return rows;
}

}  // namespace

// On 48 rows the four octaves from A2 (key 24) are one row a key, the lowest at the bottom: A4, key 48, is the 25th key shown, row 47 - 24.
TEST_CASE("a struck note lights its key's row at the right edge, bright where it was struck") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Roll r;
    play(r, audio, struck(48), 1000);
    CHECK(litRows(r, 15) == std::vector<int>{23});
    audio.frameForTest() = analyzed(noteFrame(48, 200, false), r.confirm);   // held on for a second: four columns
    tickFor(r, 1000, 1000, 50);
    CHECK(r.at(15, 23) > 0);
    CHECK(r.at(15, 23) < r.at(11, 23));   // the held bar is dimmer than its start
    audio.frameForTest() = AudioFrame{};
    audio.release();
}

TEST_CASE("the sheet rolls left at one pace at any render rate: a strike is four columns in after a second on 16 columns, rendered every 5 or every 50 ms") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    for (uint32_t tickMs : {5u, 50u}) {
        Roll r;
        play(r, audio, struck(48), 1000);
        tickFor(r, 1000, 1000, tickMs);
        int brightest = -1;
        for (int x = 0; x < 16; x++) if (brightest < 0 || r.at(x, 23) > r.at(brightest, 23)) brightest = x;
        CHECK(brightest == 15 - 4);
    }
    audio.release();
}

TEST_CASE("an octave error shows as a bar of the same color twelve rows up") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Roll r;
    AudioFrame f = struck(36);   // A3 and its octave A4
    f.notes[48] = 200;
    f.notesHits[48 >> 3] = static_cast<uint8_t>(f.notesHits[48 >> 3] | (1u << (48 & 7)));
    play(r, audio, f, 1000);
    CHECK(litRows(r, 15) == std::vector<int>{23, 35});
    CHECK(r.ch(15, 23, 0) == r.ch(15, 35, 0));
    CHECK(r.ch(15, 23, 1) == r.ch(15, 35, 1));
    CHECK(r.ch(15, 23, 2) == r.ch(15, 35, 2));
    audio.release();
}

TEST_CASE("the piano roll survives a 1x1 and a 0x0 grid") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    survivesTinyGrids<Roll>(audio);
    audio.release();
    CHECK(true);   // reached without a crash
}

TEST_CASE("the sheet music holds two notes a light across and frees them when disabled") {
    Roll r(16, 8);
    CHECK(r.e.dynamicBytes() == 16 * 2 * 16);   // 16 bytes a note
    r.e.setEnabled(false);
    r.layer.applyState();
    CHECK(r.e.dynamicBytes() == 0);
}

TEST_CASE("the sheet music shows every note the sieves keep on a staff by default, without a hand's limit") {
    SheetMusicEffect music;
    CHECK(music.notes == AudioNotesBase::kNotesNoOvertones);
    CHECK(music.view == SheetMusicEffect::kStaff);
}

TEST_CASE("with no key a note's step is its diatonic place and a black key is sharp: middle C 28, E4 30, C#4 on C4's step, A0 5, the top key D#7 50") {
    using S = SheetMusicEffect;
    CHECK(S::spell(60 - 21, 0).step == 28);
    CHECK(S::spell(60 - 21, 0).accidental == S::kNoMark);
    CHECK(S::spell(64 - 21, 0).step == 30);
    CHECK(S::spell(61 - 21, 0).step == 28);
    CHECK(S::spell(61 - 21, 0).accidental == S::kSharp);
    CHECK(S::spell(0, 0).step == 5);
    CHECK(S::spell(kAudioNoteKeys - 1, 0).step == 50);
}

TEST_CASE("a key's signature counts on the circle of fifths: G major one sharp, D major two, F major one flat, G minor two flats, E minor one sharp, A minor and no key none") {
    using S = SheetMusicEffect;
    CHECK(S::fifthsOf(8) == 1);    // G major
    CHECK(S::fifthsOf(3) == 2);    // D major
    CHECK(S::fifthsOf(6) == -1);   // F major
    CHECK(S::fifthsOf(20) == -2);  // G minor
    CHECK(S::fifthsOf(17) == 1);   // E minor
    CHECK(S::fifthsOf(22) == 0);   // A minor
    CHECK(S::fifthsOf(0) == 0);
}

// The product owner's screenshot (2026-11-08): every black key carried a sharp, which crowded a mix. Notation marks only what the signature does not say.
TEST_CASE("a note is spelled in its key: B flat on B's line in G minor unmarked, its leading tone F sharp, F natural marked in G major, E flat in F major, B natural in C minor") {
    using S = SheetMusicEffect;
    CHECK(S::spell(70 - 21, 20).step == 34);                     // B flat 4 in G minor: B4's line
    CHECK(S::spell(70 - 21, 20).accidental == S::kNoMark);
    CHECK(S::spell(66 - 21, 20).step == 31);                     // F sharp 4, G minor's leading tone, on F
    CHECK(S::spell(66 - 21, 20).accidental == S::kSharp);
    CHECK(S::spell(66 - 21, 8).accidental == S::kNoMark);        // F sharp in G major is in the key
    CHECK(S::spell(65 - 21, 8).step == 31);                      // F natural in G major
    CHECK(S::spell(65 - 21, 8).accidental == S::kNatural);
    CHECK(S::spell(63 - 21, 6).step == 30);                      // E flat 4 in F major: on E, flattened
    CHECK(S::spell(63 - 21, 6).accidental == S::kFlat);
    CHECK(S::spell(71 - 21, 13).step == 34);                     // B natural 4, C minor's leading tone
    CHECK(S::spell(71 - 21, 13).accidental == S::kNatural);
}

// On 104 rows a step is two rows and the clefs take 12 columns: G major's sharp stands at column 12 on F5's line, row 32, and F3's, row 60; F4 is row 46.
TEST_CASE("the key signature stands after the clefs, and a note of the key carries no mark while one outside it does") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto staff = [&](uint8_t key) {
        auto st = std::make_unique<EffectOnGrid<SheetMusicEffect>>(40, 104);
        st->e.notes = AudioNotesBase::kNotesAll;
        st->e.noteNames = false;
        AudioFrame f = struck(66 - 21);   // F sharp 4, its head ending at column 39
        f.key = key;
        play(*st, audio, f, 1000);
        return st;
    };
    const auto g = staff(8);
    CHECK(g->at(13, 30) == 3 * 160);   // the signature's sharp on the treble staff
    CHECK(g->at(13, 58) == 3 * 160);   // and on the bass staff
    CHECK(g->at(31, 45) == 0);         // the F sharp itself unmarked (row 44 is G4's line)
    const auto none = staff(0);
    CHECK(none->at(13, 30) == 0);      // no key, no signature
    CHECK(none->at(31, 45) == 3 * 160);   // and the sharp before the note
    audio.frameForTest() = AudioFrame{};
    audio.release();
}

// Rows from the middle, a step a row: the treble's lowest line E4 is row 24, the bass staff's highest line A3 row 28; column 0 is the line joining the staves.
TEST_CASE("on the staff E4 sits on the treble's lowest line with its stem up, and middle C on its own ledger line between the staves") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    play(st, audio, struck(64 - 21), 1000);
    CHECK(isLine(st, 2, 24));              // the line itself, left of the note
    CHECK(st.at(15, 24) > 0);              // the head on it, ending at the right edge
    CHECK_FALSE(isLine(st, 15, 24));
    CHECK(st.at(15, 20) > 0);              // its stem, up on the right
    CHECK(isLine(st, 2, 28));              // A3, the bass staff's top line
    CHECK(st.at(2, 26) == 0);              // no line through middle C without a note
    Staff c4;
    play(c4, audio, struck(60 - 21), 1000);
    CHECK(c4.at(15, 26) > 0);
    CHECK(isLine(c4, 13, 26));             // its ledger line, a little wider than the head
    CHECK(c4.at(2, 26) == 0);
    audio.release();
}

// One row a step: a sharp is a dot before the head, as gray as the lines.
TEST_CASE("a sharp sits on its natural's step with its mark before the head") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    play(st, audio, struck(63 - 21), 1000);   // D#4 on D4's step, row 25: its head on columns 14 and 15
    CHECK(st.at(15, 25) > 0);
    CHECK(st.at(13, 25) == 3 * 160);
    audio.release();
}

// C6 is step 42, row 12, two steps above the treble's top line F5 (38): A5's ledger line, row 14, crosses its head's columns.
TEST_CASE("a note above the treble staff gets the ledger lines between it and the staff, only at the note") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    play(st, audio, struck(84 - 21), 1000);
    CHECK(st.at(15, 12) > 0);
    CHECK(isLine(st, 15, 14));
    CHECK(st.at(2, 14) == 0);
    audio.release();
}

// On 104 rows a step is two rows and a head five columns; E4 is row 48 and its stem rises from 47 to 34. 40 columns drift 10 a second, so a note held `ms` stands `ms / 100` columns in.
TEST_CASE("how long a note held writes its value: a short one an eighth with a flag, half a second a quarter, a second a hollow half note, two seconds a whole note without a stem") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto held = [&](uint32_t ms) {
        auto st = std::make_unique<EffectOnGrid<SheetMusicEffect>>(40, 104);
        st->e.notes = AudioNotesBase::kNotesAll;
        st->e.noteNames = false;
        play(*st, audio, struck(64 - 21), 1000);
        audio.frameForTest() = analyzed(noteFrame(64 - 21, 200, false), st->confirm);
        tickFor(*st, 1000, ms, 50);
        audio.frameForTest() = AudioFrame{};
        return st;
    };
    const auto eighth = held(100);                 // the head ends at column 38
    CHECK(eighth->at(38, 40) > 0);                 // its stem
    CHECK(eighth->at(39, 34) > 0);                 // and its flag, right of the stem's tip
    const auto quarter = held(500);                // the head ends at column 34
    CHECK(quarter->at(34, 40) > 0);                // a stem
    CHECK(quarter->at(35, 34) == 0);               // no flag
    CHECK_FALSE(isLine(*quarter, 32, 48));         // a filled head over the line
    const auto half = held(1000);                  // the head ends at column 29
    CHECK(half->at(25, 48) > 0);                   // the oval's edge
    CHECK(isLine(*half, 27, 48));                  // its hollow middle shows the E4 line through it
    CHECK(half->at(29, 46) > 0);                   // and its stem
    const auto whole = held(2000);                 // the head ends at column 19
    CHECK(whole->at(15, 48) > 0);
    CHECK(whole->at(19, 46) == 0);                 // no stem
    audio.release();
}

TEST_CASE("the staff survives a 1x1 and a 0x0 grid") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    for (int side : {1, 0}) {
        EffectOnGrid<SheetMusicEffect> fx(side, side);
        play(fx, audio, allNotes(), 1000);
        platform::setTestNowMs(1100);
        fx.layer.tick();
    }
    audio.release();
    CHECK(true);   // reached without a crash
}

// Notes struck within one audio frame or two, 50 ms, are one chord: both heads at the right edge.
TEST_CASE("notes starting within 50 ms stand together as a chord, at the right edge") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    eThenG(st, audio, 1040);
    CHECK(noteColumn(st, 24) == 15);
    CHECK(noteColumn(st, 22) == 15);
    audio.release();
}

// The product owner (2026-11-08): no spacing control and no delay. A note is drawn at the right edge at once, and a chord that comes before the last one's room pushes the sheet on.
TEST_CASE("a new chord appears at the right edge at once and pushes the last one its room away") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    eThenG(st, audio, 1100);                  // G4 a chord of its own, before the sheet has rolled a column
    CHECK(noteColumn(st, 22) == 15);          // G4 at the right edge, at once
    CHECK(noteColumn(st, 24) == 10);          // E4 pushed five columns left, its room
    audio.release();
}

// The product owner's idea (2026-11-08): the more notes come, the faster the sheet rolls, so they find room without a speed control.
TEST_CASE("after a run of chords the sheet rolls faster than its drift, and settles back") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto rolled = [&](bool busy) {
        auto st = std::make_unique<Staff>();
        uint32_t t = 1000;
        if (busy) for (int i = 0; i < 20; i++, t += 200) play(*st, audio, struck(81 - 21), t);   // A5, five chords a second
        play(*st, audio, struck(64 - 21), t);   // E4, then half a second of quiet
        audio.frameForTest() = AudioFrame{};
        tickFor(*st, t, 500, 50);
        return 15 - noteColumn(*st, 24);
    };
    CHECK(rolled(false) == 2);   // the drift: 4 columns a second
    CHECK(rolled(true) > 6);     // the chord rate carries it faster
    audio.release();
}

// The product owner saw the staff stand still after switching controls live (2026-11-08): whatever the switches, a later note still appears and scrolls.
TEST_CASE("the staff keeps scrolling new notes after notes, chord, view and the grid are switched live") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    uint32_t t = 1000;
    for (int round = 0; round < 6; round++) {
        st.e.notes = static_cast<uint8_t>(round % 3);
        st.e.chord = round % 2 == 1;
        st.e.view = round == 4 ? SheetMusicEffect::kRoll : SheetMusicEffect::kStaff;
        st.layer.applyState();
        for (int i = 0; i < 20; i++, t += 20) play(st, audio, struck(static_cast<uint8_t>(40 + (i % 7))), t);
    }
    st.e.notes = AudioNotesBase::kNotesAll;
    st.e.chord = false;
    st.e.view = SheetMusicEffect::kStaff;
    st.layer.applyState();
    tickFor(st, t, 2000, 20);   // two seconds of silence: the staff empties
    t += 2000;
    play(st, audio, struck(64 - 21), t);
    CHECK(st.at(15, 24) > 0);   // a new E4 at the right edge
    audio.release();
}


// The product owner's video (2026-11-08): chords regrouped from the left edge each frame, so notes jumped as the leftmost scrolled off.
TEST_CASE("a chord stays together as it rolls, and a note held past the left edge does not start again there") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    eThenG(st, audio, 1040);                  // one chord
    audio.frameForTest() = AudioFrame{};
    tickFor(st, 1040, 1960, 40);              // two seconds on: eight columns
    CHECK(noteColumn(st, 24) == 7);
    CHECK(noteColumn(st, 22) == 7);
    Staff held;
    play(held, audio, struck(60 - 21), 1000);   // middle C, whose ledger line reaches a column right of its head
    audio.frameForTest() = analyzed(noteFrame(60 - 21, 200, false), held.confirm);
    tickFor(held, 1000, 5000, 50);            // twenty columns on: its start has left the 16-column panel
    CHECK(held.at(1, 26) == 0);               // no note, nor its ledger line, drawn again at the left edge
    audio.frameForTest() = AudioFrame{};
    audio.release();
}
