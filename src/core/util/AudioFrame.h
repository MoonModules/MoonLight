#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm {

/// How many semitone notes a frame carries, the piano's keys A0 to D#7: the one count `notes`, `notesHits` and every note reader size by.
constexpr uint8_t kAudioNoteKeys = 79;
/// The block an input is read and analyzed in, a power of two: about 21 ms at 24 kHz, a frame of bands each.
inline constexpr size_t kAudioBlock = 512;

/// The whole contract between the audio analysis and the effects that react to it.
///
/// @technical
///
/// ## Where a frame comes from
///
/// `AudioService` produces one per audio block and the render tick hands the newest to the effects: a producer and consumers over a plain struct, so an effect never touches I2S or the filter bank.
///
/// A flat value type: a consumer copies it or holds a `const AudioFrame*`. Every field is a small integer, so an effect does integer math straight off it, as the hot path requires.
///
/// ## Raw against smoothed
///
/// Four of the fields come in pairs, and the choice between them is what an effect should feel like.
///
/// | Field | What it carries |
/// |-------|-----------------|
/// | `level` | this block's level, 0..255 through its own dynamics window and 0 while the gate is shut, with no smoothing, so it snaps to a transient and a drum hit spikes at once. WLED calls this `volumeRaw` |
/// | `levelSmoothed` | `level` through the same peak-meter ballistic as `bandsSmoothed`, fast on a rise and slow on a fall, so it breathes with the music instead of twitching. WLED calls this `volume` or `volumeSmth` |
/// | `bands` | this block's 16 band magnitudes with no smoothing, bass at `bands[0]` and treble at `bands[15]`: third-octave bands, 40 Hz to 10 kHz, as WLED-MM's GEQ picks them |
/// | `bandsSmoothed` | the same bands with a meter's ballistic, fast rise and slow fall, as IEC 60268-10 defines for a PPM. Use it for a spectrum display that should read as bars |
///
/// Reach for a raw field to catch a hit, and a smoothed one for anything that should swell rather than flash.
///
/// ## Why peakMag does not interoperate
///
/// WLED sends `peakMag` as a raw FFT magnitude reaching about 4096, its effects' thresholds sitting at 48 to 144 after a division by 16, so a 0..255 value reads there as near-silence. The audio service page carries the trade.
///
/// ## Onset and what it cannot do
///
/// `onset` is a drum heard on this block: the kick, the snare or a hat from the range hits, `onsetLow`, `onsetMid` and `onsetHigh`, the ranges one drum reaches within the bass's delay counted once. An effect flashing on it catches the drum within a block, 21 ms at 24 kHz.
///
/// A frame synced in the MoonLight format carries the sender's hits; one in the WLED format carries WLED's `samplePeak` as `onset`, and the ranges' hits when a MoonLight board sent it. A simulated frame's onset is the song's own kick. Landing on the beat needs a tempo tracker, the audio plan's step 6.
struct AudioFrame {   // @xref{where-a-frame-comes-from}
    uint16_t level = 0;                ///< this block's level, 0..255 through its dynamics window, 0 while gated: @xref{raw-against-smoothed}
    uint16_t levelSmoothed = 0;        ///< `level` through a peak meter's ballistic
    uint16_t peakHz = 0;               ///< dominant frequency this frame, in Hz, or 0 for none
    uint16_t peakMag = 0;              ///< magnitude of that peak, 0..255, gating the `peakHz` update: @xref{why-peakmag-does-not-interoperate}
    uint8_t  bands[16] = {};           ///< 16 log-spaced band magnitudes, 0..255, raw
    uint8_t  bandsSmoothed[16] = {};   ///< the same bands under a peak-meter ballistic
    uint8_t  flux = 0;                 ///< spectral flux this block, 0..255
    uint8_t  onset = 0;                ///< 0, or the strength of the range heard first, on the block a drum was heard: @xref{onset-and-what-it-cannot-do}
    uint8_t  thirds[31] = {};          ///< the 31 third-octave bands, 20 Hz to 20 kHz, 0..255, raw
    uint8_t  thirdsCount = 0;          ///< how many of `thirds` this frame measured: 0 on a frame synced in the WLED format, fewer than 31 at a rate that cannot reach 20 kHz
    uint8_t  gate = 0;                 ///< where the gate opens on the bands' 0..255 scale, at least 1; 0 on a frame no dynamics measured, from WLED sync or simulated
    bool     gateOpen = false;         ///< whether the gate is open, so a band over zero is sound rather than the room
    uint8_t  levelGate = 0;            ///< where the gate opens on `level`'s own scale, at least 1; 0 when none was measured, on a synced or simulated frame
    uint8_t  chroma[12] = {};          ///< the twelve pitch classes, C first, 0..255 against the loudest over the last 200 ms; all 0 while gated, with `harmony` off, from WLED sync or simulated
    bool     harmonyChange = false;    ///< true on the one block the harmony moved, a chord changing
    uint8_t  notes[kAudioNoteKeys] = {};   ///< the piano's keys from A0 to D#7, one band a semitone, 0..255 on the bands' scale
    uint8_t  notesCount = 0;           ///< how many of `notes` this frame measured: 0 with `harmony` off, from WLED sync or simulated
    uint8_t  key = 0;                  ///< the key over the last 15 s: 0 none yet, 1..12 C to B major, 13..24 C to B minor
    int8_t   tuning = 0;               ///< how far the notes are tuned from A440, in cents
    uint8_t  bass = 0;                 ///< the bass, 20 to 125 Hz: its loudest band, 0..255 on the bands' scale
    uint8_t  mid = 0;                  ///< the mids, 160 Hz to 2 kHz, on the same scale
    uint8_t  treble = 0;               ///< the treble, 2.5 kHz up, on the same scale
    uint8_t  bassNote = 0;             ///< the bass line's note as a MIDI number, A4 being 69, or 0 while none stands out
    uint8_t  chordRoot = 0;            ///< the chord's root, C = 0, which ChordTracker names on every board from `chroma`
    uint8_t  chordQuality = 0;         ///< the chord's ChordQuality, 0 for none: while gated, with `harmony` off, from WLED sync or simulated
    uint8_t  onsetLow = 0;             ///< 0, or the onset's strength on the block the bass had a hit: the kick
    uint8_t  onsetMid = 0;             ///< the same for the mids: the snare, the toms, a vocal's attack
    uint8_t  onsetHigh = 0;            ///< the same for the treble: the hi-hat, the cymbals
    uint8_t  thirdsHits[4] = {};       ///< a bit per third, set on the block it had a hit: a tom in its own band; read with `hitAt`
    uint8_t  notesHits[(kAudioNoteKeys + 7) / 8] = {};   ///< a bit per note of `notes`, set on the block it started: a key's attack, a plucked string
    uint16_t seq = 0;                  ///< counts the frames its source produced, so a consumer that reads one frame on two ticks takes its hits once

