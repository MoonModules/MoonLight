/// @module AudioService

/// The harmony from the semitone bank: the notes of each name over every octave fold into twelve pitch classes, so a chord reads as its notes in whatever octave it is played, and a silent room reads none.

#include "doctest.h"
#include "core/services/AudioHarmony.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <numbers>
#include <tuple>
#include <vector>

namespace {

constexpr uint32_t kRate = 24000;   // the default rate
constexpr size_t kBlock = 512;

// One bank for every case: its filter state belongs off a test's stack.
mm::SemitoneBank& freshNotesKeep() {
    static mm::SemitoneBank bank;
    return bank;
}

mm::SemitoneBank& freshNotes() {
    auto& bank = freshNotesKeep();
    bank.init(kRate, 440.0f, mm::kSemitoneTopHz);
    return bank;
}

constexpr float kBlockMs = 1000.0f * kBlock / kRate;

// These tones together at the same amplitude, from `t` for `seconds`, through the bank and the tracker; answers how many changes fired.
int play(mm::HarmonyTracker& tracker, std::initializer_list<float> hz, double& t, float seconds, uint8_t (&chroma)[12], bool gateOpen = true) {
    auto& bank = freshNotesKeep();
    int changes = 0;
    float x[kBlock];
    for (float done = 0; done < seconds * 1000.0f; done += kBlockMs) {
        for (size_t i = 0; i < kBlock; i++) {
            x[i] = 0.0f;
            for (const float f : hz) x[i] += 1000.0f * static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(f) * (t + static_cast<double>(i) / kRate)));
        }
        t += static_cast<double>(kBlock) / kRate;
        bank.process({x, kBlock});
        if (tracker.update(bank, gateOpen, kBlockMs, chroma)) changes++;
    }
    return changes;
}

// The twelve after a second of these tones together.
void chromaOf(std::initializer_list<float> hz, uint8_t (&chroma)[12], bool gateOpen = true) {
    freshNotes();
    mm::HarmonyTracker tracker;
    double t = 0;
    play(tracker, hz, t, 1.0f, chroma, gateOpen);
}

// Whether the named classes are exactly the strongest ones.
bool strongest(const uint8_t (&chroma)[12], std::initializer_list<uint8_t> classes) {
    uint8_t weakestNamed = 255;
    for (const uint8_t c : classes) weakestNamed = chroma[c] < weakestNamed ? chroma[c] : weakestNamed;
    for (uint8_t c = 0; c < 12; c++) {
        bool named = false;
        for (const uint8_t n : classes) named = named || n == c;
        if (!named && chroma[c] >= weakestNamed) return false;
    }
    return true;
}

constexpr uint8_t kC = 0, kE = 4, kG = 7, kA = 9;
constexpr std::initializer_list<float> kCMajor = {261.63f, 329.63f, 392.0f}, kAMinor = {220.0f, 261.63f, 329.63f}, kGMajor = {196.0f, 246.94f, 293.66f}, kFMajor = {174.61f, 220.0f, 261.63f};

}  // namespace

TEST_CASE("a semitone band's pitch class is its note's name, the same an octave up") {
    CHECK(mm::pitchClassOf(mm::kA4Band) == kA);            // A4, 440 Hz
    CHECK(mm::pitchClassOf(mm::kA4Band - 9) == kC);        // C4, middle C
    CHECK(mm::pitchClassOf(mm::kA4Band + 12) == kA);       // A5
    CHECK(mm::pitchClassOf(mm::kChromaLowBand) == kC);     // C2, the lowest folded in
    CHECK(freshNotes().centerHz(mm::kA4Band) == doctest::Approx(440.0f));
}

TEST_CASE("a C major triad lights C, E and G, the loudest of them at 255") {
    uint8_t chroma[12];
    chromaOf({261.63f, 329.63f, 392.0f}, chroma);   // C4, E4, G4
    CHECK(strongest(chroma, {kC, kE, kG}));
    CHECK((chroma[kC] == 255 || chroma[kE] == 255 || chroma[kG] == 255));
    for (const uint8_t c : {kC, kE, kG}) CHECK(chroma[c] > 200);   // three equal tones read alike
}

TEST_CASE("a note reads as its pitch class in any octave") {
    for (const float hz : {130.81f, 523.25f, 2093.0f}) {   // C3, C5, C7
        CAPTURE(hz);
        uint8_t chroma[12];
        chromaOf({hz}, chroma);
        CHECK(chroma[kC] == 255);
        CHECK(strongest(chroma, {kC}));
    }
}

