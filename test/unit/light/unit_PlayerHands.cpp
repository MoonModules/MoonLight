/// @module playerHands
/// @also KeyboardEffect, StringsEffect

/// Pins the hands the note effects model: two piano hands of five fingers within a tenth, a fretting hand of one note a string within four frets, and a strum's direction and a bowed note from how the strings started and held.

#include "doctest.h"
#include "light/powerfunctions/playerHands.h"

#include <initializer_list>
#include <string>

using namespace mm;
using namespace mm::hands;

namespace {
/// A set of `keys` at `lvl`, into `level`.
NoteSet keysAt(std::initializer_list<uint8_t> keys, uint8_t lvl, uint8_t (&level)[kAudioNoteKeys]) {
    NoteSet s;
    for (uint8_t k : keys) { s.add(k); level[k] = lvl; }
    return s;
}
}  // namespace

// Eight notes over four octaves: what is kept fits two hands, each at most five keys within 16 semitones.
TEST_CASE("two piano hands keep at most five keys each, each hand within a tenth") {
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet set = keysAt({10, 13, 17, 22, 40, 44, 47, 58}, 200, level);
    level[58] = 255;
    PianoHands hands;
    hands.filter(level, set, 1000);
    int kept = 0;
    for (uint8_t i = 0; i < kAudioNoteKeys; i++) {
        if (!set.has(i)) continue;
        kept++;
        const bool inLeft = i >= hands.window[0] && i <= hands.window[0] + PianoHands::kSpan;
        const bool inRight = i >= hands.window[1] && i <= hands.window[1] + PianoHands::kSpan;
        CHECK((inLeft || inRight));
    }
    CHECK(kept >= 2);
    CHECK(kept <= 2 * PianoHands::kFingers);
    CHECK(set.has(58));   // the strongest key is always played
    CHECK_FALSE((set.has(10) && set.has(40)));   // keys more than two hands apart cannot all stay
}

// Notes in one place belong to one hand: whatever sounds, the right hand's window starts above where the left hand's ends.
TEST_CASE("the two piano hands' windows never overlap") {
    PianoHands hands;
    uint32_t ms = 1000;
    for (std::initializer_list<uint8_t> chord : {std::initializer_list<uint8_t>{30, 34, 37}, {40, 44, 47, 52}, {33, 36}, {70, 74}, {31, 50}}) {
        uint8_t level[kAudioNoteKeys] = {};
        NoteSet set = keysAt(chord, 200, level);
        hands.filter(level, set, ms);
        if (hands.window[0] >= 0 && hands.window[1] >= 0) CHECK(hands.window[1] > hands.window[0] + PianoHands::kSpan);
        ms += 400;
    }
    // The left window leans low to hold 26, 30 and 40; the strongest key left, 42, would anchor a window from 34 that overlaps it.
    PianoHands fresh;
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet set = keysAt({26, 30, 40, 42, 50}, 150, level);
    level[40] = 255;
    fresh.filter(level, set, 1000);
    REQUIRE(fresh.window[0] >= 0);
    REQUIRE(fresh.window[1] >= 0);
    CHECK(fresh.window[1] > fresh.window[0] + PianoHands::kSpan);
}

// A hand centers on the chord it plays, so the next chord a few keys up or down is in reach at once rather than after the move delay; at the chord's lowest window D's A3 was out of reach.
TEST_CASE("a piano hand centers on its chord, so a triad two keys higher plays at once") {
    PianoHands hands;
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet set = keysAt({27, 31, 34}, 200, level);   // C3 E3 G3
    hands.filter(level, set, 1000);
    uint8_t next[kAudioNoteKeys] = {};
    NoteSet f = keysAt({29, 33, 36}, 200, next);      // D3 F#3 A3, at once after
    hands.filter(next, f, 1021);
    CHECK(f.has(29));
    CHECK(f.has(33));
    CHECK(f.has(36));
}

