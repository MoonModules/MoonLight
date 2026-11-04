/// @module AudioService

/// The fractional-octave filter bank measures the bands of IEC 61260-1, each the power through its own filter: at three to the octave a tone reads its own band, the neighbors well below, and pink noise reads flat; at twelve the bands are the notes.

#include "doctest.h"
#include "core/services/FractionalOctaveBank.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

using Thirds = mm::FractionalOctaveBank<3>;

constexpr uint32_t kRate = 48000;
constexpr size_t kBlock = 512;

// One bank for every case: its filter state is several kilobytes, which belong off a test's stack.
Thirds& freshBank(uint32_t rate = kRate) {
    static Thirds bank;
    bank.init(rate);
    return bank;
}

float db(float power) { return 10.0f * std::log10(power > 1e-30f ? power : 1e-30f); }

// Each band's power averaged over the reads after `settleSeconds`, one read per block, as AudioService reads it.
std::vector<float> measure(Thirds& bank, const std::vector<float>& x, float settleSeconds) {
    std::vector<float> sum(Thirds::kBands, 0.0f);
    const size_t settle = static_cast<size_t>(settleSeconds * static_cast<float>(kRate));
    size_t reads = 0;
    for (size_t at = 0; at + kBlock <= x.size(); at += kBlock) {
        bank.process({x.data() + at, kBlock});
        if (at < settle) continue;
        for (uint8_t b = 0; b < Thirds::kBands; b++) sum[b] += bank.power(b);
        reads++;
    }
    for (auto& s : sum) s /= static_cast<float>(reads);
    return sum;
}

std::vector<float> sine(float hz, float seconds, float amplitude = 1.0f) {
    std::vector<float> x(static_cast<size_t>(seconds * static_cast<float>(kRate)));
    const double step = 2.0 * std::numbers::pi * static_cast<double>(hz) / kRate;
    for (size_t i = 0; i < x.size(); i++) x[i] = amplitude * static_cast<float>(std::sin(step * static_cast<double>(i)));
    return x;
}

// The unit contract: a tone at a band's center reads its own power there, A * A / 2, and its neighbors well below; in the top octave, squeezed toward Nyquist, the 20 kHz band hears a 16 kHz tone 12 dB down.
void checkTone(Thirds& bank, uint8_t b) {
    CAPTURE(b);
    bank.reset();
    const auto p = measure(bank, sine(bank.centerHz(b), 1.5f), 1.0f);
    CHECK(std::abs(db(p[b]) - db(0.5f)) < 0.5f);
    const float margin = b + 1 >= 28 ? 11.5f : 16.0f;   // a pair reaching into the top octave, 12.7 kHz up
    if (b > 0) CHECK(db(p[b]) - db(p[b - 1]) > margin);
    if (b + 1 < Thirds::kBands) CHECK(db(p[b]) - db(p[b + 1]) > margin);
}

// Pink noise is the spectrum real music resembles, and band power reads it flat: equal-amplitude tones at log spacing are pink, 24 to the octave so each band holds eight.
std::vector<float> pinkMultisine() {
    std::vector<float> x(static_cast<size_t>(2.0f * kRate), 0.0f);
    uint32_t seed = 12345;
    for (int m = -18 * 8 - 4; m <= 13 * 8 + 4; m++) {
        const double hz = 1000.0 * std::exp2((m + 0.5) / 24.0);
        if (hz > 0.47 * kRate) break;
        seed = seed * 1664525u + 1013904223u;   // a fixed phase per tone, so the crest stays bounded
        const double phase = 2.0 * std::numbers::pi * (seed >> 8) / 16777216.0;
        const double step = 2.0 * std::numbers::pi * hz / kRate;
        for (size_t i = 0; i < x.size(); i++) x[i] += 0.02f * static_cast<float>(std::sin(step * static_cast<double>(i) + phase));
    }
    return x;
}

void checkFlat(const std::vector<float>& p, uint8_t b) {
    CAPTURE(b);
    CHECK(std::abs(db(p[b]) - db(p[17])) < 1.0f);   // against 1 kHz
}

// How long a band takes to fall 20 dB once its tone stops, in milliseconds.
float decayMs(Thirds& bank, uint8_t b) {
    bank.reset();
    const auto tone = sine(bank.centerHz(b), 1.0f);
    bank.process({tone.data(), tone.size()});
    const float steady = bank.power(b);
    for (uint32_t ms = 1; ms <= 1000; ms++) {
        for (int i = 0; i < 48; i++) bank.push(0.0f);
        if (bank.power(b) < steady * 0.01f) return static_cast<float>(ms);
    }
    return 1000.0f;
}

