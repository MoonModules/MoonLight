/// @module AudioService
/// @also AudioSpectrumEffect

#include "doctest.h"
#include "core/services/AudioBands.h"

#include <cmath>
#include <cstring>   // std::memcpy: clang finds it transitively, GCC does not (CI's sanitizer builds)
#include <numbers>
#include <utility>
#include <vector>

// The success spec for the frequency path, host-side: synthesize a sine, feed the third-octave bank, read the sixteen bands and the peak through bankToBands, and assert the energy lands in the right band and the peak frequency tracks the tone.

namespace {

uint8_t thirdsScratch[31];   // the 31 a case does not read

constexpr uint32_t kRate = 48000;     // the default sample rate
constexpr float kAmp = 16384.0f;      // a tone at -54 dBFS, 24-bit samples shifted to their magnitude

// One bank for every case: its filter state belongs off a test's stack.
mm::ThirdOctaveBank& freshBankKeep() {
    static mm::ThirdOctaveBank bank;
    return bank;
}

mm::ThirdOctaveBank& freshBank() {
    auto& bank = freshBankKeep();
    bank.init(kRate);
    return bank;
}

// Dynamics that learned a quiet room at -100 dBFS, so a tone opens the gate.
mm::AudioDynamics quietRoom() {
    mm::AudioDynamics d;
    for (int i = 0; i < 50; i++) d.update(-100.0f, -106.0f, 10.0f);
    return d;
}

// Half a second of a sine at `freqHz` through the bank, then the frame fields of interest after the window's top settled on it.
void analyse(float freqHz, uint8_t (&bands)[16], uint16_t& peakHz, uint16_t& peakMag, float amp = kAmp) {
    auto& bank = freshBank();
    const double step = 2.0 * std::numbers::pi * static_cast<double>(freqHz) / kRate;
    for (uint32_t i = 0; i < kRate / 2; i++) bank.push(amp * static_cast<float>(std::sin(step * i)));
    mm::AudioDynamics dyn = quietRoom();
    const float levelDb = amp > 0.0f ? 20.0f * std::log10(amp / 8388608.0f) : -120.0f;
    for (int i = 0; i < 20; i++) mm::bankToBands(bank, levelDb, 10.0f, dyn, thirdsScratch, bands, peakHz, peakMag);
}

// Index of the loudest band.
int dominantBand(const uint8_t (&bands)[16]) {
    int best = 0;
    for (int b = 1; b < 16; b++) if (bands[b] > bands[best]) best = b;
    return best;
}

} // namespace

TEST_CASE("AudioBands: silence yields all-zero bands and no peak") {
    uint8_t bands[16];
    uint16_t peakHz = 9999, peakMag = 9999;
    analyse(1000.0f, bands, peakHz, peakMag, 0.0f);
    for (int b = 0; b < 16; b++) CHECK(bands[b] == 0);
    CHECK(peakHz == 0);
    CHECK(peakMag == 0);
}

TEST_CASE("AudioBands: a low tone lands in a low band, a high tone in a high band") {
    uint8_t lo[16], hi[16];
    uint16_t hz, mag;
    analyse(125.0f, lo, hz, mag);
    analyse(6300.0f, hi, hz, mag);
    CHECK(dominantBand(lo) == 3);    // the 125 Hz band
    CHECK(dominantBand(hi) == 14);   // the 6.3 kHz band
}

void checkPeak(float f) {
    CAPTURE(f);
    uint8_t bands[16];
    uint16_t peakHz, peakMag;
    analyse(f, bands, peakHz, peakMag);
    REQUIRE(peakMag > 0);
    CHECK(std::abs(static_cast<float>(peakHz) / f - 1.0f) < 0.04f);   // within 4%, a sixth of a band
}

// The peak is the loudest band refined between its neighbors, so a tone between two centers still reads close to where it sits.
TEST_CASE("AudioBands: the reported peak frequency tracks the played tone, between band centers too") {
    for (float f : {150.0f, 440.0f, 1000.0f, 1100.0f, 3000.0f, 6000.0f}) checkPeak(f);
}

TEST_CASE("AudioBands: a single tone concentrates energy, not smears it everywhere") {
    uint8_t bands[16];
    uint16_t peakHz, peakMag;
    analyse(2000.0f, bands, peakHz, peakMag);
    const int dom = dominantBand(bands);
    int sum = 0;
    for (int b = 0; b < 16; b++) sum += bands[b];
    CHECK(bands[dom] > (sum / 16) * 2);
}

