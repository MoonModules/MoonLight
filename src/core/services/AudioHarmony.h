#pragma once

#include <cmath>     // amplitudes, the smoothers and the centroid's angles
#include <cstddef>
#include <cstdint>
#include <numbers>   // pi, for the centroid's circles

#include "core/services/AudioBands.h"   // SemitoneBank
#include "platform/nonblocking.h"       // MM_NONBLOCKING: the names are drawn on the render path

namespace mm {

/// The semitone bank's A4, the band 440 Hz sits on: twelve to the octave from 20 Hz put it 54 bands up.
inline constexpr uint8_t kA4Band = 54;
/// The lowest note folded in, C2 at 65 Hz: @xref{folding-the-octaves}.
inline constexpr uint8_t kChromaLowBand = kA4Band - 33;

/// The pitch class of a semitone band, 0 for C up to 11 for B.
constexpr uint8_t pitchClassOf(uint8_t band) { return static_cast<uint8_t>((band + 60 + 9 - kA4Band) % 12); }   // A is 9, and five octaves keep the sum positive

/// The harmony over time: the twelve pitch classes, C first, each the notes of that name over every octave folded together (the chromagram of music information retrieval), and the moment the harmony moves.
///
/// @technical
///
/// ## Folding the octaves
///
/// A pitch class is the sum of its notes' powers from C2 up, read as an amplitude against the loudest class and smoothed over 200 ms, so a chord's notes read high and the rest low whatever octave it is played in. The notes under C2 are left out: the kick drum sits there.
///
/// ## The moment the harmony moves
///
/// The harmonic change detector of Harte, Sandler and Gasser (2006): the twelve become a point in six dimensions, three circles of the tonnetz (fifths, minor thirds, major thirds), and a change is that point moving fast, measured against itself 100 ms behind.
///
/// A held chord moves it under 0.02, noise included; C major to A minor, two notes shared, moves it 0.13. A change fires as the motion crosses 0.06 and re-arms under 0.03.
///
/// ## The gate holds the harmony
///
/// The twelve read 0 while the gate is shut, so a silent room shows no harmony made of its noise. The first 500 ms after the gate opens fire no change: music starting moves the point too.
struct HarmonyTracker {
    /// One block: the bank's notes folded into `chroma`, 0..255 against the loudest; answers whether the harmony moved on this block.
    bool update(const SemitoneBank& bank, bool gateOpen, float dtMs, uint8_t chroma[12]) {
        for (uint8_t c = 0; c < 12; c++) chroma[c] = 0;
        if (!gateOpen) { reset(); return false; }
        float power[12] = {};
        for (uint8_t b = kChromaLowBand; b < bank.bandCount(); b++) power[pitchClassOf(b)] += bank.power(b);
        float loudest = 0.0f;
        for (const float p : power) loudest = p > loudest ? p : loudest;
        if (loudest <= 0.0f) return false;
        const float a = 1.0f - std::exp(-dtMs / kSmoothMs);
        float top = 0.0f;
        for (uint8_t c = 0; c < 12; c++) {
            smooth_[c] += a * (std::sqrt(power[c] / loudest) - smooth_[c]);
            top = smooth_[c] > top ? smooth_[c] : top;
        }
        for (uint8_t c = 0; c < 12; c++) chroma[c] = static_cast<uint8_t>(std::lround(255.0f * smooth_[c] / top));
        return moved(dtMs);
    }

    /// Forget the harmony, as a shut gate does.
    void reset() {
        for (auto& s : smooth_) s = 0.0f;
        for (auto& b : behind_) b = 0.0f;
        openMs_ = 0.0f;
        armed_ = false;
    }

