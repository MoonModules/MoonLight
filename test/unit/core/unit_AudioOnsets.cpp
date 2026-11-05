/// @module AudioService

/// The onsets per range, per band and per note, read from the banks every 5 ms: a pitched drum fires its own band, timed at its start once the band's delay is subtracted, and a new note its own key.

#include "doctest.h"
#include "core/services/AudioHarmony.h"   // the semitone bank and its first key
#include "core/services/AudioOnsets.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace {

constexpr uint32_t kRate = 24000;
constexpr size_t kChunk = mm::ThirdOctaveBank::kChunk;

struct Tom { float hz; uint32_t atMs; };

// The room's hiss plus `signal(seconds)` through `bank` a chunk at a time, as the analysis reads it, until `untilMs` of stream time; `each(nowMs)` after every chunk.
template <typename Bank, typename Signal, typename Each>
void feed(Bank& bank, uint32_t rate, uint32_t untilMs, Signal signal, Each each) {
    uint32_t seed = 7;
    float x[kChunk];
    for (size_t at = 0; at * 1000 < static_cast<size_t>(untilMs) * rate; at += kChunk) {
        for (size_t i = 0; i < kChunk; i++) {
            seed = seed * 1664525u + 1013904223u;
            x[i] = static_cast<float>(static_cast<double>(static_cast<int32_t>(seed) >> 24) + signal(static_cast<double>(at + i) / rate));
        }
        bank.process(x);
        each(static_cast<uint32_t>((at + kChunk) * 1000 / rate));
    }
}

// A struck sound's envelope `u` seconds in: a raised-cosine attack of `attackS`, then full.
double attack(double u, double attackS) { return u < attackS ? 0.5 * (1.0 - std::cos(std::numbers::pi * u / attackS)) : 1.0; }

// Toms through the bank, each a decaying sine with a 2 ms soft attack over the room's hiss, read a chunk at a time as the analysis reads it; answers when each tom's own band's hit started, 0 where it did not fire in the 100 ms after its tom.
std::vector<uint32_t> playToms(const std::vector<Tom>& toms, const std::vector<uint8_t>& ownBands, uint32_t untilMs) {
    static mm::ThirdOctaveBank bank;
    static mm::AudioOnsets onsets;
    bank.init(kRate);
    onsets.reset();
    std::vector<uint32_t> started(toms.size(), 0);
    const auto signal = [&](double s) {
        double v = 0;
        for (const Tom& t : toms)
            if (const double u = s - t.atMs / 1000.0; u >= 0) v += 4.0e5 * attack(u, 0.002) * std::exp(-u * 15.0) * std::sin(2.0 * std::numbers::pi * t.hz * u);
        return v;
    };
    feed(bank, kRate, untilMs, signal, [&](uint32_t nowMs) {
        onsets.update(bank, nowMs);
        mm::AudioFrame f;
        onsets.take(f, true);
        for (size_t k = 0; k < toms.size(); k++)
            if (nowMs >= toms[k].atMs && nowMs <= toms[k].atMs + 100 && mm::hitAt(f.thirdsHits, ownBands[k]) && !started[k])
                started[k] = onsets.startMs(ownBands[k]);
    });
    return started;
}

}  // namespace

// Step 5 of the audio plan: a tom fill reads as a sweep across the bands, each hit timed at its start. The stick's attack fires the faster bands above too, as a real tom's does.
TEST_CASE("three toms a fourth apart fire three bands in order, each timed within 15 ms of its hit") {
    // A fill from the high tom down, 150 ms apart, each in its own third: 169 Hz in band 9, 127 in 8, 95 in 7.
    const std::vector<Tom> fill = {{169.0f, 500}, {127.0f, 650}, {95.0f, 800}};
    const auto started = playToms(fill, {9, 8, 7}, 1000);
    for (size_t k = 0; k < fill.size(); k++) {
        CAPTURE(k);
        CHECK(started[k] > 0);
        CHECK(std::abs(static_cast<int>(started[k]) - static_cast<int>(fill[k].atMs)) <= 15);
        if (k > 0) CHECK(started[k] > started[k - 1]);
    }
}

namespace {

using NoteOnsets = mm::BandOnsets<79, mm::kNotesLowBand, true>;

// Plucked notes through the semitone bank, each its fundamental and two harmonics with a 2 ms soft attack, decaying slowly; answers, per key, whether it fired in the 100 ms after `windowMs`.
std::vector<bool> keysFiredAfter(const std::vector<Tom>& notes, uint32_t windowMs) {
    static mm::SemitoneBank bank;
    static NoteOnsets onsets;
    bank.init(kRate, 440.0f, mm::kSemitoneTopHz);
    onsets.reset();
    std::vector<bool> fired(79, false);
    const auto signal = [&](double s) {
        double v = 0;
        for (const Tom& n : notes)
            if (const double u = s - n.atMs / 1000.0; u >= 0)
                for (int k = 1; k <= 3; k++) v += 3.0e5 / (k * k) * attack(u, 0.002) * std::exp(-u * 2.0 * k) * std::sin(2.0 * std::numbers::pi * n.hz * k * u);
        return v;
    };
    feed(bank, kRate, windowMs + 100, signal, [&](uint32_t nowMs) {
        onsets.update(bank, nowMs);
        uint8_t hits[NoteOnsets::kBytes];
        onsets.take(hits, true);
        if (nowMs >= windowMs) for (uint8_t k = 0; k < 79; k++) fired[k] = fired[k] || mm::hitAt(hits, k);
    });
    return fired;
}

}  // namespace