TEST_CASE("AudioBands: a bank with no bands yields silence, not a crash") {
    static mm::ThirdOctaveBank bank;
    bank.init(0);
    uint8_t bands[16];
    uint16_t peakHz = 1, peakMag = 1;
    mm::AudioDynamics dyn = quietRoom();
    mm::bankToBands(bank, -40.0f, 10.0f, dyn, thirdsScratch, bands, peakHz, peakMag);
    CHECK(peakHz == 0);
    CHECK(peakMag == 0);
}

// The sixteen display bands are WLED-MM's default GEQ bars, so two boards side by side draw the same picture and a WLED receiver reads the same sixteen from either.
TEST_CASE("the sixteen bands sit on WLED-MM's default GEQ centers") {
    const float wledMm[16] = {39.81f, 63.10f, 79.43f, 125.89f, 158.49f, 251.19f, 398.11f, 501.19f,
                              794.33f, 1000.0f, 1584.89f, 1995.26f, 3162.28f, 5011.87f, 6309.57f, 10000.0f};
    const auto& bank = freshBank();
    for (uint8_t b = 0; b < 16; b++) CHECK(std::abs(bank.centerHz(mm::kSixteenOfThirds[b]) / wledMm[b] - 1.0f) < 0.025f);
}

// The BALLISTIC of a band: fast attack, slow decay, as broadcast meters (PPM, IEC 60268-10) do, so a drum hit is not rounded off by a decay-speed rise.

TEST_CASE("a band rises to a transient at once and falls back slowly, the PPM ballistic") {
    uint8_t v = 0;
    // A hit: one block takes it most of the way up, because a drum must not be smoothed away.
    v = mm::ballistic(v, 200, /*rise*/ 200, /*fall*/ 24);
    CHECK(v > 150);
    const uint8_t afterRise = v;
    // Silence after it: the fall is gradual, so the bar decays rather than dropping out.
    v = mm::ballistic(v, 0, 200, 24);
    CHECK(v < afterRise);
    CHECK(v > (afterRise * 3) / 4);    // one block takes off a tenth: the bar decays, it does not drop
    for (int i = 0; i < 60; i++) v = mm::ballistic(v, 0, 200, 24);
    CHECK(v == 0);                     // and it does reach zero rather than sticking just above it
}

TEST_CASE("the ballistic reaches its target exactly, so a held level does not sit one short") {
    uint8_t v = 0;
    for (int i = 0; i < 60; i++) v = mm::ballistic(v, 255, 200, 24);
    CHECK(v == 255);
    for (int i = 0; i < 200; i++) v = mm::ballistic(v, 0, 200, 24);
    CHECK(v == 0);
}

TEST_CASE("equal rise and fall reduce to a symmetric follower, so the ballistic is a superset") {
    for (uint8_t rate : {uint8_t(8), uint8_t(64), uint8_t(200)}) {
        uint8_t a = 40, b = 40;
        for (int i = 0; i < 10; i++) {
            a = mm::ballistic(a, 200, rate, rate);
            b = mm::smoothFollow(b, 200, rate);
            CHECK(a == b);
        }
    }
}

TEST_CASE("every band gets its own ballistic, so a hit in the bass does not smooth the treble") {
    uint8_t raw[16] = {}, sm[16] = {};
    raw[0] = 255;                                  // a bass hit, treble silent
    mm::smoothBands(raw, sm);
    CHECK(sm[0] > 150);                            // the hit shows in one block
    for (uint8_t b = 1; b < 16; b++) CHECK(sm[b] == 0);   // and touches no other band
    raw[0] = 0;                                    // silence
    mm::smoothBands(raw, sm);
    CHECK(sm[0] > 100);                            // the bar is still falling, not gone
    CHECK(sm[0] < 200);
}

// Onset detection by SPECTRAL FLUX (Bello 2005, Dixon 2006): the sum over bands of the positive change since the last block. A spectrum-wide rise is a hit; a fall or a steady tone is not.

