#pragma once

#include <cstdint>

#include "core/services/AudioBands.h"   // ThirdOctaveBank, OnsetDetector, the range edges
#include "core/util/AudioFrame.h"

namespace mm {

/// The onsets the analysis reads from its banks about every 5 ms: one per range, bass, mids and treble, one per third-octave band and one per note, each hit timed within a reading rather than within a block.
///
/// @technical
///
/// ## The onset signal
///
/// Scheirer's construction (1998): each band's log power, its rise kept and its fall dropped, summed over the range's bands, read from the bank's detectors about every 5 ms rather than from the display's bytes once a block. Each range decides by the flux against its own recent mean, as the any-hit onset does, so a hi-hat over a held bass fires the treble alone.
///
/// A reading comes on the first 64-sample chunk boundary 5 ms or more after the last: every 5.3 ms at 24 and 48 kHz, 187.5 a second, and every 5.8 ms at 22,050 and 44,100 Hz.
///
/// A range fires only on a rise of 10 dB in one reading, summed over its bands, over the detector's own margin: a hit rises that much, a bass filter still ringing after one a few.
///
/// A band counts only within 40 dB of the loudest, both its readings held at that reading's floor (Klapuri 1999): a log rise is the same in a band 50 dB under the music as in the loudest, so a hi-hat's faint bass rising out of the room's hiss would fire the bass. The gate is the block's own, applied as the block's hits are taken.
///
/// ## An onset per band
///
/// Each band makes the same decision on its own rise, with 40 ms or the band's delay between its hits, whichever is longer.
///
/// Strokes at 10 a second stay separate hits; a faster roll merges, its band falling only about 14 dB between strokes at 20 a second, through the bank's 10 ms detector, so the detector's own mean outgrows the next rise.
///
/// A band fires once a rise: after a hit it fires again only once its rise has fallen under a hit's 10 dB over its recent level, a Schmitt trigger's two thresholds, so a rise outlasting the refractory time is still one hit.
///
/// A tom is pitched, so each lands in its own band and a fill reads as a sweep across them.
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
/// ## A range re-arms on its fall
///
/// A range fires once a rise and fires again only once its bands fall back to half their climb from their lowest since they last began rising: one attack stays one hit, a roll's next stroke is a new one. The least gap is 40 ms: the drum walk keeps a kick and a hi-hat apart up to 20 hits a second and a snare up to 12.
///
/// ## One hit, one drum
///
/// A drum spills into the ranges beside its own: a snare's body reaches the bass and a kick's beater the mids. On an input that carries the drums alone, the Audio card's `instrument` at `drums`, a bass or mids onset is heard out for 40 ms and named by its rise over the bands as they stood before it: a kick rises at 40 to 80 Hz more than half as much as at 160 to 250 Hz.
///
/// A kick fires the bass, a snare or a tom the mids, never both; the treble fires on its own, a hi-hat beside a kick or a snare being the drummer's everyday, and the frame's `onset`, one per drum hit, fires as before.
///
/// On MDB Drums' isolated hits (Southall et al. 2017) the shape names 99% of the kicks and 80% of the snares rightly; inside the band a bass guitar fills 40 to 80 Hz under a snare and the shape names 60%, so a full mix, `instrument` at `mix`, keeps each range's own onset.
///
/// ## A new note
///
/// The semitone bank's bands make the same decision, one per piano key: a key's attack or a plucked string fires its own note, and a tom its pitch. A note held under a new one has stopped rising, so only the new one fires.
///
/// A key's band also has to stand over both its neighbors, as a sounding note does: an attack's click spreads across the keys around it, the note's own standing out. A semitone band is a quarter of a third wide, so its delay is four times as long, 105 ms at 110 Hz.
///
/// A key rising under a louder neighbor keeps its recent level where the rise began, so the rise counts once the key stands out: F4 struck as E4 rings rises within E4's leakage and stands over it only near the rise's top.
///
/// ## A strike on a sounding note
///
/// A string struck again while it rings rises only by what it lost since, a few dB, under a hit's 10. The strike's attack fires the third octaves' onsets, so a key that stands out and rises 5 dB within its delay of that attack fires, as a kick's bass confirms its click: the repeated strum and the key struck again.

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

    /// Each band's onset on this reading, every level held at `floor`, the last attack `sinceAttackMs` ago; false on the first reading, which takes the bank's delays.
    template <typename Bank>
    bool decide(const Bank& bank, uint8_t count, float floor, uint32_t nowMs, uint32_t sinceAttackMs = kNoAttack) {
        if (!primed_) {
            prime(bank, count);
            return false;
        }
        for (uint8_t i = 0; i < count; i++) decideBand(i, count, floor, nowMs, sinceAttackMs);
        return true;
    }

