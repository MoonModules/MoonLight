#pragma once

#include <cmath>     // filter design and the detector are float maths
#include <complex>   // the poles, between the analog prototype and the digital section
#include <cstddef>
#include <cstdint>
#include <numbers>   // pi, for the design
#include <span>

namespace mm {

/// The fractional-octave bands of IEC 61260-1, `B` to the octave from 20 Hz to 20 kHz, measured by a multirate filter bank: each band is the power through its own band-pass filter.
///
/// Three to the octave are the 31 third-octave bands an analyzer shows, counted from 1 kHz; twelve are semitones, counted from A440.
///
/// @attribution troyhacks
///
/// @technical
///
/// ## Octave by octave
///
/// A band is a fixed fraction of its center wide, so below 400 Hz it is narrower than two bins of a 512-point transform, and only a filter per band separates it.
///
/// The textbook analyzer runs the top octave's filters at the full rate, halves the rate through a low-pass, and runs the same filters for the octave below, so the bank costs about twice its top octave.
///
/// The octaves are base-2, which IEC 61260-1 allows: a halving is exactly an octave, so one design serves every octave, and the third-octave centers sit within 2.5% of their ISO 266 labels.
///
/// ## Two octaves at the full rate
///
/// The top octave reaches 0.47 of the rate, too high to halve under, so the full rate carries two octaves and each lower one runs where it sits at 0.12 to 0.24 of its rate.
///
/// The halving low-pass is a 6th-order Butterworth at 0.15 of its rate; what folds back starts at 0.38, about 48 dB down.
///
/// ## A band limit
///
/// A bank limited below the rate's reach halves the rate while its top band still fits where a lower octave sits, 0.12 to 0.24 of the halved rate, since the halving low-pass passes no higher; every octave then has a stage of its own. It pays for the octaves it measures: semitones to 2.5 kHz at 24 kHz start at 12 kHz.
///
/// ## The filters
///
/// Each band is a 6th-order Butterworth band-pass in three sections, its edges prewarped so its -3 dB points land on the band's nominal edges at any rate.
///
/// At three to the octave the next band's center reads 17 to 21 dB down; in the top octave, squeezed toward Nyquist by the bilinear transform, 12 dB.
///
/// ## A chunk at a time
///
/// Each stage filters its waiting samples a section at a time, three bands side by side with their coefficients and state in registers: one chain waits on its own feedback every sample, three independent ones overlap. A stage hands every second sample on.
///
/// ## The detector
///
/// A band's power is its filter's output squared through a one-pole low-pass of 10 ms, the same in every band, so all bars rise and fall alike and only a narrow filter's own ringing slows the bass; the display's ballistics shape the decay.
///
/// ## Each band's delay
///
/// A band reports a sound late by its filter's group delay at its center, the band-pass's and every halving low-pass's ahead of its stage: the narrower the band the longer, 47 ms at 80 Hz against under 1 ms from 4 kHz up, within 3 ms of when a tone burst's band reaches a tenth of its power. The detector's 10 ms is the same in every band, so it shifts them all alike and stays out.

template <uint8_t B>
class FractionalOctaveBank {
    static_assert(B >= 1 && B <= 24, "a fraction of an octave from a whole one to a quarter tone");

public:
    /// How many bands, 20 Hz to 20 kHz: ten octaves of `B` and the top one's last.
    static constexpr uint8_t kBands = static_cast<uint8_t>(10 * B + 1);
    /// Second-order sections per band filter.
    static constexpr uint8_t kSections = 3;
    /// Samples a stage filters at once: `process` splits any count into these.
    static constexpr size_t kChunk = 64;

    /// A band's center in hertz, once `init` has set the reference.
    float centerHz(uint8_t band) const { return referenceHz_ * std::exp2((static_cast<float>(band) - static_cast<float>(referenceBand_)) / B); }

