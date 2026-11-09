/// @module HarmonicSieve

/// Pins the harmonic sieve: a note's overtones leave the set, a loud note above another stays.

#include <initializer_list>   // the braced key lists the loops walk

#include "doctest.h"
#include "core/util/HarmonicSieve.h"

using namespace mm;

namespace {
/// How many keys `set` holds.
int count(const NoteSet& set) {
    int n = 0;
    for (uint8_t i = 0; i < kAudioNoteKeys; i++) n += set.has(i);
    return n;
}
}  // namespace

// A C major triad, each note with its second and third harmonic: the sieve keeps the three notes, without it all eight show, C's third harmonic being G's octave.
TEST_CASE("a C major triad with its overtones keeps its three notes after the sieve, all eight before") {
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    for (uint8_t root : {39, 43, 46}) {   // C4, E4, G4 as keys from A0
        f.notes[root] = 220;
        f.notes[root + 12] = 150;
        f.notes[root + 19] = 120;
    }
    NoteSet set = soundingNotes(f);
    CHECK(count(set) == 8);
    harmonicSieve(f.notes, f.notesCount, set);
    CHECK(count(set) == 3);
    CHECK(set.has(39));
    CHECK(set.has(43));
    CHECK(set.has(46));
}

// A twelfth above a note is its third harmonic: weaker, it goes; as loud, it is a note of its own.
TEST_CASE("a weak note a twelfth above a loud one is dropped, a loud one is kept") {
    uint8_t level[kAudioNoteKeys] = {};
    level[20] = 200;
    level[39] = 100;
    NoteSet weak;
    weak.add(20);
    weak.add(39);
    harmonicSieve(level, kAudioNoteKeys, weak);
    CHECK_FALSE(weak.has(39));
    level[39] = 200;
    NoteSet loud;
    loud.add(20);
    loud.add(39);
    harmonicSieve(level, kAudioNoteKeys, loud);
    CHECK(loud.has(39));
}

// A note's harmonics start with it, so a key an octave above a louder one, struck while that one is not, is a note of its own: a solo note over a held chord.
TEST_CASE("a weak note an octave above a loud one stays when struck on its own, and goes when struck with it") {
    uint8_t level[kAudioNoteKeys] = {};
    level[20] = 200;
    level[32] = 100;   // an octave above
    uint8_t alone[(kAudioNoteKeys + 7) / 8] = {};
    alone[32 >> 3] = static_cast<uint8_t>(1u << (32 & 7));
    NoteSet solo;
    solo.add(20);
    solo.add(32);
    harmonicSieve(level, kAudioNoteKeys, solo, alone);
    CHECK(solo.has(32));
    uint8_t together[(kAudioNoteKeys + 7) / 8] = {};
    together[32 >> 3] = static_cast<uint8_t>(1u << (32 & 7));
    together[20 >> 3] = static_cast<uint8_t>(together[20 >> 3] | (1u << (20 & 7)));
    NoteSet overtone;
    overtone.add(20);
    overtone.add(32);
    harmonicSieve(level, kAudioNoteKeys, overtone, together);
    CHECK_FALSE(overtone.has(32));
}

// Five adjacent keys around a strong one are its leakage, a filter's skirt: peak picking keeps the peak; two neighbors of like strength, a played second, both stay.
TEST_CASE("peak picking keeps the strong key of five adjacent ones, and both keys of a played second") {
    uint8_t level[kAudioNoteKeys] = {};
    NoteSet row;
    for (uint8_t k = 38; k <= 42; k++) { row.add(k); level[k] = 120; }
    level[40] = 220;
    leakageSieve(level, kAudioNoteKeys, row);
    CHECK(count(row) == 1);
    CHECK(row.has(40));
    uint8_t second[kAudioNoteKeys] = {};
    second[50] = 200;
    second[52] = 195;
    NoteSet both;
    both.add(50);
    both.add(52);
    leakageSieve(second, kAudioNoteKeys, both);
    CHECK(count(both) == 2);
}

namespace {
/// A frame where key `i` sounds at `level`, struck when `hit`.
AudioFrame keyFrame(uint8_t i, uint8_t level, bool hit) {
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    f.notes[i] = level;
    if (hit) f.notesHits[i >> 3] = static_cast<uint8_t>(1u << (i & 7));
    return f;
}
}  // namespace

// An attack spreads a weak burst over the keys for a frame; a note outlasts it, and a loud one counts at once.
TEST_CASE("a weak key counts from its second frame running, a loud one on its first") {
    NoteConfirm confirm;
    NoteSet set;
    uint8_t struck[kHitBytes] = {};
    confirm.apply(keyFrame(30, 10, false), set, struck);
    CHECK_FALSE(set.has(30));                       // one weak frame: an attack's burst, no note
    confirm.apply(keyFrame(30, 10, false), set, struck);
    CHECK(set.has(30));                             // still there a frame on: a note
    NoteConfirm loud;
    loud.apply(keyFrame(40, kInstantLevel, false), set, struck);
    CHECK(set.has(40));
    NoteConfirm flash;
    flash.apply(keyFrame(50, 5, true), set, struck);
    flash.apply(AudioFrame{}, set, struck);
    CHECK_FALSE(set.has(50));                       // a burst that is gone a frame on never counts
}

// Most of an attack's flashes last two frames, far under the note the attack starts (GuitarSet and URMP, 2026-11-08): a key far under the frame's loudest waits a third frame.
TEST_CASE("a key far under the frame's loudest key counts from its third frame running, a key near it from its second") {
    AudioFrame f = keyFrame(30, 200, false);   // a loud note, and a burst key far under it
    f.notes[60] = 200 - kInstantSpread - 1;
    f.notes[45] = 200 - kInstantSpread;        // and one near enough by a hair
    NoteConfirm confirm;
    NoteSet set;
    uint8_t struck[kHitBytes] = {};
    confirm.apply(f, set, struck);
    CHECK(set.has(30));                        // loud and the loudest: at once
    CHECK(set.has(45));                        // loud enough and near it: at once
    CHECK_FALSE(set.has(60));
    confirm.apply(f, set, struck);
    CHECK_FALSE(set.has(60));                  // a second frame is not enough far under the loudest
    confirm.apply(f, set, struck);
    CHECK(set.has(60));                        // a third is: it outlasts an attack
}

// The strike lands on a weak note's first frame, before it counts: it carries to the frame that counts it, once.
TEST_CASE("a strike on a key not counted yet carries to the frame that counts it, and only once") {
    NoteConfirm confirm;
    NoteSet set;
    uint8_t struck[kHitBytes] = {};
    confirm.apply(keyFrame(30, 10, true), set, struck);
    CHECK_FALSE(set.has(30));
    confirm.apply(keyFrame(30, 10, false), set, struck);
    CHECK(set.has(30));
    CHECK(hitAt(struck, 30));                       // the strike, a frame late, with the note it started
    confirm.apply(keyFrame(30, 10, false), set, struck);
    CHECK_FALSE(hitAt(struck, 30));                 // not struck again
    NoteConfirm loud;
    loud.apply(keyFrame(40, 200, true), set, struck);
    CHECK(hitAt(struck, 40));                       // a loud strike counts on its own frame
    loud.apply(keyFrame(40, 200, false), set, struck);
    CHECK_FALSE(hitAt(struck, 40));                 // and is not carried, since its key already counted
}