    /// Whether there is sound to show: the gate where one was measured, else, on a synced or simulated frame, a smoothed level over the room's hiss.
    constexpr bool heard() const { return gate ? gateOpen : levelSmoothed >= kHeardLevel; }
    /// The smoothed level a frame with no gate counts as sound from.
    static constexpr uint16_t kHeardLevel = 8;
};

/// Whether bit `i` of a frame's hits is set, a band's or a note's.
constexpr bool hitAt(const uint8_t* bits, uint8_t i) { return (bits[i >> 3] >> (i & 7)) & 1u; }

/// The hits of the frames a reader skips, merged so none is lost: the strongest onset per range, every band's and note's hit, any harmony change.
struct AudioHits {
    uint8_t onset = 0, low = 0, mid = 0, high = 0;   ///< the strongest of each onset seen
    uint8_t thirds[sizeof(AudioFrame::thirdsHits)] = {};   ///< every third's hit seen
    uint8_t notes[sizeof(AudioFrame::notesHits)] = {};     ///< every note's hit seen
    bool    change = false;                                ///< whether any frame's harmony moved

    /// Merge one frame's hits in.
    void add(const AudioFrame& f) {
        AudioHits h;
        h.onset = f.onset; h.low = f.onsetLow; h.mid = f.onsetMid; h.high = f.onsetHigh;
        std::memcpy(h.thirds, f.thirdsHits, sizeof(h.thirds));
        std::memcpy(h.notes, f.notesHits, sizeof(h.notes));
        h.change = f.harmonyChange;
        merge(h);
    }
    /// Merge another set of hits in: the strongest onset of each, every bit of both.
    void merge(const AudioHits& o) {
        onset = o.onset > onset ? o.onset : onset;
        low = o.low > low ? o.low : low;
        mid = o.mid > mid ? o.mid : mid;
        high = o.high > high ? o.high : high;
        for (size_t k = 0; k < sizeof(thirds); k++) thirds[k] |= o.thirds[k];
        for (size_t k = 0; k < sizeof(notes); k++) notes[k] |= o.notes[k];
        change = change || o.change;
    }
    /// Give a frame every hit merged so far, in place of its own.
    void applyTo(AudioFrame& f) const {
        f.onset = onset; f.onsetLow = low; f.onsetMid = mid; f.onsetHigh = high;
        for (size_t k = 0; k < sizeof(thirds); k++) f.thirdsHits[k] = thirds[k];
        for (size_t k = 0; k < sizeof(notes); k++) f.notesHits[k] = notes[k];
        f.harmonyChange = change;
    }
};

} // namespace mm

