#pragma once

#include "core/util/AudioFrame.h"   // AudioFrame: the frame the audio sources read
#include "core/util/math8.h"        // sin8, the LFO's wave

#include <algorithm>
#include <cstdint>

/// The modulation matrix: every audio feature the frame carries and two LFOs, offered as @xref{the-sources|a source} a pattern parameter can listen to, with a depth.
///
/// One home for the routing an effect's mod slots share: the source list, what each source reads, and the one rule that turns a base value, a source and a depth into the value the pattern runs at, @xref{where-the-routing-runs|resolved once a tick}.
///
/// @attribution James Hutchinson
///
/// @technical
///
/// ## The sources
///
/// Every field of the frame that reads as a signal is a source: the level raw and smoothed, the three ranges, the drum hits together and per range, the flux, the peak magnitude, the chord change as a pulse, and the sixteen bands raw. The bands are raw rather than smoothed, since a parameter driven by a band should catch the transient; the depth tames it.
///
/// A source is a level from 0 to 255. The gate, a threshold, the peak frequency, in hertz, and the key, the chord root and the bass note, each a pitch, stay with the effects that read the frame for them.
///
/// Two low-frequency oscillators close the list, so a pattern moves on a board with no microphone: @xref{the-lfos}.
///
/// ## The LFOs
///
/// An LFO is a sine from 0 to 255, its rate a byte: rate r runs r/32 cycles a second, so rate 1 takes 32 seconds a cycle and rate 255 runs eight a second. It advances by the tick's elapsed time and carries the fraction of a phase unit a tick leaves, so its pace holds at any frame rate.
///
/// ## The one rule
///
/// A route adds the source's share of its depth to the base value, within the parameter's own range: at depth 0 the base stands untouched, at depth 255 and a full source the parameter rises by the whole depth. Sources only lift a parameter, so a route armed on a quiet room leaves the pattern exactly as its base controls set it.
///
/// The depth is in the parameter's own units, so the same depth reads the same on every parameter: depth 64 on a speed of 60 lifts it to 124 at a full source, and depth 64 on a trail of 200 lifts it to 255 at a source of 220 or more.
///
/// ## Where the routing runs
///
/// The effect resolves its modulated values once a tick, from the one frame it reads, and its motion runs on those: the routing costs one read per route per tick, shared by every particle in the frame, so all of them hear the same modulation.
namespace mm::modmatrix {

/// The sources a parameter can listen to, in the select's order: none, then the frame's signals, then the LFOs.
enum class Source : uint8_t {
    kNone,      ///< no modulation: the base value stands
    kLevel,     ///< this block's level, raw: snaps to a transient
    kSmooth,    ///< the level through a peak meter's ballistic: breathes with the music
    kBass,      ///< the bass, 20 to 125 Hz
    kMid,       ///< the mids, 160 Hz to 2 kHz
    kTreble,    ///< the treble, 2.5 kHz up
    kOnset,     ///< any drum hit, the strength of the range heard first
    kKick,      ///< the bass's hit: the kick
    kSnare,     ///< the mids' hit: the snare, the toms, a vocal's attack
    kHats,      ///< the treble's hit: the hi-hat, the cymbals
    kFlux,      ///< the spectral flux this block
    kPeak,      ///< the magnitude of the dominant frequency
    kChord,     ///< the harmony's move: full on the one block it changes, else none
    kBand0, kBand1, kBand2, kBand3, kBand4, kBand5, kBand6, kBand7,
    kBand8, kBand9, kBand10, kBand11, kBand12, kBand13, kBand14, kBand15,
    kLfo1,      ///< the first LFO
    kLfo2,      ///< the second LFO
    kCount
};

/// The option names, in the enum's order, for the select control.
inline constexpr const char* kSourceOptions[] = {
    "none", "level", "smooth level", "bass", "mid", "treble",
    "drums", "kick", "snare", "hats", "flux", "peak", "chord",
    "band 1", "band 2", "band 3", "band 4", "band 5", "band 6", "band 7", "band 8",
    "band 9", "band 10", "band 11", "band 12", "band 13", "band 14", "band 15", "band 16",
    "lfo 1", "lfo 2",
};

/// How many sources the select offers.
inline constexpr uint8_t kSourceCount = static_cast<uint8_t>(Source::kCount);
static_assert(sizeof(kSourceOptions) / sizeof(kSourceOptions[0]) == kSourceCount, "one option per source");

/// How many LFOs the sources offer.
inline constexpr uint8_t kLfoCount = 2;

/// The source a stored control value names; `read` answers 0 for one outside the list.
inline Source source(uint8_t v) { return static_cast<Source>(v); }

static_assert(static_cast<uint8_t>(Source::kBass) == 3 && static_cast<uint8_t>(Source::kSmooth) + 1 == static_cast<uint8_t>(Source::kBass), "none, the level and the smooth level, and only they, sit before the analysis's sources");
static_assert(static_cast<uint8_t>(Source::kBand15) + 1 == static_cast<uint8_t>(Source::kLfo1), "the LFOs follow the analysis's last source");
/// Whether source `s` reads the audio analysis, which runs only while a reader needs it: every frame signal from the bass on, the level and its smoothing running regardless.
inline bool readsAnalysis(Source s) {
    const uint8_t v = static_cast<uint8_t>(s);
    return v >= static_cast<uint8_t>(Source::kBass) && v <= static_cast<uint8_t>(Source::kBand15);
}
/// Whether source `s` reads the notes, which run on top of the analysis only while a reader needs them: the chord.
inline bool readsNotes(Source s) { return s == Source::kChord; }

/// One low-frequency oscillator: @xref{the-lfos}.
struct Lfo {
    uint16_t phase = 0;   ///< where in its cycle it is, a full cycle being 65536
    uint16_t carry = 0;   ///< the thousandths of a phase unit the last advance left