    /// The tonal centroid of twelve amplitudes: their weighted mean on the three circles, each a pair of coordinates.
    static void centroid(const float classes[12], float point[6]) {
        constexpr float pi = std::numbers::pi_v<float>;
        constexpr float kStep[3] = {7.0f * pi / 6.0f, 3.0f * pi / 2.0f, 2.0f * pi / 3.0f};   // fifths, minor thirds, major thirds
        constexpr float kRadius[3] = {1.0f, 1.0f, 0.5f};
        float sum = 0.0f;
        for (uint8_t c = 0; c < 12; c++) sum += classes[c];
        for (uint8_t d = 0; d < 6; d++) point[d] = 0.0f;
        if (sum <= 0.0f) return;
        for (uint8_t c = 0; c < 12; c++)
            for (uint8_t k = 0; k < 3; k++) {
                const float w = kRadius[k] * classes[c] / sum;
                point[2 * k] += w * std::sin(static_cast<float>(c) * kStep[k]);
                point[2 * k + 1] += w * std::cos(static_cast<float>(c) * kStep[k]);
            }
    }

private:
    static constexpr float kSmoothMs = 200.0f;   // the twelve's smoothing: @xref{folding-the-octaves}
    static constexpr float kLagMs = 100.0f;      // how far behind the point is measured against: @xref{the-moment-the-harmony-moves}
    static constexpr float kFire = 0.06f;
    static constexpr float kRearm = 0.03f;
    static constexpr float kWarmupMs = 500.0f;   // @xref{the-gate-holds-the-harmony}

    /// Whether the smoothed twelve's centroid moved fast enough on this block to be a change.
    bool moved(float dtMs) {
        float point[6];
        centroid(smooth_, point);
        const float lag = 1.0f - std::exp(-dtMs / kLagMs);
        float motion = 0.0f;
        for (uint8_t d = 0; d < 6; d++) {
            if (openMs_ == 0.0f) behind_[d] = point[d];   // the first block open has no past
            const float gap = point[d] - behind_[d];
            motion += gap * gap;
            behind_[d] += lag * gap;
        }
        motion = std::sqrt(motion);
        const bool warm = openMs_ >= kWarmupMs;
        openMs_ += dtMs;
        if (motion < kRearm) armed_ = warm;
        if (!armed_ || motion < kFire) return false;
        armed_ = false;
        return true;
    }

    float smooth_[12] = {};
    float behind_[6] = {};
    float openMs_ = 0.0f;
    bool armed_ = false;
};

/// The piano's lowest key, A0 at 27.5 Hz, the first of the frame's notes.
inline constexpr uint8_t kNotesLowBand = kA4Band - 48;

/// The notes from A0 on the bands' scale through the dynamics, the ones over the bank's top reading 0; answers how many the bank measures.
template <size_t N>
inline uint8_t semitonesToNotes(const SemitoneBank& bank, const AudioDynamics& dyn, uint8_t (&notes)[N]) {
    const uint8_t measured = bank.bandCount() > kNotesLowBand ? static_cast<uint8_t>(bank.bandCount() - kNotesLowBand) : 0;
    const uint8_t count = measured < N ? measured : static_cast<uint8_t>(N);
    for (size_t i = 0; i < N; i++) notes[i] = i < count ? dyn.toByte(bandPowerDbFs(bank.power(static_cast<uint8_t>(kNotesLowBand + i)))) : 0;
    return count;
}

/// The key and its mode from the twelve over 15 s, Krumhansl-Schmuckler key finding: the averaged pitch classes against the major and minor profiles turned to each tonic, the best correlation naming the key.
/// @attribution Ángel Faraldo
///
/// @technical
///
/// ## The key profiles
///
/// Two pairs, each tonic first: Krumhansl and Kessler's probe-tone profiles (1982), the classical pair, and Faraldo's from a corpus of electronic dance music (Faraldo, Gómez, Jordà and Herrera 2016, Essentia's `edma`). A key scores the better of its two correlations, so dance music is judged by the profile drawn from it and a piano piece by the classical one, with no genre to set.
///
/// A key is named after 10 s of open gate and changes only when another key correlates 0.05 better, so a borrowed chord does not flip it.
struct KeyTracker {
    /// The key: 0 for none yet, 1 to 12 C to B major, 13 to 24 C to B minor.
    uint8_t key = 0;
    /// How well the twelve match it, the correlation, 0..1.
    float strength = 0.0f;

    /// One block of the twelve, 0..255, while the gate is open; answers the key.
    uint8_t update(const uint8_t chroma[12], bool gateOpen, float dtMs) {
        if (!gateOpen) return key;   // a pause holds the key
        const float a = 1.0f - std::exp(-dtMs / kAverageMs);
        for (uint8_t c = 0; c < 12; c++) avg_[c] += a * (static_cast<float>(chroma[c]) / 255.0f - avg_[c]);
        heardMs_ += dtMs;
        if (heardMs_ < kHeardMs) return key;
        uint8_t best = 0;
        float bestR = -2.0f;
        for (uint8_t k = 1; k <= 24; k++) {
            const float r = match(k);
            if (r > bestR) { bestR = r; best = k; }
        }
        const float currentR = key ? match(key) : -2.0f;
        if (key == 0 || bestR > currentR + kSwitchMargin) { key = best; strength = bestR; }
        else strength = currentR;
        return key;
    }