TEST_CASE("spectral flux reads a rise, ignores a fall, and is zero on a steady spectrum") {
    uint8_t prev[16] = {}, cur[16] = {};
    CHECK(mm::spectralFlux(prev, cur) == 0);               // silence to silence
    for (uint8_t b = 0; b < 16; b++) cur[b] = 200;
    const uint8_t hit = mm::spectralFlux(prev, cur);        // everything rose
    CHECK(hit > 150);
    std::memcpy(prev, cur, 16);
    CHECK(mm::spectralFlux(prev, cur) == 0);               // held: no flux
    for (uint8_t b = 0; b < 16; b++) cur[b] = 0;
    CHECK(mm::spectralFlux(prev, cur) == 0);               // a fall is not an onset
}

TEST_CASE("an onset fires once per hit, not once per block the hit lasts, and not on a swell") {
    // A hit is flux well above its own recent average; a refractory window makes one hit one onset. A slow swell raises the average with it and never exceeds it enough to fire.
    mm::OnsetDetector d;
    int onsets = 0;
    for (int block = 0; block < 200; block++) {
        const bool hitBlock = (block % 20 == 0);            // a hit every 20 blocks (~half a second)
        const uint8_t flux = hitBlock ? 200 : 5;            // background flux between hits
        if (d.feed(flux, static_cast<uint32_t>(block) * 23u)) onsets++;
    }
    CHECK(onsets == 10);                                    // ten hits, ten onsets
    mm::OnsetDetector s;
    int swell = 0;
    for (int block = 0; block < 200; block++)               // a slow linear swell
        if (s.feed(static_cast<uint8_t>(block / 2), static_cast<uint32_t>(block) * 23u)) swell++;
    CHECK(swell <= 1);                                      // the first block may fire; nothing after
}

// Flux is a difference against the PREVIOUS block and the onset detector carries a running mean, so a restarted source must not measure its first block against the old source's last: AudioService::deinit clears both with the frame.
TEST_CASE("a restarted source reports no onset from the block that preceded it") {
    // The history the old source left behind: a loud spectrum.
    uint8_t prev[16], now[16];
    for (uint8_t b = 0; b < 16; b++) { prev[b] = 200; now[b] = 200; }
    CHECK(mm::spectralFlux(prev, now) == 0);            // steady: no flux, by definition

    // Cleared history reads the same block as a full-scale RISE, which is why deinit also publishes a silent frame. Clearing gives a DEFINED reference for the block after it.
    uint8_t cleared[16] = {};
    const uint16_t againstStale = mm::spectralFlux(prev, now);
    const uint16_t againstCleared = mm::spectralFlux(cleared, now);
    CHECK(againstStale == 0);
    CHECK(againstCleared > againstStale);   // which is why the frame is published silent too
}

// Step 4 of the audio plan: the bass, the mids and the treble, each its range's power per band on the bands' scale, so an effect keyed on the bass finds the kick whichever third it lands in.
TEST_CASE("a low tone lights the bass, a middle one the mids, a high one the treble") {
    for (const auto& [hz, range] : {std::pair{63.0f, 0}, std::pair{1000.0f, 1}, std::pair{6300.0f, 2}}) {
        CAPTURE(hz);
        uint8_t bands[16];
        uint16_t peakHz, peakMag;
        analyse(hz, bands, peakHz, peakMag);
        mm::AudioDynamics dyn = quietRoom();
        const float levelDb = 20.0f * std::log10(kAmp / 8388608.0f);
        mm::AudioFrame f;
        for (int i = 0; i < 20; i++) f.thirdsCount = mm::bankToBands(freshBankKeep(), levelDb, 10.0f, dyn, f.thirds, f.bands, peakHz, peakMag);
        mm::bandsToRanges(f);
        const uint8_t r[3] = {f.bass, f.mid, f.treble};
        for (int o = 0; o < 3; o++) if (o != range) CHECK(r[range] > r[o]);
    }
}

// A frame from audio sync carries the sixteen alone, and its ranges come from them, so an effect keyed on the bass follows a sender too.
TEST_CASE("a frame with only the sixteen still carries the bass, mids and treble") {
    mm::AudioFrame f;
    f.thirdsCount = 0;
    f.bands[0] = 200;    // 40 Hz: bass
    f.bands[9] = 120;    // 1 kHz: mids
    f.bands[15] = 60;    // 10 kHz: treble
    mm::bandsToRanges(f);
    CHECK(f.bass == 200);
    CHECK(f.mid == 120);
    CHECK(f.treble == 60);
}