// Step 5 of the audio plan: a new note starts where its key's band rises, the held one under it staying quiet.
TEST_CASE("a note plucked over a held one fires its own key and not the held one's") {
    // C4 held from 300 ms, E4 plucked at 800: keys 39 and 43 from A0.
    const auto fired = keysFiredAfter({{261.63f, 300}, {329.63f, 800}}, 800);
    CHECK(fired[43]);
    CHECK_FALSE(fired[39]);
    CHECK(fired[55]);    // E5, its second harmonic, a partial starting too
}

namespace {

struct KickTimes { uint32_t clickMs = 0, lowMs = 0, startMs = 0; };

// A kick at 500 ms through the bank at `rate`: a decaying 60 Hz with a 5 ms soft attack, `withBass`, and a beater's 3 ms click at 3 kHz, `withClick`, over the room's hiss; answers when the treble and the low onset first fired and when the kick started.
KickTimes kickAt(uint32_t rate, bool withBass, bool withClick) {
    static mm::ThirdOctaveBank bank;
    static mm::AudioOnsets onsets;
    bank.init(rate);
    onsets.reset();
    KickTimes times;
    const auto signal = [&](double s) {
        const double u = s - 0.5;
        double v = 0;
        if (u >= 0 && withBass) v += 4.0e5 * attack(u, 0.005) * std::exp(-u * 12.0) * std::sin(2.0 * std::numbers::pi * 60.0 * u);
        if (u >= 0 && u < 0.003 && withClick) v += 1.5e5 * std::sin(std::numbers::pi * u / 0.003) * std::sin(2.0 * std::numbers::pi * 3000.0 * u);
        return v;
    };
    feed(bank, rate, 700, signal, [&](uint32_t nowMs) {
        onsets.update(bank, nowMs);
        mm::AudioFrame f;
        onsets.take(f, true);
        if (nowMs < 500) return;
        if (f.onsetHigh && !times.clickMs) times.clickMs = nowMs;
        if (f.onsetLow && !times.lowMs) { times.lowMs = nowMs; times.startMs = onsets.kickStartMs(); }
    });
    return times;
}

// A tone burst of `burstMs` every `periodMs` at `hz` from 300 ms on; answers how many hits its own third fired.
int burstHits(float hz, uint8_t band, uint32_t periodMs, uint32_t burstMs, int bursts) {
    static mm::ThirdOctaveBank bank;
    static mm::AudioOnsets onsets;
    bank.init(kRate);
    onsets.reset();
    int hits = 0;
    const auto signal = [&](double s) {
        const double u = s - 0.3;
        const bool on = u >= 0 && u * 1000.0 < periodMs * bursts && std::fmod(u * 1000.0, periodMs) < burstMs;
        return on ? 3.0e5 * std::sin(2.0 * std::numbers::pi * hz * s) : 0.0;
    };
    feed(bank, kRate, 300 + periodMs * static_cast<uint32_t>(bursts) + 100, signal, [&](uint32_t nowMs) {
        onsets.update(bank, nowMs);
        mm::AudioFrame f;
        onsets.take(f, true);
        if (mm::hitAt(f.thirdsHits, band)) hits++;
    });
    return hits;
}

}  // namespace

// Step 5 of the audio plan: the beater's click rises within a millisecond or two, the bass tens of them, so the click times the kick and the bass confirms it.
TEST_CASE("a kick with its click fires the treble first, then the low onset, timed within 5 ms of its start") {
    const auto t = kickAt(kRate, true, true);
    REQUIRE(t.clickMs > 0);
    REQUIRE(t.lowMs > 0);
    CHECK(t.clickMs < t.lowMs);
    CHECK(t.startMs >= 495);
    CHECK(t.startMs <= 505);
}

// The bass confirms the kick: a click whose bass never rises is a hi-hat's, or a stick on a rim.
TEST_CASE("a click without a bass under it fires the treble and no kick") {
    const auto t = kickAt(kRate, false, true);
    CHECK(t.clickMs > 0);
    CHECK(t.lowMs == 0);
}

// At 8 kHz the bank stops at 3.15 kHz, so two of the click's five thirds are missing and the kick still reads.
TEST_CASE("at 8 kHz, with fewer bands, a kick with its click still fires the low onset") {
    const auto t = kickAt(8000, true, true);
    CHECK(t.lowMs > 0);
}

// Strokes at 10 a second stay separate hits; a faster roll merges (measured 2026-11-05: 2 of 10 at 20 a second), the plan's backlog.
TEST_CASE("a roll of ten strokes at 10 a second fires its band ten times") {
    CHECK(burstHits(1000.0f, 17, 100, 10, 10) >= 10);
}
