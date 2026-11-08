/// @module SheetMusicEffect

/// Pins the sheet music: on the staff a note sits on its line or space, a sharp beside its natural with a mark, ledger lines beyond the staves; in the piano roll a key's row is its pitch; both scroll left at one pace at any render rate, survive any grid and let their columns go.

#include "doctest.h"
#include "audio_effect_test.h"
#include "light/effects/SheetMusicEffect.h"

#include <memory>

using namespace mm;

namespace {

constexpr int kLineSum = 14;   ///< a channel of the faint line on each C, which a lit row outshines

/// The piano roll on a `w` by `h` grid, A2 up over four octaves, every key heard, scrolling 20 columns a second.
struct Roll : EffectOnGrid<SheetMusicEffect> {
    explicit Roll(int w = 16, int h = 48) : EffectOnGrid(w, h) { e.view = SheetMusicEffect::kRoll; e.notes = AudioNotesBase::kNotesAll; e.noteNames = false; e.octave = 2; e.octaves = 4; e.speed = 20; }
};

/// The staff on a 16 by 52 grid, one row a step, middle C on row 26, every key heard, scrolling 20 columns a second.
struct Staff : EffectOnGrid<SheetMusicEffect> {
    Staff() : EffectOnGrid(16, 52) { e.notes = AudioNotesBase::kNotesAll; e.noteNames = false; e.speed = 20; }
};

/// Whether (`x`, `y`) shows a staff or ledger line's light.
template <typename F>
bool isLine(const F& fx, int x, int y) { return fx.ch(x, y, 0) == 160 && fx.ch(x, y, 1) == 160 && fx.ch(x, y, 2) == 160; }

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
    const int strike = r.at(15, 23);
    audio.frameForTest() = analyzed(noteFrame(48, 200, false), r.confirm);   // held on
    tickFor(r, 1000, 200, 20);
    CHECK(r.at(15, 23) > 0);
    CHECK(r.at(15, 23) < strike);   // the held bar is dimmer than its start
    audio.release();
}

TEST_CASE("the roll scrolls left at one pace at any render rate: a strike is four columns in after 200 ms at 20 columns a second, rendered every 5 or every 50 ms") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    for (uint32_t tickMs : {5u, 50u}) {
        Roll r;
        play(r, audio, struck(48), 1000);
        tickFor(r, 1000, 200, tickMs);
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

TEST_CASE("the sheet music holds a column a light across and frees them when disabled") {
    Roll r(16, 8);
    CHECK(r.e.dynamicBytes() == 16 * (2 * kHitBytes + 1));   // each column's kept and struck keys, and where its notes stand
    r.e.setEnabled(false);
    r.layer.applyState();
    CHECK(r.e.dynamicBytes() == 0);
}

TEST_CASE("the sheet music shows every note the sieves keep on a staff by default, without a hand's limit") {
    SheetMusicEffect music;
    CHECK(music.notes == AudioNotesBase::kNotesNoOvertones);
    CHECK(music.view == SheetMusicEffect::kStaff);
}

TEST_CASE("a note's staff step is its diatonic place: middle C 28, E4 30, C#4 on C4's step and sharp, A0 5, the top key D#7 50") {
    CHECK(SheetMusicEffect::stepOf(60 - 21) == 28);
    CHECK(SheetMusicEffect::stepOf(64 - 21) == 30);
    CHECK(SheetMusicEffect::stepOf(61 - 21) == 28);
    CHECK(SheetMusicEffect::isSharp(61 - 21));
    CHECK_FALSE(SheetMusicEffect::isSharp(60 - 21));
    CHECK(SheetMusicEffect::stepOf(0) == 5);
    CHECK(SheetMusicEffect::stepOf(kAudioNoteKeys - 1) == 50);
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

TEST_CASE("a sharp sits on its natural's step with its mark before the head") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    play(st, audio, struck(61 - 21), 1000);   // C#4: its head on columns 14 and 15
    CHECK(st.at(15, 26) > 0);
    CHECK(st.at(13, 26) == 3 * 255);
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

// On 104 rows a step is two rows and a head five columns; E4 is row 48 and its stem rises from 47 to 34, at 20 columns a second.
TEST_CASE("how long a note held writes its value: a short one an eighth with a flag, half a second a quarter, a second a hollow half note, two seconds a whole note without a stem") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    auto held = [&](uint32_t ms, int w) {
        auto st = std::make_unique<EffectOnGrid<SheetMusicEffect>>(w, 104);
        st->e.notes = AudioNotesBase::kNotesAll;
        st->e.noteNames = false;
        st->e.speed = 20;
        play(*st, audio, struck(64 - 21), 1000);
        audio.frameForTest() = analyzed(ms > 50 ? noteFrame(64 - 21, 200, false) : AudioFrame{}, st->confirm);
        tickFor(*st, 1000, ms, 10);
        audio.frameForTest() = AudioFrame{};
        return st;
    };
    const auto eighth = held(50, 40);              // struck, gone: the head ends at column 38
    CHECK(eighth->at(38, 40) > 0);                 // its stem
    CHECK(eighth->at(39, 34) > 0);                 // and its flag, right of the stem's tip
    const auto quarter = held(500, 40);            // ten columns: the head ends at column 29
    CHECK(quarter->at(29, 40) > 0);                // a stem
    CHECK(quarter->at(30, 34) == 0);               // no flag
    CHECK_FALSE(isLine(*quarter, 27, 48));         // a filled head over the line
    const auto half = held(1000, 40);              // twenty columns: the head ends at column 19
    CHECK(half->at(15, 48) > 0);                   // the oval's edge
    CHECK(isLine(*half, 17, 48));                  // its hollow middle shows the E4 line through it
    CHECK(half->at(19, 46) > 0);                   // and its stem
    const auto whole = held(2000, 60);             // forty columns: the head ends at column 19
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

// Two notes struck one column apart on a staff whose head is two columns wide: the later joins the earlier's column, one chord.
TEST_CASE("notes struck within a head's width of each other stand on one column as a chord") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    play(st, audio, struck(64 - 21), 1000);   // E4, row 24
    play(st, audio, struck(67 - 21), 1050);   // G4, row 22, one column later
    CHECK(st.at(14, 24) > 0);                 // E4's head ends on column 14, where it was struck
    CHECK(st.at(14, 22) > 0);                 // G4 joins it there
    CHECK_FALSE(isLine(st, 14, 22));
    CHECK(isLine(st, 15, 22));                // not on its own column
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
TEST_CASE("a chord stays together as it scrolls, and a note held past the left edge does not start again there") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Staff st;
    play(st, audio, struck(64 - 21), 1000);   // E4
    play(st, audio, struck(67 - 21), 1050);   // G4 one column later joins E4's chord
    tickFor(st, 1050, 400, 50);               // eight columns on: the chord at column 14 - 8
    CHECK(st.at(6, 24) > 0);
    CHECK(st.at(6, 22) > 0);
    CHECK(isLine(st, 7, 22));
    Staff held;
    play(held, audio, struck(60 - 21), 1000);   // middle C, whose ledger line reaches a column right of its head
    audio.frameForTest() = analyzed(noteFrame(60 - 21, 200, false), held.confirm);
    tickFor(held, 1000, 1000, 50);            // twenty columns on: its start has left the 16-column panel
    CHECK(held.at(1, 26) == 0);               // no note, nor its ledger line, drawn again at the left edge
    audio.frameForTest() = AudioFrame{};
    audio.release();
}
