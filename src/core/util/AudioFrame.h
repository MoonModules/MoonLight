#pragma once

#include <cstdint>

namespace mm {

/// The whole contract between the audio analysis and the effects that react to it.
///
/// @technical
///
/// ## Where a frame comes from
///
/// `AudioService` produces one per render tick and audio-reactive effects such as `AudioSpectrumEffect` consume it.
/// That follows the producer-and-consumer-over-a-plain-struct shape the codebase already uses, the way `PreviewDriver` writes pixels the HTTP server reads.
/// Because the struct is the entire contract, an effect never touches I2S or the FFT.
///
/// It is a flat value type with no ownership and no methods, so a consumer either copies it or holds a `const AudioFrame*` to the module's latest.
/// Every field is pre-scaled to a small integer so an effect does integer math straight off it, which the hot path requires; the float FFT magnitudes never leave the module.
///
/// ## Raw against smoothed
///
/// Four of the fields come in pairs, and the choice between them is what an effect should feel like.
///
/// | Field | What it carries |
/// |-------|-----------------|
/// | `level` | the instantaneous RMS, recomputed each audio block with no smoothing, so it snaps to a transient and a drum hit spikes at once. WLED calls this `volumeRaw` |
/// | `levelSmoothed` | an exponential moving average of `level`, which lags and rounds off sudden changes, so it breathes with the music instead of twitching. WLED calls this `volume` or `volumeSmth` |
/// | `bands` | this block's 16 log-spaced band magnitudes with no smoothing, bass at `bands[0]` and treble at `bands[15]` |
/// | `bandsSmoothed` | the same bands with a meter's ballistic, fast rise and slow fall, as IEC 60268-10 defines for a PPM. Use it for a spectrum display that should read as bars |
///
/// Reach for a raw field to catch a hit, and a smoothed one for anything that should swell rather than flash.
///
/// ## Why peakMag does not interoperate
///
/// `peakMag` is the one audio-sync field on a scale of its own.
/// WLED sends a raw FFT magnitude reaching about 4096, and its effects divide by 4 or 16 before use.
/// Their thresholds therefore sit around 48 to 144 after a division by 16.
/// A 0..255 value arrives below that squelch floor and reads as near-silence.
/// The audio service page carries the trade and the open decision.
///
/// ## Onset and what it cannot do
///
/// `flux` is the onset detection function, measuring how much the spectrum rose since the last block.
/// `onset` is zero except on the one block where a hit was detected, meaning flux well above its own recent mean, at most one per refractory window.
/// An effect that flashes on it catches the drum, within the block's latency of roughly 23 ms.
/// One that wants to land on the beat needs a tempo tracker, which the audio roadmap carries as backlog.
struct AudioFrame {
    uint16_t level = 0;                ///< raw overall sound level (RMS), the instantaneous VU value
    uint16_t levelSmoothed = 0;        ///< `level` under an exponential moving average
    uint16_t peakHz = 0;               ///< dominant frequency this frame, in Hz, or 0 for none
    uint16_t peakMag = 0;              ///< magnitude of that peak, 0..255, gating the `peakHz` update
    uint8_t  bands[16] = {};           ///< 16 log-spaced band magnitudes, 0..255, raw
    uint8_t  bandsSmoothed[16] = {};   ///< the same bands under a peak-meter ballistic
    uint8_t  flux = 0;                 ///< spectral flux this block, 0..255
    uint8_t  onset = 0;                ///< 0, or the flux strength on the one block a hit was detected
};

} // namespace mm

