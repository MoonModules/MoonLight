#pragma once

#include "core/util/AudioFrame.h"
#include "core/services/AudioHarmony.h"   // kPitchClassNames
#include "platform/nonblocking.h"         // MM_NONBLOCKING

#include <cstddef>
#include <cstdint>

namespace mm {

/// A chord's quality, the frame's `chordQuality`: none, or which of the nine templates named it.
enum ChordQuality : uint8_t { kChordNone, kChordMajor, kChordMinor, kChordSeventh, kChordMajorSeventh, kChordMinorSeventh, kChordDiminished, kChordAugmented, kChordSus2, kChordSus4, kChordQualityCount };

/// Each quality's pitch classes above its root, a bit each, in ChordQuality order; none has none.
inline constexpr uint16_t kChordTemplates[kChordQualityCount] = {
    0,
    (1u << 0) | (1u << 4) | (1u << 7),                  // major: root, major third, fifth
    (1u << 0) | (1u << 3) | (1u << 7),                  // minor
    (1u << 0) | (1u << 4) | (1u << 7) | (1u << 10),     // dominant seventh
    (1u << 0) | (1u << 4) | (1u << 7) | (1u << 11),     // major seventh
    (1u << 0) | (1u << 3) | (1u << 7) | (1u << 10),     // minor seventh
    (1u << 0) | (1u << 3) | (1u << 6),                  // diminished
    (1u << 0) | (1u << 4) | (1u << 8),                  // augmented
    (1u << 0) | (1u << 2) | (1u << 7),                  // suspended second
    (1u << 0) | (1u << 5) | (1u << 7),                  // suspended fourth
};

/// Each quality's suffix after the root's name, in ChordQuality order.
inline constexpr const char* kChordSuffixes[kChordQualityCount] = {"", "", "m", "7", "maj7", "m7", "dim", "aug", "sus2", "sus4"};

/// Whether pitch class `pc` (C = 0) is in the chord on `root` of `quality`.
constexpr bool inChord(uint8_t pc, uint8_t root, uint8_t quality) MM_NONBLOCKING {
    return quality != kChordNone && quality < kChordQualityCount && root < 12 && pc < 12 && ((kChordTemplates[quality] >> ((pc + 12 - root) % 12)) & 1u);
}

/// The chord's name, "Am7" or "C", into `out`; empty for none.
inline void formatChord(uint8_t root, uint8_t quality, char* out, size_t size) MM_NONBLOCKING {
    if (size == 0) return;
    size_t len = 0;
    if (quality != kChordNone && quality < kChordQualityCount && root < 12)
        for (const char* part : {kPitchClassNames[root], kChordSuffixes[quality]})
            for (const char* t = part; *t && len + 1 < size; t++) out[len++] = *t;
    out[len] = 0;
}

/// Names the chord from a frame's twelve pitch classes by template matching (Fujishima 1999): each quality on each root scored by the cosine of its pitch classes against the frame's, a major or minor triad preferred over a colored chord, the best naming the chord: 108 sums over twelve pitch classes once a frame.
///
/// @technical
///
/// ## A chord that holds
///
/// The named chord changes only where the harmony-change detector fires and another chord scores better by kBetter, so a melody's passing notes do not flicker it.
///
/// Once the pitch classes stop matching it, another chord that fits by kAdopt takes over at once, and with none fitting it is dropped after kDropHoldMs, as when the music stops, so a drum hit smearing the pitch classes for a frame or two leaves it named.
///
/// The cosine is compared squared, so no square root runs: a template of n pitch classes scores dot² / (n · |chroma|²).
struct ChordTracker {
    static constexpr float kAdopt = 0.75f;    ///< the cosine a chord needs to be named from none
    static constexpr float kDrop = 0.55f;     ///< under this the named chord stops matching and is dropped
    static constexpr float kBetter = 0.05f;   ///< how much higher another chord's cosine must be to replace the named one at a change
    static constexpr uint32_t kDropHoldMs = 300;   ///< how long the named chord must stop matching before it is dropped, past a beat's smear

    uint8_t root = 0;              ///< the named chord's root, C = 0
    uint8_t quality = kChordNone;  ///< its quality, none before a chord is named
    uint16_t lastSeq = 0;          ///< the frame last named, so a frame read on two ticks is named once
    uint32_t mismatchMs = 0;       ///< when the named chord stopped matching, 0 while it matches

    /// Name the chord of `f` at `nowMs` and write it to the frame, once a frame: @xref{a-chord-that-holds}.
    void name(AudioFrame& f, uint32_t nowMs) MM_NONBLOCKING {
        if (f.seq != lastSeq) {
            lastSeq = f.seq;
            follow(f, nowMs);
        }
        f.chordRoot = root;
        f.chordQuality = quality;
    }

