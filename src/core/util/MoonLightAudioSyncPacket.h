#pragma once

#include "core/util/AudioFrame.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mm {

/// The one place the MoonLight audio-sync packet is built and parsed: the whole frame between MoonLight boards, one datagram a frame.
///
/// The layout, for MoonLight and for a third party, is [Audio sync format](https://moonmodules.org/MoonLight/reference/audio-sync.html). A unit test round-trips every field and pins its example's bytes.

/// The magic that starts every packet.
constexpr char     kMlSyncMagic[2] = {'M', 'L'};
/// The layout's version; a receiver refuses another one.
constexpr uint8_t  kMlSyncVersion = 1;
/// The magic, the version, the flags, `seq` and the section mask.
constexpr size_t   kMlSyncHeaderSize = 8;

/// The sections, as bits of the mask and in the order they follow the header.
enum MlSyncSection : uint8_t { kMlSectionCore, kMlSectionSmoothed, kMlSectionThirds, kMlSectionHarmony, kMlSectionNotes };

/// The core section's bytes.
constexpr size_t kMlSyncCoreSize = 11 + 16;
/// The smoothed bands' section's bytes.
constexpr size_t kMlSyncSmoothedSize = 16;
/// The harmony section's bytes.
constexpr size_t kMlSyncHarmonySize = 12 + 4;
/// The largest datagram the format allows: a sender keeps every packet within it, and a receiver reads up to it, so a section a later version adds reaches an older receiver whole.
constexpr size_t kMlSyncMaxDatagram = 256;
/// The largest packet this version builds, every section full.
constexpr size_t kMlSyncMaxPacket = kMlSyncHeaderSize + (1 + kMlSyncCoreSize) + (1 + kMlSyncSmoothedSize) + (1 + 1 + sizeof(AudioFrame::thirds) + sizeof(AudioFrame::thirdsHits))
                                  + (1 + kMlSyncHarmonySize) + (1 + 1 + sizeof(AudioFrame::notes) + sizeof(AudioFrame::notesHits));
static_assert(kMlSyncMaxPacket <= kMlSyncMaxDatagram, "a full frame fits the datagram the format allows");

/// A frame field as one byte, clamped, for the fields wider than their 0..255 range.
inline uint8_t mlSyncByte(uint16_t v) { return v > 255 ? 255 : static_cast<uint8_t>(v); }

/// Build a packet from a frame into `out`, at least kMlSyncMaxPacket bytes, and answer its size.
inline size_t buildMoonLightAudioSync(uint8_t* out, const AudioFrame& f) {
    uint16_t mask = 1u << kMlSectionCore;
    uint8_t* p = out + kMlSyncHeaderSize;

    *p++ = static_cast<uint8_t>(kMlSyncCoreSize);
    *p++ = mlSyncByte(f.level);
    *p++ = mlSyncByte(f.levelSmoothed);
    *p++ = f.gate;
    *p++ = f.gateOpen ? 1 : 0;
    *p++ = f.onset; *p++ = f.onsetLow; *p++ = f.onsetMid; *p++ = f.onsetHigh;
    *p++ = static_cast<uint8_t>(f.peakHz); *p++ = static_cast<uint8_t>(f.peakHz >> 8);
    *p++ = mlSyncByte(f.peakMag);
    std::memcpy(p, f.bands, 16); p += 16;

    mask |= 1u << kMlSectionSmoothed;   // this engine smooths, so every receiver shows its frame
    *p++ = static_cast<uint8_t>(kMlSyncSmoothedSize);
    std::memcpy(p, f.bandsSmoothed, 16); p += 16;

    const uint8_t thirds = f.thirdsCount > sizeof(f.thirds) ? sizeof(f.thirds) : f.thirdsCount;
    if (thirds > 0) {
        mask |= 1u << kMlSectionThirds;
        *p++ = static_cast<uint8_t>(1 + thirds + sizeof(f.thirdsHits));
        *p++ = thirds;
        std::memcpy(p, f.thirds, thirds); p += thirds;
        std::memcpy(p, f.thirdsHits, sizeof(f.thirdsHits)); p += sizeof(f.thirdsHits);
    }

    const uint8_t notes = f.notesCount > sizeof(f.notes) ? sizeof(f.notes) : f.notesCount;
    if (notes > 0) {   // the keys are measured exactly while `harmony` is on, which the harmony section needs too
        mask |= 1u << kMlSectionHarmony;
        *p++ = static_cast<uint8_t>(kMlSyncHarmonySize);
        std::memcpy(p, f.chroma, 12); p += 12;
        *p++ = f.key;
        *p++ = static_cast<uint8_t>(f.tuning);
        *p++ = f.harmonyChange ? 1 : 0;
        *p++ = f.bassNote;

        mask |= 1u << kMlSectionNotes;
        *p++ = static_cast<uint8_t>(1 + notes + sizeof(f.notesHits));
        *p++ = notes;
        std::memcpy(p, f.notes, notes); p += notes;
        std::memcpy(p, f.notesHits, sizeof(f.notesHits)); p += sizeof(f.notesHits);
    }

    out[0] = kMlSyncMagic[0];
    out[1] = kMlSyncMagic[1];
    out[2] = kMlSyncVersion;
    out[3] = 0;   // flags, none defined
    out[4] = static_cast<uint8_t>(f.seq); out[5] = static_cast<uint8_t>(f.seq >> 8);
    out[6] = static_cast<uint8_t>(mask); out[7] = static_cast<uint8_t>(mask >> 8);
    return static_cast<size_t>(p - out);
}