    /// Run `ms` milliseconds at `rate`, r/32 cycles a second.
    void advance(uint8_t rate, uint32_t ms) {
        const uint64_t units = static_cast<uint64_t>(rate) * ms * 2048u + carry;   // thousandths of a phase unit, wide for a long stall
        phase = static_cast<uint16_t>(phase + units / 1000u);
        carry = static_cast<uint16_t>(units % 1000u);
    }
    /// Its value now, a sine from 0 to 255.
    uint8_t value() const { return sin8(static_cast<uint8_t>(phase >> 8)); }
};

/// What the sources read in one tick: the audio frame, and the LFOs' values.
struct Inputs {
    const AudioFrame& frame;          ///< the latest audio frame
    uint8_t lfo[kLfoCount] = {0, 0};  ///< each LFO's value this tick
};

/// The frame's byte fields the sources kBass..kFlux read, in the enum's order, so one table answers all eight.
inline constexpr uint8_t AudioFrame::* kByteSources[] = {
    &AudioFrame::bass, &AudioFrame::mid, &AudioFrame::treble, &AudioFrame::onset,
    &AudioFrame::onsetLow, &AudioFrame::onsetMid, &AudioFrame::onsetHigh, &AudioFrame::flux,
};

/// Read source `s` from frame `f`, 0 to 255. A source outside the list, or an LFO, which lives in the inputs rather than the frame, reads 0, so a stored index from an older list leaves its knob at its base.
inline uint8_t read(Source s, const AudioFrame& f) {
    const uint8_t v = static_cast<uint8_t>(s);
    if (v >= static_cast<uint8_t>(Source::kBand0) && v <= static_cast<uint8_t>(Source::kBand15))
        return f.bands[v - static_cast<uint8_t>(Source::kBand0)];
    if (v >= static_cast<uint8_t>(Source::kBass) && v <= static_cast<uint8_t>(Source::kFlux))
        return f.*kByteSources[v - static_cast<uint8_t>(Source::kBass)];
    if (s == Source::kLevel) return static_cast<uint8_t>(std::min(f.level, static_cast<uint16_t>(255)));
    if (s == Source::kSmooth) return static_cast<uint8_t>(std::min(f.levelSmoothed, static_cast<uint16_t>(255)));
    if (s == Source::kPeak) return static_cast<uint8_t>(std::min(f.peakMag, static_cast<uint16_t>(255)));
    if (s == Source::kChord) return f.harmonyChange ? 255 : 0;
    return 0;   // none, the LFOs, and anything outside the list
}

/// Read source `s` from this tick's inputs: an LFO from its value, the rest from the frame.
inline uint8_t read(Source s, const Inputs& in) {
    const uint8_t v = static_cast<uint8_t>(s);
    if (v >= static_cast<uint8_t>(Source::kLfo1) && v < static_cast<uint8_t>(Source::kCount))
        return in.lfo[v - static_cast<uint8_t>(Source::kLfo1)];
    return read(s, in.frame);
}

/// A parameter's modulated value: its base, plus the source's share of the depth, within [`lo`, `hi`]: @xref{the-one-rule}.
inline uint8_t apply(uint8_t base, Source s, uint8_t depth, const Inputs& in, uint8_t lo, uint8_t hi) {
    const uint32_t v = static_cast<uint32_t>(base) + (static_cast<uint32_t>(read(s, in)) * depth) / 255u;
    return static_cast<uint8_t>(v < lo ? lo : v > hi ? hi : v);
}

/// The same rule against the frame alone, for a parameter no LFO reaches.
inline uint8_t apply(uint8_t base, Source s, uint8_t depth, const AudioFrame& f, uint8_t lo, uint8_t hi) {
    return apply(base, s, depth, Inputs{f}, lo, hi);
}

}  // namespace mm::modmatrix