    static constexpr float kColorWeight = 0.95f;   ///< a seventh's, a sus chord's, a diminished or augmented chord's cosine weighed under a major or minor triad's, so a melody's passing tone does not color the chord; on a generated C, G, Am, F 64% to 67% of the time on the right root, on Let It Be about 56% to 66%

    /// The cosine of quality `q` on `root` against `chroma`, squared, any but a major or minor triad weighed by kColorWeight.
    static float scoreSquared(const uint8_t* chroma, uint8_t r, uint8_t q) MM_NONBLOCKING {
        return weighed(q, scoreAgainst(chroma, rotated(kChordTemplates[q], r), energyOf(chroma)));
    }

private:
    /// The best-scoring chord for `chroma` into `r` and `q`, answering its squared cosine.
    static float bestChord(const uint8_t* chroma, uint8_t& r, uint8_t& q) MM_NONBLOCKING {
        const uint32_t energy = energyOf(chroma);   // the same for every template, so summed once
        float best = 0.0f;
        for (uint8_t qq = kChordMajor; qq < kChordQualityCount; qq++)
            for (uint8_t rr = 0; rr < 12; rr++) {
                const float s = weighed(qq, scoreAgainst(chroma, rotated(kChordTemplates[qq], rr), energy));
                if (s > best) { best = s; r = rr; q = qq; }
            }
        return best;
    }

    /// A squared cosine `s` of quality `q`, any but a major or minor triad's weighed by kColorWeight squared.
    static float weighed(uint8_t q, float s) MM_NONBLOCKING {
        const bool plain = q == kChordMajor || q == kChordMinor;
        return plain ? s : s * kColorWeight * kColorWeight;
    }

    /// The sum of the pitch classes' squares, the frame's half of the cosine.
    static uint32_t energyOf(const uint8_t* chroma) MM_NONBLOCKING {
        uint32_t energy = 0;
        for (uint8_t pc = 0; pc < 12; pc++) energy += static_cast<uint32_t>(chroma[pc]) * chroma[pc];
        return energy;
    }

    /// A template's pitch classes moved up to `root`, a twelve-bit rotation.
    static uint16_t rotated(uint16_t t, uint8_t root) MM_NONBLOCKING {
        return static_cast<uint16_t>(((t << root) | (t >> (12 - root))) & 0xFFFu);
    }

    /// The squared cosine of the pitch classes in `mask` against `chroma`, whose energy is `energy`.
    static float scoreAgainst(const uint8_t* chroma, uint16_t mask, uint32_t energy) MM_NONBLOCKING {
        uint32_t dot = 0, n = 0;
        for (uint8_t pc = 0; pc < 12; pc++)
            if ((mask >> pc) & 1u) { dot += chroma[pc]; n++; }
        if (energy == 0 || n == 0) return 0.0f;
        return static_cast<float>(dot) * static_cast<float>(dot) / (static_cast<float>(n) * static_cast<float>(energy));
    }

    /// The named chord stopped matching: a chord that fits takes over, the music having moved, and with none fitting the name holds until kDropHoldMs have passed.
    void mismatch(float best, uint8_t bestRoot, uint8_t bestQuality, uint32_t nowMs) MM_NONBLOCKING {
        if (best >= kAdopt * kAdopt) { root = bestRoot; quality = bestQuality; mismatchMs = 0; return; }
        if (mismatchMs == 0) { mismatchMs = nowMs ? nowMs : 1; return; }
        if (nowMs - mismatchMs >= kDropHoldMs) { quality = kChordNone; mismatchMs = 0; }
    }

    /// Adopt the best chord from none, replace the named one at a change when another scores better by kBetter, drop it once it has stopped matching for kDropHoldMs.
    void follow(const AudioFrame& f, uint32_t nowMs) MM_NONBLOCKING {
        uint8_t bestRoot = 0, bestQuality = kChordNone;
        const float best = bestChord(f.chroma, bestRoot, bestQuality);
        if (quality == kChordNone) {
            if (best >= kAdopt * kAdopt) { root = bestRoot; quality = bestQuality; }
            return;
        }
        const float current = scoreSquared(f.chroma, root, quality);
        if (current < kDrop * kDrop) { mismatch(best, bestRoot, bestQuality, nowMs); return; }
        mismatchMs = 0;
        // A margin m on the cosine c, kept squared: (c + m)² is at most c² + 2m + m², c being at most 1.
        if (f.harmonyChange && (bestRoot != root || bestQuality != quality) && best > current + 2.0f * kBetter + kBetter * kBetter) {
            root = bestRoot;
            quality = bestQuality;
        }
    }
};

}  // namespace mm
