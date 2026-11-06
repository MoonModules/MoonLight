#pragma once

#include <cmath>     // the retune's bounds
#include <cstddef>
#include <cstdint>
#include <cstring>   // memcpy: the hits of dropped frames

#include "platform/platform.h"            // InternalRamObject: the bank touches its state every sample
#include "core/util/AudioFrame.h"
#include "core/util/SpscRing.h"           // the task's frames to the render tick
#include "core/services/AudioLevel.h"     // DcBlocker
#include "core/services/AudioBands.h"     // ThirdOctaveBank, SemitoneBank
#include "core/services/AudioDynamics.h"
#include "core/services/AudioHarmony.h"   // the harmony's trackers
#include "core/services/AudioOnsets.h"    // the onsets per range and per band

namespace mm {

/// The block an input is read and analyzed in, a power of two: about 21 ms at 24 kHz, a frame of bands each.
inline constexpr size_t kAudioBlock = 512;

/// The analysis of an open input, which AudioService allocates in internal RAM as the input opens and frees as it closes, so a mode without an input holds none of it.
struct AudioAnalysis : platform::InternalRamObject {
    int32_t samples[kAudioBlock] = {};  ///< fixed hot-path scratch, sized once
    size_t filled = 0;                  ///< samples accumulated toward the next full block
    DcBlocker dc;                       ///< 5 Hz high-pass, continuous across blocks
    float dcPole = 0.9993f;             ///< its pole at the opened rate, so the corner stays at 5 Hz below the 20 Hz band
    float blockMs = 21.3f;              ///< one block's duration at the opened rate
    uint32_t openedRate = 24000;        ///< the rate the stream opened at, which the task reads rather than the control
    uint64_t streamSamples = 0;         ///< the samples analyzed since it opened: the analysis's clock
    ThirdOctaveBank bank;               ///< the 31 bands, designed for the opened rate
    AudioDynamics dyn;                  ///< the level, the floor, the gate and the window's top, learned live
    AudioOnsets onsets;                 ///< the onsets per range and per band, read every 5 ms
    AudioFrame work;                    ///< the frame the analysis writes, on the audio task where there is one
    SpscRing<AudioFrame, 8> frames;     ///< the task's finished frames, which the tick drains to the newest

    /// A new stream at `rate`, from silence, field by field since a whole analysis is too large for a stack.
    void reset(uint32_t rate) {
        dc.reset();
        dyn.reset();                    // a new rate is a new stream: the floor and the top learn it afresh
        filled = 0;                     // no block mixes samples of two rates
        dcPole = 1.0f - 6.2831853f * kDcCornerHz / static_cast<float>(rate);
        openedRate = rate;
        blockMs = static_cast<float>(kAudioBlock) * 1000.0f / static_cast<float>(rate);   // the opened rate's, which the task reads rather than the control
        streamSamples = 0;
        work = AudioFrame{};
        bank.init(rate);
        onsets.reset();
    }

private:
    static constexpr float kDcCornerHz = 5.0f;   // under the 20 Hz band
};

/// Drain `ring` into `out`, keeping the newest frame with the strongest onset among them, so a hit in an earlier block of the same tick still reaches the effects.
inline bool takeNewest(SpscRing<AudioFrame, 8>& ring, AudioFrame& out) {
    AudioFrame f;
    uint8_t onset = 0, low = 0, mid = 0, high = 0;
    uint8_t bandHits[4] = {}, noteHits[10] = {};
    bool change = false;
    bool fresh = false;
    while (ring.pop(&f, 1) == 1) {
        fresh = true;
        onset = f.onset > onset ? f.onset : onset;
        low = f.onsetLow > low ? f.onsetLow : low;
        mid = f.onsetMid > mid ? f.onsetMid : mid;
        high = f.onsetHigh > high ? f.onsetHigh : high;
        for (uint8_t k = 0; k < 4; k++) bandHits[k] |= f.thirdsHits[k];
        for (uint8_t k = 0; k < 10; k++) noteHits[k] |= f.notesHits[k];
        change = change || f.harmonyChange;
    }
    if (!fresh) return false;
    out = f;
    out.onset = onset;            // a hit, a range's or a band's hit or a chord change in a dropped block is one too
    out.onsetLow = low;
    out.onsetMid = mid;
    out.onsetHigh = high;
    std::memcpy(out.thirdsHits, bandHits, sizeof(bandHits));
    std::memcpy(out.notesHits, noteHits, sizeof(noteHits));
    out.harmonyChange = change;
    return true;
}

/// The harmony's state, which AudioService allocates only while `harmony` is on: the semitone bank and what reads it.
struct AudioHarmonyState : platform::InternalRamObject {
    SemitoneBank semitones;             ///< the notes to kSemitoneTopHz
    HarmonyTracker tracker;             ///< the pitch classes and the moment they move
    KeyTracker key;                     ///< the key over the last 15 s
    TuningEstimator tuning;             ///< how far the music sits from the bank's tuning
    BandOnsets<kAudioNoteKeys, kNotesLowBand, true> noteOnsets;   ///< a new note's start, one per piano key
    float tuningCents = 0.0f;           ///< the bank's tuning from A440, in cents
    float tuningHeldMs = 0.0f;          ///< how long the reading has stayed past the retune threshold

    /// The harmony from silence at `rate`, on A440, field by field since a whole one is too large for a stack.
    void reset(uint32_t rate) {
        semitones.init(rate, 440.0f, kSemitoneTopHz);
        tracker.reset();
        key.reset();
        tuning.reset();
        noteOnsets.reset();
        tuningCents = tuningHeldMs = 0.0f;
    }

    /// Retune the semitone bank once the music has sat off its tuning long enough, so each note lands on its own band, as TuningEstimator's notes say; true when it retuned.
    bool retune(float readingCents, uint32_t rate, float blockMs) {
        tuningHeldMs = std::fabs(readingCents) > kRetuneCents ? tuningHeldMs + blockMs : 0.0f;
        if (tuningHeldMs < kRetuneHoldMs) return false;
        tuningCents = std::fmax(-50.0f, std::fmin(50.0f, tuningCents + readingCents));
        semitones.init(rate, 440.0f, kSemitoneTopHz, tuningCents);
        tracker.reset();      // the bank starts from silence: no change for its settling
        tuning.reset();
        noteOnsets.reset();   // the retuned bands' delays
        tuningHeldMs = 0.0f;
        return true;
    }

private:
    static constexpr float kRetuneCents = 10.0f;      // a reading past this, held kRetuneHoldMs, retunes the bank
    static constexpr float kRetuneHoldMs = 10000.0f;
};

}  // namespace mm
