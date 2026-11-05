#pragma once

#include <cstdint>

#include "core/services/AudioBands.h"   // ThirdOctaveBank, OnsetDetector, the range edges
#include "core/util/AudioFrame.h"

namespace mm {

/// The onsets the analysis reads from its banks every 5 ms: one per range, bass, mids and treble, one per third-octave band and one per note, each hit timed within a reading rather than within a block.
///
/// @technical
///
/// ## The onset signal
///
/// Scheirer's construction (1998): each band's log power, its rise kept and its fall dropped, summed over the range's bands, read from the bank's detectors every 5 ms, 200 readings a second, rather than from the display's bytes once a block. Each range decides by the flux against its own recent mean, as the any-hit onset does, so a hi-hat over a held bass fires the treble alone.
///
/// A range fires only on a rise of 10 dB in one reading, summed over its bands, over the detector's own margin: a hit rises that much, a bass filter still ringing after one a few.
///
/// A band counts only within 40 dB of the loudest, both its readings held at that reading's floor (Klapuri 1999): a log rise is the same in a band 50 dB under the music as in the loudest, so a hi-hat's faint bass rising out of the room's hiss would fire the bass. The gate is the block's own, applied as the block's hits are taken.
///
/// ## An onset per band
///
/// Each band makes the same decision on its own rise, with 40 ms or the band's delay between its hits, whichever is longer.
///
/// Strokes at 10 a second stay separate hits; a faster roll merges, its band falling only about 14 dB between strokes at 20 a second, through the bank's 10 ms detector, so the detector's own mean outgrows the next rise. A band whose rise lasts its delay fires once for it. A tom is pitched, so each lands in its own band and a fill reads as a sweep across them.
///
/// A band fires as soon as its rise shows, and its hit's start is the reading's time less the band's delay, the filter's group delay that FractionalOctaveBank computes.
///
/// Holding the treble back to line it up with the bass would add the bass's delay to every hit, so the delay is subtracted where onsets are compared rather than where they are shown.
///
/// ## The kick and its click
///
/// A narrow bass band takes tens of milliseconds to rise, while the beater's click at 2 to 5 kHz rises within one or two. A kick is a click whose bass starts to rise, 5 dB summed in a reading, within the delay of the bass's fastest band, 27 ms; the click times it and the bass confirms it, so a hi-hat's click alone is no kick.
///
/// A kick without a click, a soft beater or a synthesized one, still fires on the bass's own onset, timed by the bass's fastest band.
///
/// ## A new note
///
/// The semitone bank's bands make the same decision, one per piano key: a key's attack or a plucked string fires its own note, and a tom its pitch. A note held under a new one has stopped rising, so only the new one fires.
///
/// A key's band also has to stand over both its neighbors, as a sounding note does: an attack's click spreads across the keys around it, the note's own standing out. A semitone band is a quarter of a third wide, so its delay is four times as long, 105 ms at 110 Hz.

/// Each band's onset, `N` bands of a bank from its band `First` up, a bit per band; `Pitched` fires only over both neighbors: @xref{a-new-note}.
template <uint8_t N, uint8_t First, bool Pitched>
class BandOnsets {
public:
    /// The bytes a bit per band takes.
    static constexpr uint8_t kBytes = (N + 7) / 8;

    /// Whether a reading is due at `nowMs`, at least 5 ms of stream time after the last.
    bool due(uint32_t nowMs) {
        if (primed_ && nowMs - lastMs_ < kReadMs) return false;
        lastMs_ = nowMs;
        return true;
    }

    /// Each band's power in dB from `bank`, held for this reading; answers how many bands the bank carries, and the floor a band counts above: @xref{the-onset-signal}.
    template <typename Bank>
    uint8_t read(const Bank& bank, float& floor) {
        const uint8_t count = bank.bandCount() <= First ? 0 : bank.bandCount() - First < N ? static_cast<uint8_t>(bank.bandCount() - First) : N;
        float loudest = -240.0f;
        for (uint8_t i = 0; i < count; i++) {
            db_[i] = powerDb(bank.power(static_cast<uint8_t>(First + i)));
            loudest = db_[i] > loudest ? db_[i] : loudest;
        }
        floor = loudest - kRangeDb;
        return count;
    }

    /// Band `i`'s level this reading, in dB.
    float level(uint8_t i) const { return i < N ? db_[i] : -240.0f; }

    /// Each band's onset on this reading, every level held at `floor`; false on the first reading, which takes the bank's delays.
    template <typename Bank>
    bool decide(const Bank& bank, uint8_t count, float floor, uint32_t nowMs) {
        if (!primed_) {
            prime(bank, count);
            return false;
        }
        for (uint8_t i = 0; i < count; i++) decideBand(i, count, floor, nowMs);
        return true;
    }