// Seven notes on a guitar: at most six stay, one a string, all within one position of four frets or open.
TEST_CASE("a fretting hand keeps at most one note a string, all within one four-fret position or open") {
    static constexpr uint8_t kGuitar[] = {40, 45, 50, 55, 59, 64};
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet set = keysAt({19, 24, 29, 31, 34, 38, 43}, 200, level);   // E2, A2, D3, E3, G3, B3, E4 as keys
    FrettingHand hand;
    hand.update(level, set, kGuitar, 6, 12, 1000);
    int kept = 0;
    for (uint8_t s = 0; s < 6; s++) {
        if (hand.note[s] < 0) continue;
        kept++;
        const uint8_t fr = hand.fret[s];
        CHECK((fr == 0 || (fr >= hand.position && fr < hand.position + FrettingHand::kReach)));
        CHECK(static_cast<int>(21 + hand.note[s]) - kGuitar[s] == fr);
    }
    CHECK(kept <= 6);
    CHECK(kept >= 5);
}

// A melody within the hand's reach is played from one position: the hand does not shift on every note.
TEST_CASE("a melody within reach keeps the hand at one position") {
    static constexpr uint8_t kGuitar[] = {40, 45, 50, 55, 59, 64};
    FrettingHand hand;
    int moves = 0;
    int8_t last = -1;
    uint32_t ms = 1000;
    for (uint8_t k : {39, 40, 41, 42}) {   // C4 to D#4, one at a time
        uint8_t level[kAudioNoteKeys] = {};
        NoteSet set = keysAt({k}, 200, level);
        hand.update(level, set, kGuitar, 6, 12, ms);
        if (last >= 0 && hand.position != last) moves++;
        last = hand.position;
        ms += 100;
    }
    CHECK(moves == 0);
}

// The movement cost of tablature transcription: of the positions that play a note equally, the hand takes the nearest, not the lowest (GuitarSet: 19% to 24% of notes on their labeled string, 2026-11-08).
TEST_CASE("a note out of the hand's reach moves it to the nearest position that plays it, not the lowest") {
    static constexpr uint8_t kGuitar[] = {40, 45, 50, 55, 59, 64};
    FrettingHand hand;
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet e5 = keysAt({55}, 200, level);   // E5: the high E string's 12th fret, positions 9 to 12
    hand.update(level, e5, kGuitar, 6, 12, 1000);
    REQUIRE(hand.position == 9);
    uint8_t level2[kAudioNoteKeys] = {};
    NoteSet c5 = keysAt({51}, 200, level2);  // C5: the high E string's 8th fret, positions 5 to 8, none in reach of 9
    hand.update(level2, c5, kGuitar, 6, 12, 1100);
    CHECK(hand.position == 8);
}

// Starts spread low to high over three blocks are a downstroke, high to low an upstroke, all in one block a strum without a direction, one start no strum.
TEST_CASE("string starts read as a downstroke, an upstroke, a strum without a direction, or picking") {
    // Each string's start time, string 0 the lowest, fed in the order they happen.
    auto strumOf = [](std::initializer_list<uint32_t> starts) {
        uint32_t at[kMaxStrings] = {};
        uint8_t n = 0;
        for (uint32_t t : starts) at[n++] = t;
        StringPlay play;
        bool done[kMaxStrings] = {};
        for (uint8_t k = 0; k < n; k++) {
            uint8_t next = 0xFF;
            for (uint8_t s = 0; s < n; s++) if (!done[s] && (next == 0xFF || at[s] < at[next])) next = s;
            done[next] = true;
            play.start(next, at[next], 200);
        }
        return play.strum;
    };
    const auto down = strumOf({1000, 1000, 1021, 1021, 1042, 1042});
    CHECK(down.dir == 1);
    CHECK(down.strings == 0x3F);
    CHECK(strumOf({1042, 1042, 1021, 1021, 1000, 1000}).dir == -1);
    const auto together = strumOf({1000, 1000, 1000, 1000, 1000, 1000});
    CHECK(together.strings == 0x3F);
    CHECK(together.dir == 0);
    CHECK(strumOf({1000}).strings == 0);
}

// A plucked note fades to a quarter within 300 ms, a bowed one holds.
TEST_CASE("a note that holds after 300 ms is bowed, one that faded is plucked") {
    StringPlay play;
    play.start(0, 1000, 200);
    CHECK_FALSE(play.bowed(0, 1100, 200));   // too soon to tell
    CHECK(play.bowed(0, 1300, 180));
    CHECK_FALSE(play.bowed(0, 1300, 40));
}

