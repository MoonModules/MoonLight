#pragma once

#include "core/util/AudioFrame.h"
#include "platform/nonblocking.h"   // MM_NONBLOCKING

#include <cstdint>

namespace mm {

/// The notes a reader keeps of a frame: the set it keeps them in, a bit a key as the frame's strikes are held, the minimum note duration, the harmonic sieve and peak picking.
inline constexpr uint8_t kHitBytes = (kAudioNoteKeys + 7) / 8;

/// A set of the piano's keys, a bit a key: the notes a reader keeps of a frame's `notes`.
struct NoteSet {
    uint8_t bits[kHitBytes] = {};   ///< a bit per key, A0 first

    /// Whether key `i` is in the set.
    bool has(uint8_t i) const MM_NONBLOCKING { return i < kAudioNoteKeys && ((bits[i >> 3] >> (i & 7)) & 1u); }
    /// Put key `i` in the set.
    void add(uint8_t i) MM_NONBLOCKING { if (i < kAudioNoteKeys) bits[i >> 3] = static_cast<uint8_t>(bits[i >> 3] | (1u << (i & 7))); }
    /// Take key `i` out of the set.
    void remove(uint8_t i) MM_NONBLOCKING { if (i < kAudioNoteKeys) bits[i >> 3] = static_cast<uint8_t>(bits[i >> 3] & ~(1u << (i & 7))); }
};

/// The keys a frame sounds or strikes: every key with a level or a hit this frame.
inline NoteSet soundingNotes(const AudioFrame& f) MM_NONBLOCKING {
    NoteSet s;
    for (uint8_t i = 0; i < f.notesCount && i < kAudioNoteKeys; i++)
        if (f.notes[i] > 0 || hitAt(f.notesHits, i)) s.add(i);
    return s;
}

inline constexpr uint8_t kInstantLevel = 32;   ///< a key this loud counts on its first frame; at its start a note reaches it more often than three times as often as an attack's burst (GuitarSet, 2026-11-07)

/// A minimum note duration, as transcription keeps one: a key heard counts from its second frame running, or on its first with a level of kInstantLevel or more, since an attack spreads a burst over the keys for a frame or a few and a note outlasts it. A strike on a key not counted yet carries to the frame that counts it.
struct NoteConfirm {
    NoteSet heard;                  ///< last frame's sounding keys
    NoteSet counted;                ///< last frame's counted keys
    uint8_t hits[kHitBytes] = {};   ///< last frame's strikes

    /// The keys of `f` that count into `set`, and this frame's strikes into `struck`, a strike carried from a key the last frame heard but did not count.
    void apply(const AudioFrame& f, NoteSet& set, uint8_t (&struck)[kHitBytes]) MM_NONBLOCKING {
        const NoteSet now = soundingNotes(f);
        set = NoteSet{};
        for (uint8_t i = 0; i < kAudioNoteKeys; i++)
            if (now.has(i) && (heard.has(i) || f.notes[i] >= kInstantLevel)) set.add(i);
        for (uint8_t b = 0; b < kHitBytes; b++) {
            struck[b] = static_cast<uint8_t>(f.notesHits[b] | (hits[b] & ~counted.bits[b]));
            hits[b] = f.notesHits[b];
        }
        heard = now;
        counted = set;
    }
};

inline constexpr uint8_t kSieveMargin = 16;   ///< how much weaker a note must be than a note below it to count as its overtone, on the bands' 0..255 scale
inline constexpr uint8_t kSieveHarmonics[] = {12, 19, 24};   ///< the 2nd, 3rd and 4th harmonics, in semitones above their note
/// Whether key `i` of `set` is the 2nd, 3rd or 4th harmonic of a kept key below it, weaker by over kSieveMargin, unless struck while that key is not.
inline bool overtoneOf(const uint8_t* level, const NoteSet& set, const uint8_t* hits, uint8_t i) MM_NONBLOCKING {
    const bool struck = hits && hitAt(hits, i) && level[i] >= kSieveMargin;   // a click with no level is no note
    for (uint8_t h : kSieveHarmonics) {
        if (i < h || !set.has(static_cast<uint8_t>(i - h))) continue;
        if (struck && !hitAt(hits, static_cast<uint8_t>(i - h))) continue;   // struck while the key below is not: a note of its own
        if (static_cast<uint16_t>(level[i]) + kSieveMargin <= level[i - h]) return true;
    }
    return false;
}

/// The harmonic sieve (Klapuri 2003): each overtoneOf a kept key leaves `set`, bottom up, so an overtone's overtones go too; a key struck alone stays, as harmonics start with their note (common onset, Bregman 1990).
inline void harmonicSieve(const uint8_t* level, uint8_t count, NoteSet& set, const uint8_t* hits = nullptr) MM_NONBLOCKING {
    for (uint8_t i = 0; i < count && i < kAudioNoteKeys; i++)
        if (set.has(i) && overtoneOf(level, set, hits, i)) set.remove(i);
}

inline constexpr uint8_t kLeakMargin = 16;   ///< how much weaker a key must be than a neighbor to count as its leakage, on the bands' 0..255 scale
inline constexpr uint8_t kLeakReach = 2;     ///< the semitones either side a strong key leaks into

/// Whether key `j` of `set` is louder than key `i` by kLeakMargin or more, so `i` beside it is its leakage.
inline bool leaksFrom(const uint8_t* level, uint8_t count, const NoteSet& set, uint8_t i, int16_t j) MM_NONBLOCKING {
    return j >= 0 && j < count && set.has(static_cast<uint8_t>(j)) && static_cast<uint16_t>(level[i]) + kLeakMargin <= level[j];
}

/// Peak picking, the sieve's second stage: a key within kLeakReach semitones of a key stronger by kLeakMargin is its leakage and leaves `set`; neighbors of like strength both stay, as a played second does.
inline void leakageSieve(const uint8_t* level, uint8_t count, NoteSet& set) MM_NONBLOCKING {
    const NoteSet before = set;   // compared against the keys as they were, so the order of removal does not matter
    for (uint8_t i = 0; i < count && i < kAudioNoteKeys; i++) {
        if (!before.has(i)) continue;
        for (int16_t d = 1; d <= kLeakReach; d++)
            if (leaksFrom(level, count, before, i, i - d) || leaksFrom(level, count, before, i, i + d)) { set.remove(i); break; }
    }
}

}  // namespace mm