/// Parse a packet into a frame, its `seq` the sender's, and answer the section mask it carried: 0 for a packet refused, one without the core section or with a section past the datagram, `out` then unspecified.
inline uint16_t parseMoonLightAudioSync(const uint8_t* pkt, size_t len, AudioFrame& out) {
    if (!pkt || len < kMlSyncHeaderSize) return 0;
    if (pkt[0] != kMlSyncMagic[0] || pkt[1] != kMlSyncMagic[1] || pkt[2] != kMlSyncVersion) return 0;
    const uint16_t mask = static_cast<uint16_t>(pkt[6] | (pkt[7] << 8));
    if (!(mask & (1u << kMlSectionCore))) return 0;

    AudioFrame& f = out;   // written in place: a frame copy costs the stack as much as the packet
    f = AudioFrame{};
    f.seq = static_cast<uint16_t>(pkt[4] | (pkt[5] << 8));
    size_t at = kMlSyncHeaderSize;
    for (uint8_t bit = 0; bit < 16; bit++) {
        if (!(mask & (1u << bit))) continue;
        if (at >= len) return 0;
        const size_t n = pkt[at++];
        if (at + n > len) return 0;
        const uint8_t* s = pkt + at;
        at += n;
        switch (bit) {
        case kMlSectionCore:
            if (n < kMlSyncCoreSize) return 0;
            f.level = s[0]; f.levelSmoothed = s[1]; f.gate = s[2]; f.gateOpen = s[3] != 0;
            f.onset = s[4]; f.onsetLow = s[5]; f.onsetMid = s[6]; f.onsetHigh = s[7];
            f.peakHz = static_cast<uint16_t>(s[8] | (s[9] << 8));
            f.peakMag = s[10];
            std::memcpy(f.bands, s + 11, 16);
            break;
        case kMlSectionSmoothed:
            if (n < kMlSyncSmoothedSize) return 0;
            std::memcpy(f.bandsSmoothed, s, 16);
            break;
        case kMlSectionThirds: {
            const uint8_t count = n > 0 ? s[0] : 0;
            if (count > sizeof(f.thirds) || n < 1u + count + sizeof(f.thirdsHits)) return 0;
            f.thirdsCount = count;
            std::memcpy(f.thirds, s + 1, count);
            std::memcpy(f.thirdsHits, s + 1 + count, sizeof(f.thirdsHits));
            break;
        }
        case kMlSectionHarmony:
            if (n < kMlSyncHarmonySize) return 0;
            std::memcpy(f.chroma, s, 12);
            f.key = s[12];
            f.tuning = static_cast<int8_t>(s[13]);
            f.harmonyChange = s[14] != 0;
            f.bassNote = s[15];
            break;
        case kMlSectionNotes: {
            const uint8_t count = n > 0 ? s[0] : 0;
            if (count > sizeof(f.notes) || n < 1u + count + sizeof(f.notesHits)) return 0;
            f.notesCount = count;
            std::memcpy(f.notes, s + 1, count);
            std::memcpy(f.notesHits, s + 1 + count, sizeof(f.notesHits));
            break;
        }
        default: break;   // a section a later version added: skipped by its length
        }
    }
    return mask;
}

} // namespace mm