    /// Design the filters for this rate, counting the bands from `referenceHz`, up to `topHz` if given, every center moved by `tuningCents`, and start from silence; the bands above read 0.
    /// @guide 1000 Hz is the IEC reference for third octaves; 440 Hz puts semitones on the notes.
    void init(uint32_t sampleRate, float referenceHz = 1000.0f, float topHz = 0.0f, float tuningCents = 0.0f) {
        // The grid, which band is which and where the limit cuts, comes from the nominal reference; the tuning moves the centers only, so band 54 stays A4 however a recording is tuned.
        referenceHz_ = referenceHz;
        referenceBand_ = static_cast<uint8_t>(std::lround(B * std::log2(referenceHz / kLowestHz)));
        top_ = 0;
        bandCount_ = 0;
        if (sampleRate == 0) return;
        const float fs = static_cast<float>(sampleRate);
        rate_ = fs;
        const int top = topBand(fs, topHz);
        if (top < 0) return;
        halvings_ = halvingsFor(centerHz(static_cast<uint8_t>(top)), fs);
        shared_ = halvings_ ? 1 : 2;
        top_ = static_cast<uint8_t>(top);
        bandCount_ = static_cast<uint8_t>(top + 1);
        referenceHz_ = referenceHz * std::exp2(tuningCents / 1200.0f);   // under half a semitone, inside the reach's margin
        const float topRate = fs / static_cast<float>(1u << stageOf(0)), stdRate = fs / static_cast<float>(1u << stageOf(1));
        for (uint8_t slot = 0; slot < B; slot++) {
            const int topBand = top - slot, stdBand = top - B - slot;
            if (topBand >= 0) designBandPass(centerHz(static_cast<uint8_t>(topBand)), topRate, topSet_[slot]);
            if (stdBand >= 0) designBandPass(centerHz(static_cast<uint8_t>(stdBand)), stdRate, stdSet_[slot]);
        }
        designLowPass(kLowPassCorner * fs, fs, lowPass_);
        const int octaves = top / B + 1;
        stages_ = static_cast<uint8_t>(stageOf(static_cast<uint8_t>(octaves - 1)) + 1);
        if (stages_ > kMaxStages) stages_ = kMaxStages;
        for (uint8_t b = 0; b < bandCount_; b++) {
            const float rate = fs / static_cast<float>(1u << stageOf(static_cast<uint8_t>((top - b) / B)));
            alpha_[b] = 1.0f - std::exp(-1.0f / (kDetectorSeconds * rate));   // @xref{the-detector}
        }
        reset();
    }

    /// Return every filter and detector to silence.
    void reset() {
        for (auto& band : bandState_) for (auto& s : band) s = {};
        for (auto& stage : lowPassState_) for (auto& s : stage) s = {};
        for (auto& p : power_) p = 0.0f;
        for (auto& ph : phase_) ph = 0;
    }

    /// Feed samples at the rate the bank was designed for, a chunk at a time: @xref{a-chunk-at-a-time}.
    void process(std::span<const float> x) {
        if (bandCount_ == 0) return;
        while (!x.empty()) {
            const size_t m = x.size() < kChunk ? x.size() : kChunk;
            for (size_t i = 0; i < m; i++) chunk_[0][i] = x[i];
            runStages(m);
            x = x.subspan(m);
        }
    }

    /// Feed one sample.
    void push(float x) { process({&x, 1}); }

    /// A band's power, in the input's units squared: a sine of amplitude A at its center reads A * A / 2.
    float power(uint8_t band) const { return band < bandCount_ ? power_[band] : 0.0f; }

    /// How many bands this rate carries, from band 0 up.
    uint8_t bandCount() const { return bandCount_; }

    /// How late a band reports a sound, in seconds: @xref{each-bands-delay}.
    float delaySeconds(uint8_t band) const {
        if (band >= bandCount_) return 0.0f;
        const uint8_t octave = static_cast<uint8_t>((top_ - band) / B), slot = static_cast<uint8_t>((top_ - band) % B);
        const uint8_t stage = stageOf(octave);
        const float hz = centerHz(band);
        float seconds = 0.0f;
        for (uint8_t k = 0; k < stage; k++) {
            const float rate = rate_ / static_cast<float>(1u << k);
            seconds += filterDelay(lowPass_, 2.0f * std::numbers::pi_v<float> * hz / rate) / rate;
        }
        const float rate = rate_ / static_cast<float>(1u << stage);
        return seconds + filterDelay(octave == 0 ? topSet_[slot] : stdSet_[slot], 2.0f * std::numbers::pi_v<float> * hz / rate) / rate;
    }

private:
    struct Biquad { float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    struct State { float z1 = 0, z2 = 0; };
    using Filter = Biquad[kSections];
    using FilterState = State[kSections];