    /// Read `bank` at `nowMs` when a reading is due, the last attack `sinceAttackMs` ago: @xref{a-strike-on-a-sounding-note}.
    template <typename Bank>
    void update(const Bank& bank, uint32_t nowMs, uint32_t sinceAttackMs = kNoAttack) {
        if (!due(nowMs)) return;
        float floor;
        const uint8_t count = read(bank, floor);
        decide(bank, count, floor, nowMs, sinceAttackMs);
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
        for (uint8_t& r : rising_) r = 0;
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
    /// The rise that confirms an attack heard elsewhere first, half a hit's: a kick's bass after its click, a sounding key after its strike.
    static constexpr float kConfirmDb = 5.0f;
    /// No attack heard yet.
    static constexpr uint32_t kNoAttack = UINT32_MAX;
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

    /// Whether sounding key `i`, risen `rise` dB, confirms an attack `sinceAttackMs` ago that it has not fired for yet: @xref{a-strike-on-a-sounding-note}.
    bool struckAgain(uint8_t i, float rise, uint32_t nowMs, uint32_t sinceAttackMs) const {
        const bool answered = detector_[i].fired_ && nowMs - detector_[i].lastMs_ <= sinceAttackMs;   // fired since that attack
        return !answered && sinceAttackMs <= delayMs_[i] + kReadMs && rise >= kConfirmDb;
    }

    /// Band `i`'s rise against its recent level, and its detector's decision on it, or a pitched key's strike confirmed `sinceAttackMs` after an attack.
    void decideBand(uint8_t i, uint8_t count, float floor, uint32_t nowMs, uint32_t sinceAttackMs) {
        const float now = db_[i] > floor ? db_[i] : floor;
        const float recent = refDb_[i] > floor ? refDb_[i] : floor;
        const bool out = !Pitched || standsOut(i, count);
        follow(i, out);
        const float flux = now > recent && out ? (now - recent) * kFluxPerDb : 0.0f;
        const bool strike = Pitched && out && struckAgain(i, now - recent, nowMs, sinceAttackMs);
        if (trigger(i, flux, strike, nowMs)) fired_[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    }

    /// Band `i`'s recent level following its level: a one-pole over the band's own delay, its coefficient the backward-Euler step, @xref{an-onset-per-band}; held while the key rises under a louder neighbor, @xref{a-new-note}.
    void follow(uint8_t i, bool out) {
        if (out || db_[i] < refDb_[i]) refDb_[i] += static_cast<float>(kReadMs) / static_cast<float>(delayMs_[i] + kReadMs) * (db_[i] - refDb_[i]);
    }

    /// Band `i`'s detector on `flux`, once a rise, or on a `strike` confirmed elsewhere; answers whether it fired: @xref{an-onset-per-band}.
    bool trigger(uint8_t i, float flux, bool strike, uint32_t nowMs) {
        const uint8_t bit = static_cast<uint8_t>(1u << (i & 7));
        const uint16_t refractory = delayMs_[i] > kRefractoryMs ? delayMs_[i] : kRefractoryMs;
        const bool hit = detector_[i].exceeds(toByte(flux), 3, 2, kMinFlux) && !(rising_[i >> 3] & bit);
        const bool onset = (hit || strike) && detector_[i].fire(nowMs, refractory);
        if (flux < kMinFlux) rising_[i >> 3] &= static_cast<uint8_t>(~bit);   // the rise is over: armed for the next
        else if (onset) rising_[i >> 3] |= bit;
        return onset;
    }

    float db_[N] = {};               // each band's level this reading, held here rather than on the audio task's stack
    float refDb_[N] = {};            // each band's recent level, following it over the band's own delay
    uint16_t delayMs_[N] = {};       // each band's delay, read from the bank as the stream starts
    OnsetDetector detector_[N];
    uint8_t fired_[kBytes] = {};
    uint8_t rising_[kBytes] = {};    // a bit per band whose hit's rise still stands over a hit's: it fires again once it falls under
    uint32_t lastMs_ = 0;
    bool primed_ = false;
};

/// The third octaves' onsets per band and per range, bass, mids and treble: a hit in its own range, @xref{the-onset-signal|the rise} summed over the range's bands.
struct AudioOnsets {
    /// The 31 bands' own onsets.
    using Bands = BandOnsets<31, 0, false>;

    /// Whether the input carries the drums alone, so a bass or mids hit is named one drum: @xref{one-hit-one-drum}.
    bool drumInput = false;

    /// Read the bank once at least 5 ms of stream time have passed, at `nowMs`.
    void update(const ThirdOctaveBank& bank, uint32_t nowMs) {
        if (!bands_.due(nowMs)) return;
        float floor;
        const uint8_t count = bands_.read(bank, floor);
        const bool primed = bands_.decide(bank, count, floor, nowMs);
        float rangeRise[3] = {}, climb[3] = {};
        rangeRises(count, floor, rangeRise, climb);
        if (!primed) return;
        noteClick(count, nowMs);
        for (uint8_t r = 0; r < 3; r++) {
            const uint8_t flux = Bands::toByte(rangeRise[r] * Bands::kFluxPerDb);
            if (rangeFires(r, flux, rangeRise[r], climb[r], nowMs)) fired(r, flux, nowMs);
        }
        if (drum_.open) decideDrum(count, floor, nowMs);   // a hit held when the input changed is still named
    }

    /// When the last kick started, timed by its click where it had one: @xref{the-kick-and-its-click}.
    uint32_t kickStartMs() const { return kickStartMs_; }

    /// How long before `nowMs` the last hit across the ranges fired, Bands::kNoAttack before the first: what a sounding key's strike confirms, @xref{a-strike-on-a-sounding-note}.
    uint32_t sinceAttackMs(uint32_t nowMs) const { return hit_ ? nowMs - hitMs_ : Bands::kNoAttack; }

    /// The strongest onset each range fired, the drum hit they make as the frame's `onset`, and the bands that fired since the last take into `f`, none while the block's gate is shut, then clear for the next block.
    void take(AudioFrame& f, bool gateOpen) {
        f.onsetLow = gateOpen ? rangeFired_[0] : 0;
        f.onsetMid = gateOpen ? rangeFired_[1] : 0;
        f.onsetHigh = gateOpen ? rangeFired_[2] : 0;
        f.onset = gateOpen ? hitFired_ : 0;
        hitFired_ = 0;
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
        clickMs_ = clickStartMs_ = kickMs_ = kickStartMs_ = hitMs_ = 0;
        hitFired_ = 0;
        clicked_ = kicked_ = hit_ = false;
        drum_ = DrumHit{};
        for (float& d : dipDb_) d = -240.0f;
        for (bool& a : armed_) a = true;
        for (float& c : climbPeak_) c = 0.0f;
    }

private:
    static constexpr uint8_t kClickLowThird = 20, kClickTopThird = 24;   // 2 to 5 kHz, where a beater's click sits
    static constexpr uint32_t kKickRefractoryMs = 40;  // one kick, whether its click or its bass decided it: the re-arm keeps one attack one hit, so a double kick at 16 a second stays two
    static constexpr uint16_t kRangeRefractoryMs = 40; // a range's least gap between hits, 25 a second; the re-arm keeps one attack one hit: @xref{a-range-re-arms-on-its-fall}
    static constexpr uint8_t kTreble = 2;              // the hats' and cymbals' range, which sounds beside a kick or a snare
    static constexpr uint32_t kDrumWindowMs = 40;      // two frames: as long as a hit's bands take to show which drum it is, MDB Drums 2026-11-08

    /// A bass or mids hit being heard out: the bands before it, their peak since, its strongest onset: @xref{one-hit-one-drum}.
    struct DrumHit {
        float base[31] = {}, peak[31] = {};
        uint32_t startMs = 0;
        uint8_t flux = 0;
        bool open = false;
        void start(const float (&before)[31], uint32_t nowMs) {
            for (uint8_t b = 0; b < 31; b++) peak[b] = base[b] = before[b];
            startMs = nowMs;
            flux = 0;
            open = true;
        }
    };

    /// Follow the held hit's bands, and once its window has passed name its drum by the shape of its rise: a kick fires the bass, a snare or a tom the mids, never both: @xref{one-hit-one-drum}.
    void decideDrum(uint8_t count, float floor, uint32_t nowMs) {
        for (uint8_t b = 0; b < count && b < 31; b++) {
            const float now = bands_.level(b) > floor ? bands_.level(b) : floor;
            if (now > drum_.peak[b]) drum_.peak[b] = now;
        }
        if (nowMs - drum_.startMs < kDrumWindowMs) return;
        drum_.open = false;
        const auto rise = [&](uint8_t from, uint8_t to) {
            float sum = 0.0f;
            for (uint8_t b = from; b <= to; b++) sum += drum_.peak[b] - drum_.base[b];
            return sum;
        };
        const uint8_t r = rise(kKickLowThird, kKickTopThird) > kKickOverSnare * rise(kSnareLowThird, kSnareTopThird) ? 0 : 1;
        if (drum_.flux > rangeFired_[r]) rangeFired_[r] = drum_.flux;
    }
    static constexpr uint8_t kKickLowThird = 3, kKickTopThird = 6;     // 40 to 80 Hz, where a kick's body sits and a snare's does not
    static constexpr uint8_t kSnareLowThird = 9, kSnareTopThird = 11;  // 160 to 250 Hz, a snare's and a tom's body
    static constexpr float kKickOverSnare = 0.5f;                      // a kick rises at 40 to 80 Hz over half its rise at 160 to 250 Hz: 99% of MDB Drums' kicks, 80% of its snares the other way

    /// Whether range `r` fires on this reading's `flux`: once a rise, re-armed once it falls back to half its `climb`, @xref{a-range-re-arms-on-its-fall}; the bass as the kick decides, @xref{the-kick-and-its-click}.
    bool rangeFires(uint8_t r, uint8_t flux, float rise, float climb, uint32_t nowMs) {
        const bool rising = rangeDetector_[r].exceeds(flux, 3, 2, Bands::kMinFlux);
        bool fire = armed_[r] && rising && rangeDetector_[r].fire(nowMs, kRangeRefractoryMs);
        if (r == 0) fire = decideKick(fire, rise, nowMs);
        if (fire) { armed_[r] = false; climbPeak_[r] = climb; }
        else if (!armed_[r]) {   // re-armed once the range has fallen back to half its climb: a new hit, not the same one still rising
            if (climb > climbPeak_[r]) climbPeak_[r] = climb;
            else if (climb <= climbPeak_[r] / 2) armed_[r] = true;
        }
        return fire;
    }

    /// Range `r` fired with `flux`: the treble and a mix's ranges as their own onset, a drum input's bass and mids as a drum hit to name, @xref{one-hit-one-drum}.
    void fired(uint8_t r, uint8_t flux, uint32_t nowMs) {
        noteHit(flux, nowMs);
        if (r == kTreble || !drumInput) { if (flux > rangeFired_[r]) rangeFired_[r] = flux > 0 ? flux : 1; return; }
        if (!drum_.open) drum_.start(dipDb_, nowMs);
        if (flux > drum_.flux) drum_.flux = flux > 0 ? flux : 1;
    }

    /// Each range's rise since the last reading, its bands' rises summed, both readings held at this reading's floor so a band under it rises by nothing when the loudest jumps.
    void rangeRises(uint8_t count, float floor, float (&rise)[3], float (&climb)[3]) {
        for (uint8_t b = 0; b < count; b++) {
            const float now = bands_.level(b) > floor ? bands_.level(b) : floor;
            const float before = prevDb_[b] > floor ? prevDb_[b] : floor;
            if (now > before) rise[rangeOfThird(b)] += now - before;
            if (now < before) dipDb_[b] = now;                         // falling: the band's dip follows it
            else if (dipDb_[b] < floor) dipDb_[b] = floor;            // under the floor: held there, as `before` is, so a rise from under it climbs in full
            climb[rangeOfThird(b)] += now - dipDb_[b];
            prevDb_[b] = bands_.level(b);
        }
    }

    /// A click is a hit in the beater's bands on this reading; its start is the earliest of them.
    void noteClick(uint8_t count, uint32_t nowMs) {
        for (uint8_t b = kClickLowThird; b <= kClickTopThird && b < count; b++)
            if (bands_.firedAt(b, nowMs)) { clickMs_ = nowMs; clickStartMs_ = bands_.startMs(b); clicked_ = true; return; }
    }

    /// One drum hit across the ranges: a range firing within the bass's delay of the last is the same hit, its treble heard first and its bass a filter's delay later, so the frame's `onset` fires once per drum.
    void noteHit(uint8_t flux, uint32_t nowMs) {
        const bool same = hit_ && nowMs - hitMs_ <= bands_.delayMs(kBassTopThird) + Bands::kReadMs;
        if (!same) { hitMs_ = nowMs; hit_ = true; hitFired_ = flux > 0 ? flux : 1; }
    }

    /// The bass's onset `own`, or the bass starting to rise within its fastest band's delay of a click, once a kick: @xref{the-kick-and-its-click}.
    bool decideKick(bool own, float bassRise, uint32_t nowMs) {
        const uint32_t window = bands_.delayMs(kBassTopThird) + Bands::kReadMs;
        const bool confirmed = clicked_ && nowMs - clickMs_ <= window && bassRise >= Bands::kConfirmDb;
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
    uint32_t hitMs_ = 0;             // when the last drum hit started across the ranges
    uint8_t hitFired_ = 0;           // that hit's strength, waiting for the next take
    bool hit_ = false;               // whether a hit has started yet
    uint32_t clickMs_ = 0, clickStartMs_ = 0, kickMs_ = 0, kickStartMs_ = 0;
    uint8_t rangeFired_[3] = {};
    bool clicked_ = false, kicked_ = false;
    DrumHit drum_;                   // the bass or mids hit being heard out
    float dipDb_[31] = {-240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f,
                        -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f, -240.0f};   // each band's lowest since it last began to rise
    bool armed_[3] = {true, true, true};   // whether each range may fire: cleared by a hit, set once the range has fallen
    float climbPeak_[3] = {};        // each range's highest climb since its last hit
};

}  // namespace mm