// The chord's shape from the lowest position up, the root as the bass: the open C, A minor and G shapes a guitarist plays.
TEST_CASE("a fretting hand fingers C, A minor and G as their open shapes") {
    static constexpr uint8_t kGuitar[] = {40, 45, 50, 55, 59, 64};
    auto frets = [](uint8_t root, uint8_t quality) {
        FrettingHand hand;
        REQUIRE(hand.shape(root, quality, kGuitar, 6, 12));
        std::string out;
        for (uint8_t s = 0; s < 6; s++) out += hand.note[s] < 0 ? 'x' : static_cast<char>('0' + hand.fret[s]);
        return out;
    };
    CHECK(frets(0, kChordMajor) == "x32010");
    CHECK(frets(9, kChordMinor) == "x02210");
    CHECK(frets(7, kChordMajor) == "320003");
}

namespace {
/// A hit bit for each of `keys`.
void strike(std::initializer_list<uint8_t> keys, uint8_t (&hits)[(kAudioNoteKeys + 7) / 8]) {
    for (uint8_t k : keys) hits[k >> 3] = static_cast<uint8_t>(hits[k >> 3] | (1u << (k & 7)));
}
}  // namespace

// A solo high on the neck over a loud held chord low on it: the struck notes pull the hand up, as they pull the player's.
TEST_CASE("a fretting hand follows the notes being struck, not the louder held ones") {
    static constexpr uint8_t kGuitar[] = {40, 45, 50, 55, 59, 64};
    FrettingHand hand;
    uint32_t ms = 1000;
    for (int frame = 0; frame < 30; frame++, ms += 20) {
        uint8_t level[kAudioNoteKeys] = {};
        NoteSet set = keysAt({20, 25, 30}, 220, level);   // F2 A#2 D#3 held loud, a chord only the first frets play
        const uint8_t solo = frame < 15 ? 60 : 62;          // A5 then B5, fret 17 and 19 on the high E
        set.add(solo);
        level[solo] = 90;
        uint8_t hits[(kAudioNoteKeys + 7) / 8] = {};
        if (frame == 0 || frame == 15) strike({solo}, hits);
        hand.update(level, set, kGuitar, 6, 19, ms, hits);
    }
    CHECK(hand.position >= 15);
    CHECK(hand.note[5] == 62);   // the high E string plays the solo's B5
    CHECK(hand.fret[5] == 19);
}

// A soft melody struck above two loud held clusters: without the strikes both hands take the clusters and the melody goes.
TEST_CASE("the piano hands keep a struck melody over louder held clusters") {
    PianoHands hands;
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet set = keysAt({10, 14, 17, 30, 34, 37}, 220, level);
    set.add(60);
    level[60] = 80;
    uint8_t hits[(kAudioNoteKeys + 7) / 8] = {};
    strike({60}, hits);
    hands.filter(level, set, 1000, hits);
    CHECK(set.has(60));
    PianoHands held;   // the same notes with nothing struck: the melody is the weakest and goes
    uint8_t level2[kAudioNoteKeys] = {};
    NoteSet set2 = keysAt({10, 14, 17, 30, 34, 37}, 220, level2);
    set2.add(60);
    level2[60] = 80;
    held.filter(level2, set2, 1000);
    CHECK_FALSE(set2.has(60));
}

// A strike at full level weighs as much more as any loud strike: the bonus is the strike's, not capped by the key's level.
TEST_CASE("a recent strike at full level weighs more than a quieter one") {
    RecentStrikes strikes;
    uint8_t level[kAudioNoteKeys] = {};
    level[40] = 255;
    level[50] = 100;
    NoteSet set;
    set.add(40);
    set.add(50);
    uint8_t hits[(kAudioNoteKeys + 7) / 8] = {};
    strike({40, 50}, hits);
    uint16_t w[kAudioNoteKeys];
    strikes.weighed(level, hits, set, 1000, w);
    CHECK(w[40] > w[50]);
    CHECK(w[40] == 256 + RecentStrikes::kStruck * 255);
    strikes.weighed(level, hits, set, 1010, w);   // struck again: its latest strike counts, once
    CHECK(w[40] == 256 + RecentStrikes::kStruck * 255);
}