    static constexpr uint8_t kMaxStages = 10;
    static constexpr float kLowestHz = 20.0f;      // where band 0 sits, within half a band
    static inline const float kEdge = std::exp2(0.5f / B);   // a band's edges sit half a band either side of its center
    static constexpr float kTopReach = 0.475f;     // the top band's upper edge stays this far under the rate, short of Nyquist
    static constexpr float kLowPassCorner = 0.15f; // the halving low-pass, as a fraction of the rate it runs at
    static constexpr float kDetectorSeconds = 0.010f;   // one time constant for every band

    /// One section over `m` samples in place, transposed direct form II, its state held in registers for the run.
    static void runSection(const Biquad& q, State& st, float* y, size_t m) {
        const float b0 = q.b0, b1 = q.b1, b2 = q.b2, a1 = q.a1, a2 = q.a2;
        float z1 = st.z1, z2 = st.z2;
        for (size_t i = 0; i < m; i++) {
            const float x = y[i];
            const float out = b0 * x + z1;
            z1 = b1 * x - a1 * out + z2;
            z2 = b2 * x - a2 * out;
            y[i] = out;
        }
        st.z1 = z1;
        st.z2 = z2;
    }

    /// A cascade over `m` samples in place, a section at a time.
    static void runFilter(const Filter& f, FilterState& s, float* y, size_t m) {
        for (uint8_t k = 0; k < kSections; k++) runSection(f[k], s[k], y, m);
    }

    /// Three bands side by side, each section's loop running their independent chains together so the processor overlaps them: @xref{a-chunk-at-a-time}.
    void runThree(const Filter* set, int b, const float* in, size_t m) {
        float* y0 = lane_[0];
        float* y1 = lane_[1];
        float* y2 = lane_[2];
        for (uint8_t k = 0; k < kSections; k++) {
            const Biquad &q0 = set[0][k], &q1 = set[1][k], &q2 = set[2][k];
            State &s0 = bandState_[b][k], &s1 = bandState_[b - 1][k], &s2 = bandState_[b - 2][k];
            const float g0 = q0.b0, g1 = q1.b0, g2 = q2.b0;   // a band-pass section is b0 (1 - z^-2): b1 is 0 and b2 is -b0
            const float c0 = q0.a1, c1 = q1.a1, c2 = q2.a1, d0 = q0.a2, d1 = q1.a2, d2 = q2.a2;
            float u0 = s0.z1, u1 = s1.z1, u2 = s2.z1, v0 = s0.z2, v1 = s1.z2, v2 = s2.z2;
            const float* x0 = k == 0 ? in : y0;
            const float* x1 = k == 0 ? in : y1;
            const float* x2 = k == 0 ? in : y2;
            for (size_t i = 0; i < m; i++) {
                const float i0 = x0[i], i1 = x1[i], i2 = x2[i];
                const float o0 = g0 * i0 + u0, o1 = g1 * i1 + u1, o2 = g2 * i2 + u2;
                u0 = v0 - c0 * o0; u1 = v1 - c1 * o1; u2 = v2 - c2 * o2;
                v0 = -g0 * i0 - d0 * o0; v1 = -g1 * i1 - d1 * o1; v2 = -g2 * i2 - d2 * o2;
                y0[i] = o0; y1[i] = o1; y2[i] = o2;
            }
            s0.z1 = u0; s1.z1 = u1; s2.z1 = u2;
            s0.z2 = v0; s1.z2 = v1; s2.z2 = v2;
        }
        float p0 = power_[b], p1 = power_[b - 1], p2 = power_[b - 2];
        const float a0 = alpha_[b], a1 = alpha_[b - 1], a2 = alpha_[b - 2];
        for (size_t i = 0; i < m; i++) {
            p0 += a0 * (y0[i] * y0[i] - p0);
            p1 += a1 * (y1[i] * y1[i] - p1);
            p2 += a2 * (y2[i] * y2[i] - p2);
        }
        power_[b] = p0; power_[b - 1] = p1; power_[b - 2] = p2;
    }

