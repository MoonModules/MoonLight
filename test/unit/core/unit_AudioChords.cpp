/// @module AudioChords

/// Pins the chord named from the twelve pitch classes: the templates name the chords, a passing note does not move the name, and the name changes only at a harmony change.

#include "doctest.h"
#include "core/services/AudioChords.h"

#include <cstring>
#include <initializer_list>
#include <string>

using namespace mm;

namespace {
/// A frame whose pitch classes `pcs` sound at 220, the others silent, numbered `seq`.
AudioFrame chordFrame(std::initializer_list<uint8_t> pcs, uint16_t seq, bool change = false) {
    AudioFrame f;
    for (uint8_t pc : pcs) f.chroma[pc] = 220;
    f.seq = seq;
    f.harmonyChange = change;
    return f;
}
}  // namespace

TEST_CASE("the pitch classes of C major, A minor seven and G seven name those chords") {
    auto named = [](std::initializer_list<uint8_t> pcs) {
        ChordTracker t;
        AudioFrame f = chordFrame(pcs, 1);
        t.name(f);
        char out[8];
        formatChord(f.chordRoot, f.chordQuality, out, sizeof(out));
        return std::string(out);
    };
    CHECK(named({0, 4, 7}) == "C");
    CHECK(named({9, 0, 4, 7}) == "Am7");
    CHECK(named({7, 11, 2, 5}) == "G7");
}

// A melody's passing note between changes does not rename the chord.
TEST_CASE("a C major chord with a passing D in the melody stays C major") {
    ChordTracker t;
    AudioFrame f = chordFrame({0, 4, 7}, 1);
    t.name(f);
    AudioFrame passing = chordFrame({0, 4, 7}, 2);
    passing.chroma[2] = 120;   // a D, quieter than the chord
    t.name(passing);
    CHECK(passing.chordRoot == 0);
    CHECK(passing.chordQuality == kChordMajor);
}

// The name holds over a held chord, and a change to another chord follows at the change detector's frame, not before.
TEST_CASE("a held chord keeps its name, and a new chord takes over at the harmony change") {
    ChordTracker t;
    AudioFrame c = chordFrame({0, 4, 7}, 1);
    t.name(c);
    AudioFrame am = chordFrame({9, 0, 4}, 2);   // A minor, no change detected yet
    t.name(am);
    CHECK(am.chordQuality == kChordMajor);   // still C: no change fired
    AudioFrame changed = chordFrame({9, 0, 4}, 3, true);
    t.name(changed);
    CHECK(changed.chordRoot == 9);
    CHECK(changed.chordQuality == kChordMinor);
}

TEST_CASE("silence names no chord, and a chord stops matching when the music stops") {
    ChordTracker t;
    AudioFrame quiet = chordFrame({}, 1);
    t.name(quiet);
    CHECK(quiet.chordQuality == kChordNone);
    AudioFrame c = chordFrame({0, 4, 7}, 2);
    t.name(c);
    AudioFrame other = chordFrame({1, 6, 11}, 3);   // nothing of C left
    t.name(other);
    CHECK(other.chordQuality == kChordNone);
}
