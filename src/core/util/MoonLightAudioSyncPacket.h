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
/// The layout's version; a receiver refuses another one, and it changes only for a change no rule below absorbs.
constexpr uint8_t  kMlSyncVersion = 1;
/// The magic, the version and `seq`; it never grows, since what a packet adds is a section.
constexpr size_t   kMlSyncHeaderSize = 5;
/// A section's type byte and its 16-bit length, before its bytes.
constexpr size_t   kMlSyncSectionHeader = 3;

/// The section types this version knows. A sender may send them in any order; a receiver skips a type it does not know by its length.
enum MlSyncSection : uint8_t { kMlSectionCore, kMlSectionSmoothed, kMlSectionThirds, kMlSectionHarmony, kMlSectionNotes };

/// The core section's bytes.
constexpr size_t kMlSyncCoreSize = 11 + 16;
/// The smoothed bands' section's bytes.
constexpr size_t kMlSyncSmoothedSize = 16;
/// The harmony section's bytes.
constexpr size_t kMlSyncHarmonySize = 12 + 4;
/// The largest datagram: 508 bytes, the UDP payload every IPv4 host accepts (576 less the largest IP header and the UDP header), so a packet never fragments and a receiver reads every one whole.
constexpr size_t kMlSyncMaxDatagram = 508;
/// The largest packet this version builds, every section full.
constexpr size_t kMlSyncMaxPacket = kMlSyncHeaderSize + 5 * kMlSyncSectionHeader + kMlSyncCoreSize + kMlSyncSmoothedSize
                                  + (1 + sizeof(AudioFrame::thirds) + sizeof(AudioFrame::thirdsHits)) + kMlSyncHarmonySize
                                  + (1 + sizeof(AudioFrame::notes) + sizeof(AudioFrame::notesHits) + sizeof(AudioFrame::notesKept) + sizeof(AudioFrame::notesKeptHits));
static_assert(kMlSyncMaxPacket <= kMlSyncMaxDatagram, "a full frame fits the datagram the format allows");

/// A frame field as one byte, clamped, for the fields wider than their 0..255 range.
inline uint8_t mlSyncByte(uint16_t v) { return v > 255 ? 255 : static_cast<uint8_t>(v); }

/// Start a section of `type` and `n` bytes at `p`, and answer where its bytes go.
inline uint8_t* mlSyncSection(uint8_t* p, MlSyncSection type, size_t n) {
    p[0] = type;
    p[1] = static_cast<uint8_t>(n); p[2] = static_cast<uint8_t>(n >> 8);
    return p + kMlSyncSectionHeader;
}

/// Build a packet from a frame into `out`, at least kMlSyncMaxPacket bytes, and answer its size.
inline size_t buildMoonLightAudioSync(uint8_t* out, const AudioFrame& f) {
    out[0] = kMlSyncMagic[0];
    out[1] = kMlSyncMagic[1];
    out[2] = kMlSyncVersion;
    out[3] = static_cast<uint8_t>(f.seq); out[4] = static_cast<uint8_t>(f.seq >> 8);
    uint8_t* p = mlSyncSection(out + kMlSyncHeaderSize, kMlSectionCore, kMlSyncCoreSize);
    *p++ = mlSyncByte(f.level);
    *p++ = mlSyncByte(f.levelSmoothed);
    *p++ = f.gate;
    *p++ = f.gateOpen ? 1 : 0;
    *p++ = f.onset; *p++ = f.onsetLow; *p++ = f.onsetMid; *p++ = f.onsetHigh;
    *p++ = static_cast<uint8_t>(f.peakHz); *p++ = static_cast<uint8_t>(f.peakHz >> 8);
    *p++ = mlSyncByte(f.peakMag);
    std::memcpy(p, f.bands, 16); p += 16;

    p = mlSyncSection(p, kMlSectionSmoothed, kMlSyncSmoothedSize);   // this engine smooths, so every receiver shows its frame
    std::memcpy(p, f.bandsSmoothed, 16); p += 16;

    const uint8_t thirds = f.thirdsCount > sizeof(f.thirds) ? sizeof(f.thirds) : f.thirdsCount;
    if (thirds > 0) {
        p = mlSyncSection(p, kMlSectionThirds, 1 + thirds + sizeof(f.thirdsHits));
        *p++ = thirds;
        std::memcpy(p, f.thirds, thirds); p += thirds;
        std::memcpy(p, f.thirdsHits, sizeof(f.thirdsHits)); p += sizeof(f.thirdsHits);
    }

    const uint8_t notes = f.notesCount > sizeof(f.notes) ? sizeof(f.notes) : f.notesCount;
    if (notes > 0) {   // the keys are measured while the notes run, which the harmony section needs too
        p = mlSyncSection(p, kMlSectionHarmony, kMlSyncHarmonySize);
        std::memcpy(p, f.chroma, 12); p += 12;
        *p++ = f.key;
        *p++ = static_cast<uint8_t>(f.tuning);
        *p++ = f.harmonyChange ? 1 : 0;
        *p++ = f.bassNote;

        p = mlSyncSection(p, kMlSectionNotes, 1 + notes + sizeof(f.notesHits) + sizeof(f.notesKept) + sizeof(f.notesKeptHits));
        *p++ = notes;
        std::memcpy(p, f.notes, notes); p += notes;
        std::memcpy(p, f.notesHits, sizeof(f.notesHits)); p += sizeof(f.notesHits);
        std::memcpy(p, f.notesKept, sizeof(f.notesKept)); p += sizeof(f.notesKept);
        std::memcpy(p, f.notesKeptHits, sizeof(f.notesKeptHits)); p += sizeof(f.notesKeptHits);
    }
    return static_cast<size_t>(p - out);
}

