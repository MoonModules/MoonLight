#pragma once

#include "core/util/AudioFrame.h"
#include "platform/nonblocking.h"   // MM_NONBLOCKING

#include <cstdint>

namespace mm {

/// The notes the core keeps of each frame: the set it keeps them in, a bit a key as the frame's strikes are held, the minimum note duration, the harmonic sieve and peak picking, run once a frame by keepNotes.
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
inline constexpr uint8_t kInstantSpread = 60;  ///< within this of the frame's loudest key a note is near it: a played note starts a median 17 under it, an attack's burst 102 (GuitarSet and URMP, 2026-11-08)

/// A minimum note duration, as transcription keeps one: a key near the frame's loudest counts from its second frame running, or on its first when it is loud, and a key far under it from its third, since an attack spreads a burst over the keys for a frame or a few, far under the note it starts, and a note outlasts it. A strike on a key not counted yet carries to the frame that counts it.
struct NoteConfirm {
    NoteSet heard;                  ///< last frame's sounding keys
    NoteSet heardTwice;             ///< the keys the last two frames sounded
    NoteSet counted;                ///< last frame's counted keys
    uint8_t hits[kHitBytes] = {};   ///< last frame's strikes

    /// Whether sounding key `i` at `level` counts, the frame's loudest at `top`: from its third frame running, or near the loudest from its second, or on its first when loud too.
    bool counts(uint8_t i, uint8_t level, uint8_t top) const MM_NONBLOCKING {
        if (heardTwice.has(i)) return true;
        const bool near = level + kInstantSpread >= top;
        return near && (heard.has(i) || level >= kInstantLevel);
    }

    /// The keys of `f` that count into `set`, and this frame's strikes into `struck`, a strike carried from a key the last frame heard but did not count.
    void apply(const AudioFrame& f, NoteSet& set, uint8_t (&struck)[kHitBytes]) MM_NONBLOCKING {
        const NoteSet now = soundingNotes(f);
        uint8_t top = 0;
        for (uint8_t i = 0; i < f.notesCount && i < kAudioNoteKeys; i++) top = f.notes[i] > top ? f.notes[i] : top;
        set = NoteSet{};
        for (uint8_t i = 0; i < kAudioNoteKeys; i++)
            if (now.has(i) && counts(i, f.notes[i], top)) set.add(i);
        for (uint8_t b = 0; b < kHitBytes; b++) {
            struck[b] = static_cast<uint8_t>(f.notesHits[b] | (hits[b] & ~counted.bits[b]));
            hits[b] = f.notesHits[b];
        }
        for (uint8_t b = 0; b < kHitBytes; b++) heardTwice.bits[b] = static_cast<uint8_t>(heard.bits[b] & now.bits[b]);
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

static_assert(sizeof(AudioFrame::notesKept) == kHitBytes && sizeof(AudioFrame::notesKeptHits) == kHitBytes, "a frame's kept notes are a NoteSet's bits");

/// The notes the core keeps of `f`, into `notesKept` and `notesKeptHits`: the keys that outlast an attack (`confirm` holds NoteConfirm's state), less overtones and leakage; once a frame, so every reader and receiver reads the same.
inline void keepNotes(AudioFrame& f, NoteConfirm& confirm) MM_NONBLOCKING {
    NoteSet set;
    uint8_t struck[kHitBytes];
    confirm.apply(f, set, struck);
    harmonicSieve(f.notes, f.notesCount, set, struck);
    leakageSieve(f.notes, f.notesCount, set);
    for (uint8_t b = 0; b < kHitBytes; b++) {
        f.notesKept[b] = set.bits[b];
        f.notesKeptHits[b] = static_cast<uint8_t>(struck[b] & set.bits[b]);
    }
}

}  // namespace mm