void checkDecay(Thirds& bank, uint8_t b, float reference) {
    CAPTURE(b);
    CHECK(std::abs(decayMs(bank, b) / reference - 1.0f) < 0.25f);
}

// Each band's delay, a click to the moment its power peaks, in milliseconds: what onsets compared across bands subtract.
std::vector<int> clickDelaysMs(Thirds& bank) {
    bank.reset();
    std::vector<float> x(48 * 600, 0.0f);
    x[0] = 10000.0f;   // a click, then 600 ms of silence
    std::vector<float> peak(Thirds::kBands, 0.0f);
    std::vector<int> at(Thirds::kBands, 0);
    for (size_t ms = 0; ms < 600; ms++) {
        bank.process({x.data() + ms * 48, 48});
        for (uint8_t b = 0; b < Thirds::kBands; b++)
            if (bank.power(b) > peak[b]) { peak[b] = bank.power(b); at[b] = static_cast<int>(ms) + 1; }
    }
    return at;
}

// The decimation phase and every filter's state carry across calls, so how the samples are split between calls changes nothing.
std::vector<float> readInChunks(const std::vector<float>& x, size_t chunk) {
    auto& bank = freshBank();
    for (size_t at = 0; at < x.size(); at += chunk) bank.process({x.data() + at, std::min(chunk, x.size() - at)});
    std::vector<float> reads;
    for (uint8_t b = 0; b < Thirds::kBands; b++) reads.push_back(bank.power(b));
    return reads;
}

void checkSameRead(const std::vector<float>& a, const std::vector<float>& b, uint8_t band) {
    CAPTURE(band);
    CHECK(b[band] == doctest::Approx(a[band]).epsilon(1e-4));
}

// A semitone tone's power averaged over half a second after a second's settling, every band read.
std::vector<float> settledSemitones(mm::FractionalOctaveBank<12>& bank, float hz) {
    bank.reset();
    const auto x = sine(hz, 1.5f);
    std::vector<float> sum(mm::FractionalOctaveBank<12>::kBands, 0.0f);
    int reads = 0;
    for (size_t at = 0; at + kBlock <= x.size(); at += kBlock) {
        bank.process({x.data() + at, kBlock});
        if (at < kRate) continue;
        for (uint8_t b = 0; b < bank.bandCount(); b++) sum[b] += bank.power(b);
        reads++;
    }
    for (auto& v : sum) v /= static_cast<float>(reads);
    return sum;
}

}  // namespace

// The base-2 series the octave-by-octave bank needs, against the labels every analyzer prints.
TEST_CASE("the 31 band centers are base-2 third octaves within 2.5% of their ISO 266 labels") {
    const float iso[31] = {20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                           1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};
    const auto& bank = freshBank();
    for (uint8_t b = 0; b < 31; b++) CHECK(std::abs(bank.centerHz(b) / iso[b] - 1.0f) < 0.025f);
    CHECK(bank.centerHz(17) == doctest::Approx(1000.0f));
}

// A band whose upper edge the rate cannot carry is left out, so a lower rate measures fewer bands rather than wrong ones.
TEST_CASE("48 kHz carries all 31 bands and a lower rate drops those above its reach") {
    auto& bank = freshBank(48000);
    CHECK(bank.bandCount() == 31);
    bank.init(44100);
    CHECK(bank.bandCount() == 30);   // the 20 kHz band's upper edge, 22.6 kHz, is past 44.1 kHz's Nyquist
    bank.init(22050);
    CHECK(bank.bandCount() == 27);   // up to the 8 kHz band
    CHECK(bank.power(30) == 0.0f);
}

TEST_CASE("a tone at each band's center reads its own power, its neighbors 16 dB down and 11.5 in the top octave") {
    auto& bank = freshBank();
    for (uint8_t b = 0; b < Thirds::kBands; b++) checkTone(bank, b);
}

// Two bass notes a few semitones apart, which the 43 Hz bins of a 512-point transform put in one bin.
TEST_CASE("50 Hz and 60 Hz light different bands") {
    auto& bank = freshBank();
    const uint8_t band50 = 4, band60 = 5;   // the 50 Hz and 63 Hz bands
    const auto p50 = measure(bank, sine(50.0f, 1.5f), 1.0f);
    bank.reset();
    const auto p60 = measure(bank, sine(60.0f, 1.5f), 1.0f);
    CHECK(db(p50[band50]) - db(p50[band60]) > 10.0f);
    CHECK(db(p60[band60]) - db(p60[band50]) > 10.0f);
}

TEST_CASE("pink noise reads every band within 1 dB of flat") {
    const auto p = measure(freshBank(), pinkMultisine(), 1.0f);
    for (uint8_t b = 0; b < Thirds::kBands; b++) checkFlat(p, b);
}