    /// Read `bank` at `nowMs` when a reading is due.
    template <typename Bank>
    void update(const Bank& bank, uint32_t nowMs) {
        if (!due(nowMs)) return;
        float floor;
        const uint8_t count = read(bank, floor);
        decide(bank, count, floor, nowMs);
    }

    /// The bands that fired since the last take into `hits`, none while the block's gate is shut, then clear for the next block.
    void take(uint8_t (&hits)[kBytes], bool gateOpen) {
        for (uint8_t k = 0; k < kBytes; k++) { hits[k] = gateOpen ? fired_[k] : 0; fired_[k] = 0; }
    }

    /// When band `i`'s last hit started on the stream's clock, its delay subtracted from when it fired: what onsets compared across bands read.
    uint32_t startMs(uint8_t i) const { return i < N ? detector_[i].lastMs_ - delayMs_[i] : 0; }

    /// Whether band `i` fired on the reading at `nowMs`.
    bool firedAt(uint8_t i, uint32_t nowMs) const { return i < N && detector_[i].fired_ && detector_[i].lastMs_ == nowMs; }

    /// How late band `i` reports a sound, in milliseconds.
    uint16_t delayMs(uint8_t i) const { return i < N ? delayMs_[i] : 0; }

    /// Forget the stream, as a new one starts, field by field: a whole one is too large for the audio task's stack, and the first reading primes the levels and delays again.
    void reset() {
        for (OnsetDetector& d : detector_) d = OnsetDetector{};
        for (uint8_t& f : fired_) f = 0;
        lastMs_ = 0;
        primed_ = false;
    }

    /// The reading's interval in stream time: @xref{the-onset-signal}.
    static constexpr uint32_t kReadMs = 5;
    /// The flux byte per dB of rise: 64 dB in one reading saturates it.
    static constexpr float kFluxPerDb = 4.0f;
    /// How far under the loudest band a band still counts.
    static constexpr float kRangeDb = 40.0f;
    /// The least flux that fires: a hit rises 10 dB in a reading, a filter still ringing a few.
    static constexpr uint8_t kMinFlux = 40;
    /// A flux to its byte, saturating.
    static uint8_t toByte(float v) { return static_cast<uint8_t>(v > 255.0f ? 255.0f : v); }

private:
    static constexpr uint16_t kRefractoryMs = 40;   // @xref{an-onset-per-band}

    /// The first reading: each band's delay from the bank, and its level as where its recent level starts.
    template <typename Bank>
    void prime(const Bank& bank, uint8_t count) {
        for (uint8_t i = 0; i < count; i++) {
            const float ms = bank.delaySeconds(static_cast<uint8_t>(First + i)) * 1000.0f;
            delayMs_[i] = static_cast<uint16_t>(ms < 65535.0f ? ms : 65535.0f);
            refDb_[i] = db_[i];
        }
        primed_ = true;
    }

    /// Whether band `i` stands over both neighbors, as a sounding note does: @xref{a-new-note}.
    bool standsOut(uint8_t i, uint8_t count) const {
        return (i == 0 || db_[i] > db_[i - 1]) && (i + 1 >= count || db_[i] > db_[i + 1]);
    }

    /// Band `i`'s rise against its recent level, and its detector's decision on it.
    void decideBand(uint8_t i, uint8_t count, float floor, uint32_t nowMs) {
        const float now = db_[i] > floor ? db_[i] : floor;
        const float recent = refDb_[i] > floor ? refDb_[i] : floor;
        // A one-pole over the band's own delay, its coefficient the backward-Euler step: @xref{an-onset-per-band}.
        refDb_[i] += static_cast<float>(kReadMs) / static_cast<float>(delayMs_[i] + kReadMs) * (db_[i] - refDb_[i]);
        const float flux = now > recent && (!Pitched || standsOut(i, count)) ? (now - recent) * kFluxPerDb : 0.0f;
        const uint16_t refractory = delayMs_[i] > kRefractoryMs ? delayMs_[i] : kRefractoryMs;
        if (detector_[i].feed(toByte(flux), nowMs, 3, 2, kMinFlux, refractory)) fired_[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }

    float db_[N] = {};               // each band's level this reading, held here rather than on the audio task's stack
    float refDb_[N] = {};            // each band's recent level, following it over the band's own delay
    uint16_t delayMs_[N] = {};       // each band's delay, read from the bank as the stream starts
    OnsetDetector detector_[N];
    uint8_t fired_[kBytes] = {};
    uint32_t lastMs_ = 0;
    bool primed_ = false;
};

/// The third octaves' onsets per band and per range, bass, mids and treble: a hit in its own range, @xref{the-onset-signal|the rise} summed over the range's bands.
struct AudioOnsets {
    /// The 31 bands' own onsets.
    using Bands = BandOnsets<31, 0, false>;

