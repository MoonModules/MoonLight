#pragma once

#include "core/util/AudioFrame.h"

#include <cstdint>
#include <cstring>

namespace mm {

/// @defgroup wled_audio_sync WLED audio-sync wire format
/// @{
/// The one place this packet layout lives, built to broadcast our analyzed audio and parsed to drive effects from a peer's.
///
/// The contract is fixed upstream, so the bytes must be exact, and a unit test round-trips against a golden vector.
///
/// @moreinfo
///
/// ## The packet
///
/// Forty-four bytes on a fixed port, broadcast: a header string, two levels as floats and a peak flag.
/// Then the sixteen bands as bytes, and the dominant bin's magnitude and frequency.
/// Two padding runs are real wire bytes and go out as zero, and one reserved byte stays zero because the upstream may claim it later.
/// The offsets are hand-serialized rather than trusted to struct packing, and the floats are little-endian, which every target here and upstream shares.
///
/// ## Units convert at this boundary
///
/// The wire carries the upstream's magnitude units and the audio frame keeps ours, byte-scaled and conditioned by the same noise floor and gain as the bands.
/// The scale factor is exactly the divisor its own effects apply, so a byte of ours lands on the brightness a native source would produce.

constexpr uint16_t WLED_SYNC_PORT = 11988;
constexpr size_t   WLED_SYNC_PACKET_SIZE = 44;
// 6 chars incl NUL: "00002". v1 packets use "00001" (83 bytes): legacy, ignored.
constexpr char     WLED_SYNC_HEADER[6] = "00002";
constexpr size_t   WLED_SYNC_NUM_BANDS = 16;   // == AudioFrame bands + WLED NUM_GEQ_CHANNELS

/// The divisor the upstream's own effects apply, so a byte of ours matches a native sender's brightness.
inline constexpr float kWledMagScale = 16.0f;

/// Store a float in wire order, which on every target here is the raw bytes.
inline void wledPutFloatLE(uint8_t* p, float v) { std::memcpy(p, &v, 4); }
inline float wledGetFloatLE(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

/// Truncate a wire float into a frame field, clamped, since a foreign packet may carry anything.
inline uint16_t wledFloatToU16(float v) {
    if (!(v > 0.0f)) return 0;              // <= 0 or NaN
    if (v > 65535.0f) return 65535;
    return static_cast<uint16_t>(v);
}

/// Build a packet from a frame; `peak` is the beat flag the caller owns, and it answers the size.
inline size_t buildWledAudioSync(uint8_t out[WLED_SYNC_PACKET_SIZE], const AudioFrame& f,
                                 bool peak) {
    std::memset(out, 0, WLED_SYNC_PACKET_SIZE);        // zeroes header pad + both gaps
    std::memcpy(out, WLED_SYNC_HEADER, 6);             // "00002\0"
    wledPutFloatLE(out + 8,  static_cast<float>(f.level));          // sampleRaw
    wledPutFloatLE(out + 12, static_cast<float>(f.levelSmoothed));  // sampleSmth
    out[16] = peak ? 1 : 0;                            // samplePeak
    // The reserved byte stays zero, and bands clamp to 254, which a receiver may treat 255 as a sentinel against.
    for (size_t i = 0; i < WLED_SYNC_NUM_BANDS; i++)
        out[18 + i] = f.bands[i] > 254 ? 254 : f.bands[i];           // fftResult[16]
    wledPutFloatLE(out + 36, static_cast<float>(f.peakMag) * kWledMagScale);   // FFT_Magnitude
    wledPutFloatLE(out + 40, static_cast<float>(f.peakHz));         // FFT_MajorPeak
    return WLED_SYNC_PACKET_SIZE;
}

/// Parse and validate a packet into a frame; true only for a well-formed one of exactly the right size and header.
inline bool parseWledAudioSync(const uint8_t* pkt, size_t len, AudioFrame& out) {
    if (!pkt || len != WLED_SYNC_PACKET_SIZE) return false;
    if (std::memcmp(pkt, WLED_SYNC_HEADER, 6) != 0) return false;
    out.level         = wledFloatToU16(wledGetFloatLE(pkt + 8));
    out.levelSmoothed = wledFloatToU16(wledGetFloatLE(pkt + 12));
    // pkt[16] samplePeak is a hint the AudioFrame doesn't carry; pkt[17] is WLED's reserved2.
    std::memcpy(out.bands, pkt + 18, WLED_SYNC_NUM_BANDS);
    // Back into our units and clamped: an upstream magnitude can run past the range after the divide.
    const float mag = wledGetFloatLE(pkt + 36) / kWledMagScale;
    const uint16_t magU16 = wledFloatToU16(mag);
    out.peakMag = magU16 > 255 ? 255 : magU16;
    out.peakHz  = wledFloatToU16(wledGetFloatLE(pkt + 40));
    return true;
}

/// @}

} // namespace mm