    /// How well the averaged twelve match key `k`, 1..24: the better of the two profile pairs' correlations, @xref{the-key-profiles}.
    float match(uint8_t k) const {
        const uint8_t tonic = static_cast<uint8_t>((k - 1) % 12);
        const bool major = k <= 12;
        const float classical = correlation(avg_, major ? kMajor : kMinor, tonic);
        const float dance = correlation(avg_, major ? kDanceMajor : kDanceMinor, tonic);
        return classical > dance ? classical : dance;
    }

    /// Forget the music heard, as a new stream does.
    void reset() { *this = KeyTracker{}; }

    /// Pearson correlation of twelve values against a profile turned to `tonic`.
    static float correlation(const float (&x)[12], const float (&profile)[12], uint8_t tonic) {
        float mx = 0.0f, mp = 0.0f;
        for (uint8_t c = 0; c < 12; c++) { mx += x[c]; mp += profile[c]; }
        mx /= 12.0f; mp /= 12.0f;
        float sxy = 0.0f, sxx = 0.0f, syy = 0.0f;
        for (uint8_t c = 0; c < 12; c++) {
            const float dx = x[c] - mx, dp = profile[(c + 12 - tonic) % 12] - mp;
            sxy += dx * dp; sxx += dx * dx; syy += dp * dp;
        }
        return sxx > 0.0f && syy > 0.0f ? sxy / std::sqrt(sxx * syy) : 0.0f;
    }

private:
    static constexpr float kAverageMs = 15000.0f;
    static constexpr float kHeardMs = 10000.0f;
    static constexpr float kSwitchMargin = 0.05f;
    static constexpr float kMajor[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};   // @xref{the-key-profiles}
    static constexpr float kMinor[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};
    static constexpr float kDanceMajor[12] = {1.00f, 0.29f, 0.50f, 0.40f, 0.60f, 0.56f, 0.32f, 0.80f, 0.31f, 0.45f, 0.42f, 0.39f};
    static constexpr float kDanceMinor[12] = {1.00f, 0.31f, 0.44f, 0.58f, 0.33f, 0.49f, 0.29f, 0.78f, 0.43f, 0.29f, 0.53f, 0.32f};
    float avg_[12] = {};
    float heardMs_ = 0.0f;
};

/// How far the music sits from A440, in cents: where each strong note's power sits between its two neighbors, averaged over 10 s.
///
/// @technical
///
/// ## Reading the tuning
///
/// A note's offset is the parabola through its band and its two neighbors in dB, the position of its peak; a sine 40 cents sharp reads 34, 20 reads 15, so the reading is three quarters of the offset and of the right sign.
///
/// Offsets are averaged on the circle, a note 50 cents sharp being 50 flat of the next, weighted by power, so a recording's many notes outvote one out-of-tune one. AudioService retunes the bank by the reading once it holds over 10 cents for 10 s; the residual shrinks to a quarter each time.
struct TuningEstimator {
    /// One block of the bank's notes; answers the reading in cents, -50..50, 0 before any strong note: @xref{reading-the-tuning}.
    float update(const SemitoneBank& bank, bool gateOpen, float dtMs) {
        if (!gateOpen) return cents();
        float loudest = 0.0f;
        for (uint8_t b = kChromaLowBand; b < bank.bandCount(); b++) loudest = bank.power(b) > loudest ? bank.power(b) : loudest;
        if (loudest <= 0.0f) return cents();
        float c = 0.0f, s = 0.0f, w = 0.0f;
        for (uint8_t b = static_cast<uint8_t>(kChromaLowBand + 1); b + 1 < bank.bandCount(); b++) {
            const float p = bank.power(b);
            if (p < loudest * kStrong || p <= bank.power(b - 1) || p < bank.power(b + 1)) continue;   // a strong peak
            const float angle = 2.0f * std::numbers::pi_v<float> * offsetAt(bank, b);
            c += p * std::cos(angle); s += p * std::sin(angle); w += p;
        }
        if (w <= 0.0f) return cents();
        const float a = 1.0f - std::exp(-dtMs / kAverageMs);
        cos_ += a * (c / w - cos_);
        sin_ += a * (s / w - sin_);
        return cents();
    }

