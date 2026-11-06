#pragma once

#include "light/powerfunctions/draw.h"        // text, Canvas, fonts
#include "core/util/AudioFrame.h"              // AudioFrame: chroma, key, tuning
#include "core/services/AudioHarmony.h"        // formatNotes, formatKey

namespace mm {

/// The per-note hit state the audio-reactive effects share: when each note last struck, a tick and a bit a note, so a frame two ticks read is one hit, told by its `seq`.
struct NoteHits {
    uint16_t tick[kAudioNoteKeys] = {};  ///< when each note last struck, in 4 ms ticks
    uint8_t lit[(kAudioNoteKeys + 7) / 8] = {};   ///< a bit per note while its flash shows
    uint16_t lastSeq = 0;                ///< the frame the last hits came from

    /// Each note a new frame carries a hit for starts its flash now, and every flash older than `span` ends, so a note no effect draws cannot flash again when the tick wraps.
    void note(const AudioFrame& f, uint32_t ms, uint32_t span) MM_NONBLOCKING {
        for (uint8_t i = 0; i < kAudioNoteKeys; i++)
            if ((lit[i >> 3] >> (i & 7)) & 1u) glow(i, ms, span);   // glow ends an expired flash
        if (f.seq == lastSeq) return;
        lastSeq = f.seq;
        const uint16_t t = static_cast<uint16_t>(ms >> 2);
        for (uint8_t i = 0; i < kAudioNoteKeys && i < f.notesCount; i++)
            if (hitAt(f.notesHits, i)) {
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

/// The audio readout on row `y`: the strongest pitch classes at the left, "C E G", and the key with its tuning at the right, "Am +12".
inline void noteNames(const Canvas& cv, const AudioFrame& f, lengthType w, lengthType y) {
    char names[12];
    formatNotes(f.chroma, names, sizeof(names));
    text(cv, fonts::kFont4x6, names, 0, y, RGB{255, 255, 255});
    // The key at the right, short, with the tuning when it is off A440: "Am +12".
    char key[12];
    formatKey(f.key, key, sizeof(key), true);
    lengthType keyLen = 0;
    while (key[keyLen]) keyLen++;
    if (f.tuning != 0 && static_cast<size_t>(keyLen) + 5 < sizeof(key)) {
        const int cents = f.tuning < 0 ? -f.tuning : f.tuning;
        key[keyLen++] = ' ';
        key[keyLen++] = f.tuning < 0 ? '-' : '+';
        if (cents >= 10) key[keyLen++] = static_cast<char>('0' + cents / 10);
        key[keyLen++] = static_cast<char>('0' + cents % 10);
        key[keyLen] = 0;
    }
    const lengthType keyW = static_cast<lengthType>(keyLen * fonts::kFont4x6.width);
    if (f.key && w > keyW)
        text(cv, fonts::kFont4x6, key, static_cast<lengthType>(w - keyW), y, RGB{255, 255, 255});
}

}  // namespace mm::draw