    /// One band alone, where an octave has fewer than three left.
    void runOne(const Filter& f, int b, const float* in, size_t m) {
        for (size_t i = 0; i < m; i++) lane_[0][i] = in[i];
        runFilter(f, bandState_[b], lane_[0], m);
        float p = power_[b];
        const float a = alpha_[b];
        for (size_t i = 0; i < m; i++) p += a * (lane_[0][i] * lane_[0][i] - p);
        power_[b] = p;
    }

    /// The highest band whose upper edge the rate still resolves, under the limit if one is given; -1 for none: @xref{two-octaves-at-the-full-rate|why the top octave has its own design}.
    int topBand(float fs, float topHz) const {
        for (int b = kBands - 1; b >= 0; b--) {
            const float hz = centerHz(static_cast<uint8_t>(b));
            if (hz * kEdge < kTopReach * fs && (topHz <= 0.0f || hz <= topHz)) return b;
        }
        return -1;
    }

    /// The halvings that only lower the rate, while a top band at `topHz` still fits where a lower octave sits: @xref{a-band-limit}.
    static uint8_t halvingsFor(float topHz, float fs) {
        uint8_t h = 0;
        while (h + 1 < kMaxStages && topHz * kEdge < kTopReach * fs / static_cast<float>(4u << h)) h++;
        return h;
    }

    /// The stage an octave runs at: octaves 0 and 1 share the full rate, each lower one a halving further: @xref{octave-by-octave}. Behind a band limit's halvings every octave has its own.
    uint8_t stageOf(uint8_t octave) const { return static_cast<uint8_t>(halvings_ + (octave < shared_ ? 0 : octave - shared_ + 1)); }

    /// The bands a stage measures, the octaves `stageOf` puts there.
    void runBands(uint8_t stage, const float* in, size_t m) {
        if (stage < halvings_) return;
        const int local = stage - halvings_;
        const int firstOctave = local == 0 ? 0 : local + shared_ - 1;
        const int lastOctave = local + shared_ - 1;
        for (int octave = firstOctave; octave <= lastOctave; octave++) {
            const Filter* set = octave == 0 ? topSet_ : stdSet_;
            for (uint8_t slot = 0; slot < B; slot = static_cast<uint8_t>(slot + 3)) {
                const int b = static_cast<int>(top_) - B * octave - slot;
                if (b < 0) break;
                if (slot + 3 <= B && b >= 2) runThree(set + slot, b, in, m);
                else for (uint8_t s = slot; s < B && s < slot + 3 && b - (s - slot) >= 0; s++) runOne(set[s], b - (s - slot), in, m);
            }
        }
    }

    /// The decimation chain over `m` samples in `chunk_[0]`: each stage measures its bands, low-passes in place and hands every second sample on, the two buffers taking turns.
    void runStages(size_t m) {
        uint8_t cur = 0;
        for (uint8_t stage = 0; stage < stages_ && m > 0; stage++) {
            float* in = chunk_[cur];
            runBands(stage, in, m);
            if (stage + 1 == stages_) return;
            runFilter(lowPass_, lowPassState_[stage], in, m);
            float* next = chunk_[cur ^ 1u];
            size_t kept = 0;
            for (size_t i = 0; i < m; i++) {
                phase_[stage] ^= 1u;
                if (!phase_[stage]) next[kept++] = in[i];   // every second sample: the next stage runs at half this rate
            }
            m = kept;
            cur ^= 1u;
        }
    }

    /// A pole of the analog prototype, through the bilinear transform, into one section's denominator.
    static void poleToSection(std::complex<float> s, float fs2, Biquad& q) {
        const std::complex<float> z = (fs2 + s) / (fs2 - s);
        q.a1 = -2.0f * z.real();
        q.a2 = std::norm(z);
    }

    /// The section's gain at angular frequency w, in radians per sample.
    static float gainAt(const Biquad& q, float w) {
        const std::complex<float> e1 = std::polar(1.0f, -w), e2 = std::polar(1.0f, -2.0f * w);
        const float b0 = q.b0, b1 = q.b1, b2 = q.b2, a1 = q.a1, a2 = q.a2;
        return std::abs((b0 + b1 * e1 + b2 * e2) / (1.0f + a1 * e1 + a2 * e2));
    }