    /// Read the bank once at least 5 ms of stream time have passed, at `nowMs`.
    void update(const ThirdOctaveBank& bank, uint32_t nowMs) {
        if (!bands_.due(nowMs)) return;
        float floor;
        const uint8_t count = bands_.read(bank, floor);
        const bool primed = bands_.decide(bank, count, floor, nowMs);
        float rangeRise[3] = {};
        rangeRises(count, floor, rangeRise);
        if (!primed) return;
        noteClick(count, nowMs);
        for (uint8_t r = 0; r < 3; r++) {
            const uint8_t flux = Bands::toByte(rangeRise[r] * Bands::kFluxPerDb);
            bool fire = rangeDetector_[r].feed(flux, nowMs, 3, 2, Bands::kMinFlux);
            if (r == 0) fire = decideKick(fire, rangeRise[0], nowMs);
            if (fire && flux > rangeFired_[r]) rangeFired_[r] = flux > 0 ? flux : 1;
        }
    }

    /// When the last kick started, timed by its click where it had one: @xref{the-kick-and-its-click}.
    uint32_t kickStartMs() const { return kickStartMs_; }

    /// The strongest onset each range fired and the bands that fired since the last take into `f`, none while the block's gate is shut, then clear for the next block.
    void take(AudioFrame& f, bool gateOpen) {
        f.onsetLow = gateOpen ? rangeFired_[0] : 0;
        f.onsetMid = gateOpen ? rangeFired_[1] : 0;
        f.onsetHigh = gateOpen ? rangeFired_[2] : 0;
        for (uint8_t& r : rangeFired_) r = 0;
        bands_.take(f.thirdsHits, gateOpen);
    }

    /// When a band's last hit started, its delay subtracted.
    uint32_t startMs(uint8_t band) const { return bands_.startMs(band); }

    /// Forget the stream, as a new one starts, field by field as BandOnsets does.
    void reset() {
        bands_.reset();
        for (OnsetDetector& d : rangeDetector_) d = OnsetDetector{};
        for (uint8_t& r : rangeFired_) r = 0;
        clickMs_ = clickStartMs_ = kickMs_ = kickStartMs_ = 0;
        clicked_ = kicked_ = false;
    }

private:
    static constexpr uint8_t kClickLowThird = 20, kClickTopThird = 24;   // 2 to 5 kHz, where a beater's click sits
    static constexpr float kConfirmDb = 5.0f;          // a kick's bass rising, half a hit's rise: @xref{the-kick-and-its-click}
    static constexpr uint32_t kKickRefractoryMs = 100; // one kick, whether its click or its bass decided it

    /// Each range's rise since the last reading, its bands' rises summed, both readings held at this reading's floor so a band under it rises by nothing when the loudest jumps.
    void rangeRises(uint8_t count, float floor, float (&rise)[3]) {
        for (uint8_t b = 0; b < count; b++) {
            const float now = bands_.level(b) > floor ? bands_.level(b) : floor;
            const float before = prevDb_[b] > floor ? prevDb_[b] : floor;
            if (now > before) rise[rangeOfThird(b)] += now - before;
            prevDb_[b] = bands_.level(b);
        }
    }

    /// A click is a hit in the beater's bands on this reading; its start is the earliest of them.
    void noteClick(uint8_t count, uint32_t nowMs) {
        for (uint8_t b = kClickLowThird; b <= kClickTopThird && b < count; b++)
            if (bands_.firedAt(b, nowMs)) { clickMs_ = nowMs; clickStartMs_ = bands_.startMs(b); clicked_ = true; return; }
    }

    /// The bass's onset `own`, or the bass starting to rise within its fastest band's delay of a click, once a kick: @xref{the-kick-and-its-click}.
    bool decideKick(bool own, float bassRise, uint32_t nowMs) {
        const uint32_t window = bands_.delayMs(kBassTopThird) + Bands::kReadMs;
        const bool confirmed = clicked_ && nowMs - clickMs_ <= window && bassRise >= kConfirmDb;
        if (!(own || confirmed) || (kicked_ && nowMs - kickMs_ < kKickRefractoryMs)) return false;
        kickMs_ = nowMs;
        kicked_ = true;
        kickStartMs_ = confirmed ? clickStartMs_ : nowMs - bands_.delayMs(kBassTopThird);
        clicked_ = false;
        return true;
    }

    Bands bands_;
    float prevDb_[31] = {};          // each band's last reading
    OnsetDetector rangeDetector_[3];
    uint32_t clickMs_ = 0, clickStartMs_ = 0, kickMs_ = 0, kickStartMs_ = 0;
    uint8_t rangeFired_[3] = {};
    bool clicked_ = false, kicked_ = false;
};

}  // namespace mm
