#pragma once

#include "light/powerfunctions/draw.h"        // text, Canvas, fonts
#include "core/util/AudioFrame.h"              // AudioFrame: chroma, key, tuning
#include "core/services/AudioHarmony.h"        // formatNotes, formatKey
#include "core/util/HarmonicSieve.h"           // NoteSet: the notes a reader keeps
#include "core/services/AudioChords.h"          // formatChord, inChord: the chord named

namespace mm {

/// The bar scales an audio effect picks from, in the `bands` options' order: the sixteen bands, the third octaves, the piano's keys.
enum BarScale : uint8_t { kBands16, kBandsThirds, kBandsNotes, kBandsCount };
/// The `bands` option names, in `BarScale` order.
inline constexpr const char* kBarScaleOptions[kBandsCount] = {"16", "thirds", "notes"};

/// One frame's bars at a scale: their magnitudes, their hits (none for the sixteen), how many, and the scale they came from.
struct AudioBars {
    const uint8_t* mag;    ///< one magnitude a bar, 0..255
    const uint8_t* hits;   ///< a bit a bar, set on the block it was struck; null for the sixteen
    uint8_t n;             ///< how many bars
    BarScale scale;        ///< the scale they came from, which may be coarser than the one asked for
};

/// The bars at the scale `asked`, falling back to the next coarser one a frame does not carry: a frame synced in the WLED format has the sixteen alone, a board with `harmony` off has no keys.
inline AudioBars barsOf(const AudioFrame& f, uint8_t asked) MM_NONBLOCKING {
    if (asked == kBandsNotes && f.notesCount > 0) return {f.notes, f.notesHits, f.notesCount, kBandsNotes};
    if (asked != kBands16 && f.thirdsCount > 0) return {f.thirds, f.thirdsHits, f.thirdsCount, kBandsThirds};
    return {f.bands, nullptr, 16, kBands16};
}

/// The per-note hit state the audio-reactive effects share: when each note last struck, a tick and a bit a note, so a frame two ticks read is one hit, told by its `seq`.
struct NoteHits {
    uint16_t tick[kAudioNoteKeys] = {};  ///< when each note last struck, in 4 ms ticks
    uint8_t lit[kHitBytes] = {};         ///< a bit per note while its flash shows
    uint16_t lastSeq = 0;                ///< the frame the last hits came from

    /// Each note a new frame carries a hit for, in `hits` when given, else the frame's own, and `keep` holds when given, starts its flash now, and every flash older than `span` ends, so a note no effect draws cannot flash again when the tick wraps.
    void note(const AudioFrame& f, uint32_t ms, uint32_t span, const NoteSet* keep = nullptr, const uint8_t* hits = nullptr) MM_NONBLOCKING {
        for (uint8_t i = 0; i < kAudioNoteKeys; i++)
            if ((lit[i >> 3] >> (i & 7)) & 1u) glow(i, ms, span);   // glow ends an expired flash
        if (f.seq == lastSeq) return;
        lastSeq = f.seq;
        const uint16_t t = static_cast<uint16_t>(ms >> 2);
        for (uint8_t i = 0; i < kAudioNoteKeys && i < f.notesCount; i++)
            if (hitAt(hits ? hits : f.notesHits, i) && (!keep || keep->has(i))) {
                tick[i] = t;
                lit[i >> 3] = static_cast<uint8_t>(lit[i >> 3] | (1u << (i & 7)));
            }
    }

    /// A struck note's flash, full on its hit and fading quadratically over `span`, so a time-dimmed panel still shows it until the last stretch; `note` ends it long before the 16-bit age wraps.
    uint8_t glow(uint8_t i, uint32_t ms, uint32_t span) MM_NONBLOCKING {
        const uint8_t bit = static_cast<uint8_t>(1u << (i & 7));
        if (!(lit[i >> 3] & bit)) return 0;
        const uint16_t t = static_cast<uint16_t>(ms >> 2);
        const uint32_t age = static_cast<uint32_t>(static_cast<uint16_t>(t - tick[i])) * 4u;   // the difference in 16 bits, so the wrap is the age
        if (span == 0 || age >= span) {
            lit[i >> 3] = static_cast<uint8_t>(lit[i >> 3] & ~bit);
            return 0;
        }
        return static_cast<uint8_t>(255u - 255u * age * age / (span * span));   // 255 * (1 - (age/span)^2); age <= span <= 1000, so age*age*255 fits a uint32
    }
};

}  // namespace mm

namespace mm::draw {

/// The key after `len` characters of `out`, two spaces apart from a chord before it, with the tuning when it is off A440: "C +12"; answers the new length.
inline lengthType appendKey(const AudioFrame& f, char* out, size_t size, lengthType len) {
    if (len) { out[len++] = ' '; out[len++] = ' '; }
    formatKey(f.key, out + len, size - static_cast<size_t>(len), true);
    while (out[len]) len++;
    if (f.tuning != 0 && static_cast<size_t>(len) + 5 < size) {
        const int cents = f.tuning < 0 ? -f.tuning : f.tuning;
        out[len++] = ' ';
        out[len++] = f.tuning < 0 ? '-' : '+';
        if (cents >= 10) out[len++] = static_cast<char>('0' + cents / 10);
        out[len++] = static_cast<char>('0' + cents % 10);
        out[len] = 0;
    }
    return len;
}

/// The audio readout on row `y`: the strongest pitch classes at the left, "C E G", and the chord and the key with its tuning at the right, "Am7  C +12".
inline void noteNames(const Canvas& cv, const AudioFrame& f, lengthType w, lengthType y) {
    char names[12];
    formatNotes(f.chroma, names, sizeof(names));
    text(cv, fonts::kFont4x6, names, 0, y, RGB{255, 255, 255});
    // The chord and the key at the right, short, the key with its tuning when it is off A440: "Am7  C +12".
    char key[20];
    formatChord(f.chordRoot, f.chordQuality, key, sizeof(key));
    lengthType keyLen = 0;
    while (key[keyLen]) keyLen++;
    if (f.key) keyLen = appendKey(f, key, sizeof(key), keyLen);
    const lengthType keyW = static_cast<lengthType>(keyLen * fonts::kFont4x6.width);
    if (keyLen && w > keyW)
        text(cv, fonts::kFont4x6, key, static_cast<lengthType>(w - keyW), y, RGB{255, 255, 255});
}

}  // namespace mm::draw