// One detector time constant for every band, so the bars fall alike, about 50 ms for 20 dB; below 250 Hz a narrow filter's own ringing adds to it (measured 2026-11-03: 84 ms at 100 Hz, 184 at 31.5).
TEST_CASE("every band from 250 Hz up falls 20 dB in the same time once its tone stops") {
    auto& bank = freshBank();
    const float reference = decayMs(bank, 17);   // 1 kHz
    CHECK(reference < 60.0f);
    for (uint8_t b = 11; b < Thirds::kBands; b++) checkDecay(bank, b, reference);
}

// Delay grows toward the bass, a narrow filter and the halving low-passes both taking their time (measured 2026-11-03: 118 ms at 31.5 Hz, 42 at 100 Hz, 6 at 1 kHz, 1 from 6.3 kHz up).
TEST_CASE("a click reaches the high bands first and the bass last, within the measured delays") {
    const auto at = clickDelaysMs(freshBank());
    for (uint8_t b = 1; b < Thirds::kBands; b++) { CAPTURE(b); CHECK(at[b] <= at[b - 1]); }
    CHECK(at[2] <= 130);    // 31.5 Hz
    CHECK(at[7] <= 50);     // 100 Hz
    CHECK(at[17] <= 8);     // 1 kHz
    CHECK(at[30] <= 2);     // 20 kHz
}

TEST_CASE("the same signal in chunks of 1, 7 and 512 reads the same in every band") {
    const auto x = sine(997.0f, 0.5f);
    const auto one = readInChunks(x, 1), seven = readInChunks(x, 7), block = readInChunks(x, 512);
    for (uint8_t b = 0; b < Thirds::kBands; b++) { checkSameRead(one, seven, b); checkSameRead(one, block, b); }
}

// A low rate carries fewer bands, and the ones it carries still read their tones.
TEST_CASE("at 8 kHz the bank carries 23 bands and a 1 kHz tone reads its own") {
    auto& bank = freshBank(8000);
    CHECK(bank.bandCount() == 23);
    std::vector<float> x(8000);
    for (size_t i = 0; i < x.size(); i++) x[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / 8000.0));
    bank.process({x.data(), x.size()});
    CHECK(std::abs(db(bank.power(17)) - db(0.5f)) < 0.5f);
    CHECK(db(bank.power(17)) - db(bank.power(16)) > 15.0f);
}

// Twelve to the octave counted from A440 are the notes: the same bank, one band a semitone.
TEST_CASE("twelve bands to the octave from A440 put a band on every note, 20 Hz to 20 kHz") {
    static mm::FractionalOctaveBank<12> semitones;
    semitones.init(kRate, 440.0f);
    CHECK(semitones.bandCount() == 121);
    CHECK(semitones.centerHz(54) == doctest::Approx(440.0f));          // A4
    CHECK(semitones.centerHz(54 + 12) == doctest::Approx(880.0f));     // A5, an octave up
    CHECK(semitones.centerHz(54 - 48) == doctest::Approx(27.5f));      // A0, the piano's lowest key
    CHECK(semitones.centerHz(0) > 19.0f);
}

// Harmony needs the notes up to 2.5 kHz, and a limited bank pays only for those: it halves the rate until its top band fits.
TEST_CASE("a band limit keeps the notes under it, each reading its own power with the next semitone well below") {
    static mm::FractionalOctaveBank<12> notes;
    notes.init(kRate, 440.0f, 2500.0f);
    CHECK(notes.bandCount() == 85);                                    // up to D#7, 2489 Hz
    CHECK(notes.power(85) == 0.0f);                                    // E7 lies over the limit
    for (const uint8_t b : {uint8_t(18), uint8_t(54), uint8_t(84)}) { // A1, A4 and the top band
        CAPTURE(b);
        const auto p = settledSemitones(notes, notes.centerHz(b));
        CHECK(std::abs(db(p[b]) - db(0.5f)) < 0.5f);
        CHECK(db(p[b]) - db(p[b - 1]) > 15.0f);
        if (b + 1 < notes.bandCount()) CHECK(db(p[b]) - db(p[b + 1]) > 15.0f);
    }
}

// Silence in, silence out, after any input.
TEST_CASE("reset returns every band to silence") {
    auto& bank = freshBank();
    const auto x = sine(1000.0f, 0.1f);
    bank.process({x.data(), x.size()});
    CHECK(bank.power(17) > 0.0f);
    bank.reset();
    for (uint8_t b = 0; b < Thirds::kBands; b++) CHECK(bank.power(b) == 0.0f);
}