    /// A polynomial's delay at w, Re(sum k c_k e^-jwk / sum c_k e^-jwk), in samples: the textbook group delay of a section's numerator or denominator.
    static float polyDelay(float c0, float c1, float c2, float w) {
        const std::complex<float> e1 = std::polar(1.0f, -w), e2 = std::polar(1.0f, -2.0f * w);
        return std::real((c1 * e1 + 2.0f * c2 * e2) / (c0 + c1 * e1 + c2 * e2));
    }

    /// A cascade's group delay at w, in samples: each section's numerator's less its denominator's.
    static float filterDelay(const Filter& f, float w) {
        float samples = 0.0f;
        for (const Biquad& q : f) samples += polyDelay(q.b0, q.b1, q.b2, w) - polyDelay(1.0f, q.a1, q.a2, w);
        return samples;
    }

    /// The 6th-order Butterworth band-pass for one band, each section unity at the center: @xref{the-filters}.
    static void designBandPass(float centerHz, float fs, Filter& f) {
        constexpr float pi = std::numbers::pi_v<float>;
        const float fs2 = 2.0f * fs, edge = kEdge;
        const float lo = fs2 * std::tan(pi * centerHz / edge / fs);
        const float hi = fs2 * std::tan(pi * centerHz * edge / fs);
        const float w0sq = lo * hi, bw = hi - lo;
        const float wCenter = 2.0f * std::atan(std::sqrt(w0sq) / fs2);   // where the analog center lands, in radians per sample
        // The prototype's poles in the upper half plane: 120 and 180 degrees. Each maps to two band-pass poles.
        uint8_t k = 0;
        for (const float angle : {2.0f * pi / 3.0f, pi}) {
            const std::complex<float> p = std::polar(1.0f, angle) * (bw / 2.0f);
            const std::complex<float> root = std::sqrt(p * p - w0sq);
            const std::complex<float> poles[2] = {p + root, p - root};
            for (const auto& s : poles) {
                if (k == kSections) break;
                // The real prototype pole gives a conjugate pair, one section; keep its upper pole.
                if (angle == pi && s.imag() < 0) continue;
                Biquad& q = f[k++];
                poleToSection(s, fs2, q);
                q.b0 = 1.0f; q.b1 = 0.0f; q.b2 = -1.0f;   // a zero at DC and one at Nyquist per section
                const float g = 1.0f / gainAt(q, wCenter);
                q.b0 *= g; q.b2 *= g;
            }
        }
    }

    /// The 6th-order Butterworth low-pass that halves the rate, each section unity at DC.
    static void designLowPass(float cornerHz, float fs, Filter& f) {
        constexpr float pi = std::numbers::pi_v<float>;
        const float fs2 = 2.0f * fs;
        const float wc = fs2 * std::tan(pi * cornerHz / fs);
        for (uint8_t k = 0; k < kSections; k++) {
            const float angle = pi * (2.0f * k + 7.0f) / 12.0f;   // the order-6 prototype's poles in the upper-left quarter
            Biquad& q = f[k];
            poleToSection(std::polar(wc, angle), fs2, q);
            const float g = (1.0f + q.a1 + q.a2) / 4.0f;
            q.b0 = g; q.b1 = 2.0f * g; q.b2 = g;   // two zeros at Nyquist
        }
    }

    float referenceHz_ = 1000.0f;
    float rate_ = 0.0f;
    uint8_t referenceBand_ = 0;
    uint8_t top_ = 0;
    uint8_t bandCount_ = 0;
    uint8_t stages_ = 0;
    uint8_t halvings_ = 0;           // the stages ahead of the top band that only halve: @xref{a-band-limit}
    uint8_t shared_ = 2;             // the octaves the first measuring stage carries
    Filter topSet_[B] = {};          // the top octave's bands, at the full rate
    Filter stdSet_[B] = {};          // every octave below, each at the rate that puts it here
    Filter lowPass_ = {};
    FilterState bandState_[kBands] = {};
    FilterState lowPassState_[kMaxStages] = {};
    uint8_t phase_[kMaxStages] = {};
    float chunk_[2][kChunk] = {};            // a stage's samples and the next stage's, taking turns
    float lane_[3][kChunk] = {};            // three bands' filter outputs, measured and dropped
    float alpha_[kBands] = {};
    float power_[kBands] = {};
};

}  // namespace mm
