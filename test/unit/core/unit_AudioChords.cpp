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
        t.name(f, 1000);
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
    t.name(f, 1000);
    AudioFrame passing = chordFrame({0, 4, 7}, 2);
    passing.chroma[2] = 120;   // a D, quieter than the chord
    t.name(passing, 1021);
    CHECK(passing.chordRoot == 0);
    CHECK(passing.chordQuality == kChordMajor);
}

// The name holds over a held chord, and a change to another chord follows at the change detector's frame, not before.
TEST_CASE("a held chord keeps its name, and a new chord takes over at the harmony change") {
    ChordTracker t;
    AudioFrame c = chordFrame({0, 4, 7}, 1);
    t.name(c, 1000);
    AudioFrame am = chordFrame({9, 0, 4}, 2);   // A minor, no change detected yet
    t.name(am, 1021);
    CHECK(am.chordQuality == kChordMajor);   // still C: no change fired
    AudioFrame changed = chordFrame({9, 0, 4}, 3, true);
    t.name(changed, 1042);
    CHECK(changed.chordRoot == 9);
    CHECK(changed.chordQuality == kChordMinor);
}

// A drum hit smears the pitch classes for a frame or two; the chord holds through it and is dropped only once the music has left it for kDropHoldMs.
TEST_CASE("silence names no chord, a chord holds through a beat's smear, and stops once nothing fits for the hold") {
    ChordTracker t;
    AudioFrame quiet = chordFrame({}, 1);
    t.name(quiet, 1000);
    CHECK(quiet.chordQuality == kChordNone);
    AudioFrame c = chordFrame({0, 4, 7}, 2);
    t.name(c, 1021);
    const auto smeared = [](uint16_t seq) {           // a drum hit: every pitch class alike, no chord fitting
        AudioFrame f = chordFrame({}, seq);
        for (uint8_t& pc : f.chroma) pc = 150;
        return f;
    };
    AudioFrame smear = smeared(3);
    t.name(smear, 1042);
    CHECK(smear.chordQuality == kChordMajor);       // held through the smear
    AudioFrame back = chordFrame({0, 4, 7}, 4);
    t.name(back, 1063);
    CHECK(back.chordQuality == kChordMajor);
    uint16_t seq = 5;
    uint32_t ms = 1084;
    AudioFrame other;
    for (; ms < 1084 + ChordTracker::kDropHoldMs + 21; ms += 21) { other = smeared(seq++); t.name(other, ms); }
    CHECK(other.chordQuality == kChordNone);        // no chord fitting for longer than the hold
}

// Once the named chord stops matching, a chord that fits takes over at once, change detector or not: the music moved.
TEST_CASE("a chord that fits takes over at once when the named one stops matching") {
    ChordTracker t;
    AudioFrame c = chordFrame({0, 4, 7}, 1);
    t.name(c, 1000);
    AudioFrame g = chordFrame({7, 11, 2}, 2);   // G major, nothing of C but its G
    t.name(g, 1021);
    CHECK(g.chordRoot == 7);
    CHECK(g.chordQuality == kChordMajor);
}

// A melody's passing tone on a triad does not make it a seventh: the plain triad is preferred until the fourth tone is as strong as the chord's own.
TEST_CASE("a C major triad with a quieter B names C, and a B as strong as the triad names Cmaj7") {
    auto named = [](uint8_t b) {
        ChordTracker t;
        AudioFrame f = chordFrame({0, 4, 7}, 1);
        f.chroma[11] = b;
        t.name(f, 1000);
        char out[8];
        formatChord(f.chordRoot, f.chordQuality, out, sizeof(out));
        return std::string(out);
    };
    CHECK(named(120) == "C");
    CHECK(named(255) == "Cmaj7");
}