/// One section's bytes, read field by field: a field the section is too short for stays as it was, zero in a fresh frame, so a section an older sender wrote gives the fields it has.
struct MlSyncFields {
    const uint8_t* s;   ///< the section's bytes
    size_t n;           ///< how many
    size_t at = 0;      ///< the next field's offset
    /// Copy the next `size` bytes into `dst` when the section holds them all; answers whether it did. A field missing ends the section, since every field after it is missing too.
    bool read(void* dst, size_t size) {
        if (at + size > n) { at = n; return false; }
        std::memcpy(dst, s + at, size);
        at += size;
        return true;
    }
    /// The next byte, 0 past the section's end.
    uint8_t byte() { return at < n ? s[at++] : 0; }
};

/// The core section into `f`.
inline void parseMlCore(MlSyncFields r, AudioFrame& f) {
    f.level = r.byte(); f.levelSmoothed = r.byte(); f.gate = r.byte(); f.gateOpen = r.byte() != 0;
    f.onset = r.byte(); f.onsetLow = r.byte(); f.onsetMid = r.byte(); f.onsetHigh = r.byte();
    uint8_t hz[2] = {};
    r.read(hz, 2);
    f.peakHz = static_cast<uint16_t>(hz[0] | (hz[1] << 8));
    f.peakMag = r.byte();
    r.read(f.bands, 16);
}

/// The harmony section into `f`.
inline void parseMlHarmony(MlSyncFields r, AudioFrame& f) {
    r.read(f.chroma, 12);
    f.key = r.byte();
    f.tuning = static_cast<int8_t>(r.byte());
    f.harmonyChange = r.byte() != 0;
    f.bassNote = r.byte();
}

/// A count-prefixed array of at most `max` bytes into `dst`, its count into `count`; false for a count past the array, a malformed section.
inline bool parseMlArray(MlSyncFields& r, uint8_t* dst, size_t max, uint8_t& count) {
    const uint8_t c = r.byte();
    if (c > max) return false;
    if (r.read(dst, c)) count = c;
    return true;
}

/// One section of `type` into `f`; false for a malformed one. A type this version does not know is skipped.
inline bool parseMlSection(uint8_t type, MlSyncFields r, AudioFrame& f) {
    switch (type) {
    case kMlSectionCore: parseMlCore(r, f); return true;
    case kMlSectionSmoothed: r.read(f.bandsSmoothed, 16); return true;
    case kMlSectionThirds:
        if (!parseMlArray(r, f.thirds, sizeof(f.thirds), f.thirdsCount)) return false;
        r.read(f.thirdsHits, sizeof(f.thirdsHits));
        return true;
    case kMlSectionHarmony: parseMlHarmony(r, f); return true;
    case kMlSectionNotes:
        if (!parseMlArray(r, f.notes, sizeof(f.notes), f.notesCount)) return false;
        r.read(f.notesHits, sizeof(f.notesHits));
        r.read(f.notesKept, sizeof(f.notesKept));
        r.read(f.notesKeptHits, sizeof(f.notesKeptHits));
        return true;
    default: return true;   // a type a later version added
    }
}

/// Parse a packet into `out`, its `seq` the sender's; false for another magic or version, a section past the datagram or a count past its array. `sections` gets a bit per known type carried; none is required.
inline bool parseMoonLightAudioSync(const uint8_t* pkt, size_t len, AudioFrame& out, uint16_t* sections = nullptr) {
    if (!pkt || len < kMlSyncHeaderSize || pkt[0] != kMlSyncMagic[0] || pkt[1] != kMlSyncMagic[1] || pkt[2] != kMlSyncVersion) return false;
    AudioFrame& f = out;   // written in place: a frame copy costs the stack as much as the packet
    f = AudioFrame{};
    f.seq = static_cast<uint16_t>(pkt[3] | (pkt[4] << 8));
    uint16_t seen = 0;
    for (size_t at = kMlSyncHeaderSize; at < len;) {
        if (at + kMlSyncSectionHeader > len) return false;
        const uint8_t type = pkt[at];
        const size_t n = static_cast<size_t>(pkt[at + 1] | (pkt[at + 2] << 8));
        at += kMlSyncSectionHeader;
        if (at + n > len) return false;
        if (!parseMlSection(type, MlSyncFields{pkt + at, n}, f)) return false;
        if (type <= kMlSectionNotes) seen = static_cast<uint16_t>(seen | (1u << type));
        at += n;
    }
    if (sections) *sections = seen;
    return true;
}

} // namespace mm