TEST_CASE("the twelve read 0 while the gate is shut, so a silent room shows no harmony") {
    uint8_t chroma[12];
    chromaOf({261.63f, 329.63f, 392.0f}, chroma, false);
    for (const uint8_t v : chroma) CHECK(v == 0);
}

TEST_CASE("the notes readout names the three strongest classes, strongest first") {
    uint8_t chroma[12] = {};
    chroma[kE] = 255;
    chroma[kC] = 200;
    chroma[kG] = 130;
    chroma[kA] = 100;   // the fourth: not named
    char out[32];
    mm::formatNotes(chroma, out, sizeof(out));
    CHECK(std::strcmp(out, "E C G") == 0);
    char shortOut[4];
    mm::formatNotes(chroma, shortOut, sizeof(shortOut));
    CHECK(std::strcmp(shortOut, "E C") == 0);   // the weakest fall off a short buffer
    const uint8_t silent[12] = {};
    mm::formatNotes(silent, out, sizeof(out));
    CHECK(std::strcmp(out, "-") == 0);
}

// Harte, Sandler and Gasser's detector: the twelve as a point on the circles of fifths and thirds, a change being that point moving fast.
TEST_CASE("C major to A minor fires one harmonic change, near the switch") {
    freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    CHECK(play(tracker, kCMajor, t, 2.0f, chroma) == 0);   // the gate opening and the chord settling fire nothing
    CHECK(play(tracker, kAMinor, t, 0.3f, chroma) == 1);   // within 300 ms of the switch
    CHECK(play(tracker, kAMinor, t, 1.7f, chroma) == 0);
    CHECK(strongest(chroma, {kA, kC, kE}));
}

TEST_CASE("a held chord fires no harmonic change") {
    freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    CHECK(play(tracker, kCMajor, t, 4.0f, chroma) == 0);
}

TEST_CASE("C, G, A minor and F fire three changes, one a chord") {
    freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    int changes = 0;
    for (const auto chord : {kCMajor, kGMajor, kAMinor, kFMajor}) changes += play(tracker, chord, t, 1.0f, chroma);
    CHECK(changes == 3);
}

TEST_CASE("a shut gate forgets the harmony, and reopening fires no change for music starting") {
    freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    play(tracker, kCMajor, t, 1.0f, chroma);
    play(tracker, kCMajor, t, 0.5f, chroma, false);
    for (const uint8_t v : chroma) CHECK(v == 0);
    CHECK(play(tracker, kAMinor, t, 2.0f, chroma) == 0);   // a new chord after the gate opened is music starting, not a change
}

TEST_CASE("the notes are the piano's keys from A0, a tone lighting its own key the most") {
    auto& bank = freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    play(tracker, {440.0f}, t, 1.0f, chroma);
    mm::AudioDynamics dyn;
    for (int i = 0; i < 50; i++) dyn.update(-100.0f, -106.0f, kBlockMs);   // a quiet room learned
    dyn.update(-40.0f, -40.0f, kBlockMs);
    for (int i = 0; i < 50; i++) dyn.update(-40.0f, mm::bandPowerDbFs(bank.power(mm::kA4Band)), kBlockMs);
    uint8_t notes[mm::kAudioNoteKeys];
    CHECK(mm::semitonesToNotes(bank, dyn, notes) == mm::kAudioNoteKeys);   // A0 to D#7
    int loudest = 0;
    for (int i = 1; i < 79; i++) if (notes[i] > notes[loudest]) loudest = i;
    CHECK(loudest == 48);                                   // A4, four octaves over A0
}

// Krumhansl-Schmuckler key finding: the twelve averaged over 15 s against the major and minor profiles at every tonic.
namespace {

// `seconds` of these chords in turn, a second each, the chord's notes at 255 and the rest silent; answers the key.
uint8_t keyOf(mm::KeyTracker& k, std::initializer_list<std::initializer_list<uint8_t>> chords, float seconds) {
    uint8_t key = 0;
    for (float t = 0; t < seconds * 1000.0f; t += kBlockMs) {
        const auto& chord = *(chords.begin() + static_cast<size_t>(t / 1000.0f) % chords.size());
        uint8_t chroma[12] = {};
        for (const uint8_t c : chord) chroma[c] = 255;
        key = k.update(chroma, true, kBlockMs);
    }
    return key;
}

}  // namespace

TEST_CASE("C, F, G and C read as C major, after 10 s and not before") {
    mm::KeyTracker k;
    CHECK(keyOf(k, {{0, 4, 7}, {5, 9, 0}, {7, 11, 2}, {0, 4, 7}}, 8.0f) == 0);   // not heard long enough
    CHECK(keyOf(k, {{0, 4, 7}, {5, 9, 0}, {7, 11, 2}, {0, 4, 7}}, 12.0f) == 1);  // C major
}