    /// The reading in cents.
    float cents() const { return (cos_ == 0.0f && sin_ == 0.0f) ? 0.0f : 100.0f * std::atan2(sin_, cos_) / (2.0f * std::numbers::pi_v<float>); }

    /// Start over, as after a retune.
    void reset() { cos_ = sin_ = 0.0f; }

private:
    static constexpr float kAverageMs = 10000.0f;
    static constexpr float kStrong = 0.01f;   // a peak within 20 dB of the loudest note
    /// A peak's offset from its band's center in semitones, -0.5..0.5: the parabola through it and its neighbors in dB.
    static float offsetAt(const SemitoneBank& bank, uint8_t b) {
        const float l = powerDb(bank.power(b - 1)), m = powerDb(bank.power(b)), r = powerDb(bank.power(b + 1));
        const float den = l - 2.0f * m + r;
        return den < 0.0f ? 0.5f * (l - r) / den : 0.0f;
    }
    float cos_ = 0.0f, sin_ = 0.0f;
};

/// The bass line's range, B0 to B3, and how far its loudest note must stand over the range's mean to count as a note.
inline constexpr uint8_t kBassNoteLowBand = kA4Band - 46, kBassNoteTopBand = kA4Band - 10;
inline constexpr float kBassNoteStandout = 4.0f;   // 6 dB

/// The bass line's note as a MIDI number, A4 being 69, or 0 while no note stands out of the bass, as in a dense mix where the kick shares the range.
inline uint8_t semitonesToBassNote(const SemitoneBank& bank, bool gateOpen) {
    if (!gateOpen || bank.bandCount() <= kBassNoteTopBand) return 0;
    float sum = 0.0f, loudest = 0.0f;
    uint8_t at = 0;
    for (uint8_t b = kBassNoteLowBand; b <= kBassNoteTopBand; b++) {
        sum += bank.power(b);
        if (bank.power(b) > loudest) { loudest = bank.power(b); at = b; }
    }
    const float mean = sum / static_cast<float>(kBassNoteTopBand - kBassNoteLowBand + 1);
    return loudest > 0.0f && loudest > kBassNoteStandout * mean ? static_cast<uint8_t>(at + 69 - kA4Band) : 0;
}

/// The names of the twelve, C first.
inline constexpr const char* kPitchClassNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/// The key's name, "A minor" or, short, "Am"; "-" for none.
inline void formatKey(uint8_t key, char* out, size_t size, bool shortForm = false) MM_NONBLOCKING {
    if (size < 2) return;
    const char* parts[2] = {"-", ""};
    if (key >= 1 && key <= 24) {
        parts[0] = kPitchClassNames[(key - 1) % 12];
        parts[1] = shortForm ? (key > 12 ? "m" : "") : (key > 12 ? " minor" : " major");
    }
    size_t len = 0;
    for (const char* part : parts)
        for (const char* t = part; *t && len + 1 < size; t++) out[len++] = *t;
    out[len] = 0;
}

/// The strongest pitch class left to name, or -1 when the rest read 0.
inline int strongestUnnamed(const uint8_t chroma[12], const bool (&named)[12]) MM_NONBLOCKING {
    int best = -1;
    for (uint8_t c = 0; c < 12; c++)
        if (!named[c] && chroma[c] > 0 && (best < 0 || chroma[c] > chroma[best])) best = c;
    return best;
}

/// The three strongest pitch classes as names, strongest first, "C E G"; "-" while all read 0.
inline void formatNotes(const uint8_t chroma[12], char* out, size_t size) MM_NONBLOCKING {
    if (size < 2) return;
    size_t len = 0;
    bool named[12] = {};
    for (uint8_t n = 0; n < 3; n++) {
        const int best = strongestUnnamed(chroma, named);
        if (best < 0) break;
        named[best] = true;
        const char* name = kPitchClassNames[best];
        if (len + (len ? 1 : 0) + (name[1] ? 2 : 1) + 1 > size) break;   // the weakest fall off a short buffer
        if (len) out[len++] = ' ';
        for (const char* p = name; *p; p++) out[len++] = *p;
    }
    if (len == 0) out[len++] = '-';
    out[len] = 0;
}

}  // namespace mm
