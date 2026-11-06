#pragma once

#include <cstdint>

namespace mm {

/// How many semitone notes a frame carries, the piano's keys A0 to D#7: the one count `notes`, `notesHits` and every note reader size by.
constexpr uint8_t kAudioNoteKeys = 79;

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
/// | `level` | the instantaneous RMS, recomputed each audio block with no smoothing, so it snaps to a transient and a drum hit spikes at once. WLED calls this `volumeRaw` |
/// | `levelSmoothed` | an exponential moving average of `level`, which lags and rounds off sudden changes, so it breathes with the music instead of twitching. WLED calls this `volume` or `volumeSmth` |
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
/// `onset` is nonzero on the block where flux rose well over its own recent mean, so an effect flashing on it catches the drum within a block, 21 ms at 24 kHz. Landing on the beat needs a tempo tracker, the audio plan's last step.
struct AudioFrame {   // @xref{where-a-frame-comes-from}
    uint16_t level = 0;                ///< raw overall sound level (RMS), the instantaneous VU value: @xref{raw-against-smoothed}
    uint16_t levelSmoothed = 0;        ///< `level` under an exponential moving average
    uint16_t peakHz = 0;               ///< dominant frequency this frame, in Hz, or 0 for none
    uint16_t peakMag = 0;              ///< magnitude of that peak, 0..255, gating the `peakHz` update: @xref{why-peakmag-does-not-interoperate}
    uint8_t  bands[16] = {};           ///< 16 log-spaced band magnitudes, 0..255, raw
    uint8_t  bandsSmoothed[16] = {};   ///< the same bands under a peak-meter ballistic
    uint8_t  flux = 0;                 ///< spectral flux this block, 0..255
    uint8_t  onset = 0;                ///< 0, or the flux strength on the one block a hit was detected: @xref{onset-and-what-it-cannot-do}
    uint8_t  thirds[31] = {};          ///< the 31 third-octave bands, 20 Hz to 20 kHz, 0..255, raw
    uint8_t  thirdsCount = 0;          ///< how many of `thirds` this frame measured: 0 on a frame from audio sync, fewer than 31 at a rate that cannot reach 20 kHz
    uint8_t  gate = 0;                 ///< where the gate opens on the bands' 0..255 scale, at least 1; 0 on a frame no dynamics measured, from audio sync or simulated
    bool     gateOpen = false;         ///< whether the gate is open, so a band over zero is sound rather than the room
    uint8_t  chroma[12] = {};          ///< the twelve pitch classes, C first, 0..255 against the loudest over the last 200 ms; all 0 while gated, with `harmony` off, from audio sync or simulated
    bool     harmonyChange = false;    ///< true on the one block the harmony moved, a chord changing
    uint8_t  notes[kAudioNoteKeys] = {};   ///< the piano's keys from A0 to D#7, one band a semitone, 0..255 on the bands' scale
    uint8_t  notesCount = 0;           ///< how many of `notes` this frame measured: 0 with `harmony` off, from audio sync or simulated
    uint8_t  key = 0;                  ///< the key over the last 15 s: 0 none yet, 1..12 C to B major, 13..24 C to B minor
    int8_t   tuning = 0;               ///< how far the notes are tuned from A440, in cents
    uint8_t  bass = 0;                 ///< the bass, 20 to 125 Hz: its loudest band, 0..255 on the bands' scale
    uint8_t  mid = 0;                  ///< the mids, 160 Hz to 2 kHz, on the same scale
    uint8_t  treble = 0;               ///< the treble, 2.5 kHz up, on the same scale
    uint8_t  bassNote = 0;             ///< the bass line's note as a MIDI number, A4 being 69, or 0 while none stands out
    uint8_t  onsetLow = 0;             ///< 0, or the onset's strength on the block the bass had a hit: the kick
    uint8_t  onsetMid = 0;             ///< the same for the mids: the snare, the toms, a vocal's attack
    uint8_t  onsetHigh = 0;            ///< the same for the treble: the hi-hat, the cymbals
    uint8_t  thirdsHits[4] = {};       ///< a bit per third, set on the block it had a hit: a tom in its own band; read with `hitAt`
    uint8_t  notesHits[(kAudioNoteKeys + 7) / 8] = {};   ///< a bit per note of `notes`, set on the block it started: a key's attack, a plucked string
    uint16_t seq = 0;                  ///< counts the frames its source produced, so a consumer that reads one frame on two ticks takes its hits once
};

/// Whether bit `i` of a frame's hits is set, a band's or a note's.
constexpr bool hitAt(const uint8_t* bits, uint8_t i) { return (bits[i >> 3] >> (i & 7)) & 1u; }

} // namespace mm