TEST_CASE("A minor, D minor and E major read as A minor, the minor key beside its relative major") {
    mm::KeyTracker k;
    CHECK(keyOf(k, {{9, 0, 4}, {2, 5, 9}, {4, 8, 11}, {9, 0, 4}}, 20.0f) == 13 + 9);   // A minor
}

TEST_CASE("a pause holds the key, and one borrowed chord does not flip it") {
    mm::KeyTracker k;
    keyOf(k, {{0, 4, 7}, {5, 9, 0}, {7, 11, 2}}, 20.0f);
    REQUIRE(k.key == 1);
    uint8_t silent[12] = {};
    for (int b = 0; b < 200; b++) k.update(silent, false, kBlockMs);
    CHECK(k.key == 1);
    CHECK(keyOf(k, {{10, 2, 5}}, 2.0f) == 1);   // a B-flat major chord borrowed for two seconds
}

TEST_CASE("the key reads as its name, long and short") {
    char out[12];
    mm::formatKey(1, out, sizeof(out));
    CHECK(std::strcmp(out, "C major") == 0);
    mm::formatKey(22, out, sizeof(out));
    CHECK(std::strcmp(out, "A minor") == 0);
    mm::formatKey(22, out, sizeof(out), true);
    CHECK(std::strcmp(out, "Am") == 0);
    mm::formatKey(0, out, sizeof(out));
    CHECK(std::strcmp(out, "-") == 0);
}

// The tuning: where each strong note's power sits between its neighbors, which reads three quarters of the offset and of its sign.
TEST_CASE("a tone 40 cents sharp reads sharp, one in tune reads in tune") {
    for (const auto& [cents, low, high] : {std::tuple{40.0f, 25.0f, 45.0f}, std::tuple{0.0f, -3.0f, 3.0f}, std::tuple{-20.0f, -25.0f, -10.0f}}) {
        CAPTURE(cents);
        auto& bank = freshNotes();
        mm::TuningEstimator est;
        const double hz = 440.0 * std::exp2(static_cast<double>(cents) / 1200.0);
        float x[kBlock];
        double t = 0;
        float reading = 0;
        for (size_t b = 0; b < 3 * kRate / kBlock; b++) {
            for (size_t i = 0; i < kBlock; i++) x[i] = 1000.0f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * (t + static_cast<double>(i) / kRate)));
            t += static_cast<double>(kBlock) / kRate;
            bank.process({x, kBlock});
            reading = est.update(bank, true, kBlockMs);
        }
        CHECK(reading > low);
        CHECK(reading < high);
    }
}

// Faraldo's profiles from electronic dance music sit beside the classical pair: a track shaped as his minor profile reads as that minor key at a near-perfect match.
TEST_CASE("a track shaped like Faraldo's dance-music minor profile reads as that key") {
    mm::KeyTracker k;
    const float minor[12] = {1.00f, 0.31f, 0.44f, 0.58f, 0.33f, 0.49f, 0.29f, 0.78f, 0.43f, 0.29f, 0.53f, 0.32f};
    uint8_t chroma[12];
    for (uint8_t c = 0; c < 12; c++) chroma[(c + 7) % 12] = static_cast<uint8_t>(minor[c] * 255.0f);   // G minor
    for (float t = 0; t < 20000.0f; t += kBlockMs) k.update(chroma, true, kBlockMs);
    CHECK(k.key == 13 + 7);
    CHECK(k.strength > 0.95f);
}

// The bass line's note from the semitone bands, where a sparse mix lets it stand out of the bass range.
TEST_CASE("a bass note alone reads as its MIDI number, and none while the gate is shut") {
    auto& bank = freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    play(tracker, {110.0f}, t, 1.0f, chroma);   // A2
    CHECK(mm::semitonesToBassNote(bank, true) == 45);
    CHECK(mm::semitonesToBassNote(bank, false) == 0);
}

TEST_CASE("a bass range full of notes at once names no bass note") {
    freshNotes();
    mm::HarmonyTracker tracker;
    uint8_t chroma[12];
    double t = 0;
    std::initializer_list<float> cluster = {41.2f, 55.0f, 65.4f, 82.4f, 98.0f, 110.0f, 130.8f, 164.8f, 196.0f, 220.0f};
    play(tracker, cluster, t, 1.0f, chroma);   // ten notes as loud as each other: no line stands out
    CHECK(mm::semitonesToBassNote(freshNotesKeep(), true) == 0);
}
