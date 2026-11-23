#pragma once

#include "core/util/AudioFrame.h"

#include <cmath>
#include <cstdint>

namespace mm {

/// The beat as an effect reads it on one tick: the beat section's fields with the phase run forward to now, and whether a beat started since the tick before.
struct BeatNow {
    uint16_t bpm100 = 0;        ///< the tempo × 100, 12800 for 128 BPM; 0 while no beat runs
    uint16_t count = 0;         ///< the beats since the source locked, wrapping; it steps forward once a beat
    uint8_t  phase = 0;         ///< how far the current beat has run, 0..255
    uint8_t  inBar = 0;         ///< the beat in the bar, 1..4, or 0 while its place in the bar is unknown
    uint8_t  source = 0;        ///< a BeatSource
    uint8_t  confidence = 0;    ///< 0..255: the tracker's lock, 255 for a deck or a clock
    uint8_t  inPhrase = 0;      ///< the beat in the phrase, 1..`phraseBeats`, or 0 while there is no phrase
    uint8_t  phraseBeats = 0;   ///< the phrase's length in beats, or 0 while there is none
    uint8_t  flags = 0;         ///< AudioBeat's kBeatDownbeatKnown and kBeatFill
    bool     onBeat = false;    ///< true on the tick a beat started
    /// Whether a beat runs.
    constexpr bool live() const { return bpm100 != 0; }
    /// The tempo in whole BPM, the one rounding every readout shows.
    constexpr uint16_t bpm() const { return static_cast<uint16_t>((bpm100 + 50u) / 100u); }
};

/// The receiver rules of the MoonModules format's beat section, the one place they run: [Audio sync format](https://moonmodules.org/MoonLight/reference/audio-sync.html#beat).
///
/// Between packets the phase advances with the tempo and the time since the packet arrived; each packet pulls it a quarter of the way back toward the phase the packet carries, a phase-locked loop's proportional correction, which smooths WiFi's 5 to 20 ms of jitter.
///
/// A packet more than half a beat off, or at another tempo, sets the clock outright. A change of the beat count is a beat, reported once: a jump forward is one beat start with the count taking the source's. The beat ends four beats after the last packet.
///
/// A pull crosses at most one beat back, so the clock holds at that beat's start; a count further back is a source that moved, a sender change or a deck handover, and the clock takes it as it is.
class BeatClock {
public:
    /// How many beats the clock runs on after the last packet.
    static constexpr uint32_t kStaleBeats = 4;
    /// How far a packet pulls the clock toward its own phase.
    static constexpr float kPull = 0.25f;

    /// Take a packet's beat, arriving at `nowMs`; a section with no tempo is no beat, and the clock runs on as it was.
    void take(const AudioBeat& b, uint32_t nowMs) {
        if (b.bpm100 == 0) return;
        const float packetFrac = static_cast<float>(b.phase) / 256.0f;
        if (!live_ || tempoMoved(b.bpm100)) {
            beats_ = b.count;   // the count extended past 16 bits from here on
            frac_ = packetFrac;
        } else {
            const float rel = relAt(nowMs);
            const float whole = std::floor(rel);
            const uint32_t predicted = beats_ + static_cast<uint32_t>(static_cast<int32_t>(whole));
            const int16_t ahead = static_cast<int16_t>(static_cast<uint16_t>(b.count - static_cast<uint16_t>(predicted)));
            const float err = static_cast<float>(ahead) + packetFrac - (rel - whole);
            const float moved = (std::fabs(err) > 0.5f ? err : err * kPull) + (rel - whole);
            const float step = std::floor(moved);
            beats_ = predicted + static_cast<uint32_t>(static_cast<int32_t>(step));
            frac_ = moved - step;
        }
        packetBeats_ = beats_ + static_cast<uint32_t>(static_cast<int16_t>(static_cast<uint16_t>(b.count - static_cast<uint16_t>(beats_))));
        anchorMs_ = nowMs;
        packetMs_ = nowMs;
        last_ = b;
        live_ = true;
    }

    /// The beat at `nowMs`, its `onBeat` set when a beat started since the call before; one call a tick.
    BeatNow tick(uint32_t nowMs) {
        if (live_ && nowMs - packetMs_ > kStaleBeats * 6000000u / last_.bpm100) live_ = false;
        if (!live_) { shown_ = false; return BeatNow{}; }
        const float rel = relAt(nowMs);
        const float whole = std::floor(rel);
        BeatNow n;
        n.phase = show(beats_ + static_cast<uint32_t>(static_cast<int32_t>(whole)), static_cast<uint8_t>((rel - whole) * 256.0f), n.onBeat);
        n.bpm100 = last_.bpm100;
        n.count = static_cast<uint16_t>(shownBeats_);
        const int32_t since = static_cast<int32_t>(shownBeats_ - packetBeats_);
        n.inBar = last_.inBar ? static_cast<uint8_t>(wrap(last_.inBar - 1 + since, 4) + 1) : 0;
        n.inPhrase = last_.inPhrase && last_.phraseBeats ? static_cast<uint8_t>(wrap(last_.inPhrase - 1 + since, last_.phraseBeats) + 1) : 0;
        n.phraseBeats = last_.inPhrase ? last_.phraseBeats : 0;
        n.source = last_.source;
        n.confidence = last_.confidence;
        n.flags = last_.flags;
        return n;
    }

private:
    /// Report the clock's beat `at` with its `phase`: `onBeat` when it passed the beat shown last, the phase held at that beat's start when a pull took it one beat back, the beat taken as it is on a lock or a source that moved further back.
    uint8_t show(uint32_t at, uint8_t phase, bool& onBeat) {
        const int32_t ahead = static_cast<int32_t>(at - shownBeats_);
        const bool restart = !shown_ || ahead < -1;   // a pull reaches one beat back at most, so further is the source's own move
        shown_ = true;
        if (restart || ahead > 0) { onBeat = !restart; shownBeats_ = at; return phase; }
        return ahead < 0 ? 0 : phase;
    }
    /// The beats since the anchor's whole beat, at `nowMs`.
    float relAt(uint32_t nowMs) const { return frac_ + static_cast<float>(nowMs - anchorMs_) * static_cast<float>(last_.bpm100) / 6000000.0f; }
    /// Whether `bpm100` is more than 4% from the tempo the clock runs at.
    bool tempoMoved(uint16_t bpm100) const {
        const int32_t d = static_cast<int32_t>(bpm100) - last_.bpm100;
        return (d < 0 ? -d : d) * 25 > static_cast<int32_t>(last_.bpm100);
    }
    /// `v` modulo `n`, in 0..n-1.
    static int32_t wrap(int32_t v, int32_t n) { const int32_t r = v % n; return r < 0 ? r + n : r; }

    AudioBeat last_{};          ///< the last packet's fields
    uint32_t beats_ = 0;        ///< the whole beats at the anchor, the packets' count extended past 16 bits
    float    frac_ = 0;         ///< the beat's fraction at the anchor
    uint32_t packetBeats_ = 0;  ///< the last packet's count, extended, which the bar and the phrase count from
    uint32_t anchorMs_ = 0;     ///< when the clock was last set
    uint32_t packetMs_ = 0;     ///< when the last packet arrived
    uint32_t shownBeats_ = 0;   ///< the beat the last tick reported, so a beat is taken once
    bool     live_ = false;
    bool     shown_ = false;
};

}  // namespace mm
