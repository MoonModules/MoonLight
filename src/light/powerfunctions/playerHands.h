#pragma once

#include "core/util/HarmonicSieve.h"   // NoteSet
#include "core/services/AudioChords.h"   // inChord: the chord a shape plays
#include "platform/nonblocking.h"      // MM_NONBLOCKING

#include <cstdint>

/// The hands that play the instruments the note effects draw, as constraints on which notes sound together and as a reading of how a string was played: the audio carries no hand, so nothing labels a note by the hand that played it.
///
/// @technical
///
/// ## Why constraints rather than labels
///
/// A pianist's two hands each reach about a tenth with five fingers, and a guitarist's fretting hand covers four frets with one note a string, so a set of notes no hands could play is mostly the engine reading overtones as notes.
///
/// The models keep the most of the sounding energy that hands could play, and move a hand only after the notes have left its reach for a while, as a player's hand stays put between phrases.
///
/// ## Where the player is playing now
///
/// A note struck in the last kRecentMs weighs 32 times its level more, so a loud strike outweighs all the held notes together and a hand goes where new notes are struck rather than where the loudest sound holds; a click with no level weighs nothing more: a solo above a loud chord or bass pulls the hand up the neck or the keyboard, as it pulls the player's.

namespace mm::hands {

/// The keys of `set` by weight, heaviest first, into `order`; answers how many. An insertion sort, since a frame sounds a handful of keys.
inline uint8_t byWeight(const uint16_t* weight, const NoteSet& set, uint8_t (&order)[kAudioNoteKeys]) MM_NONBLOCKING {
    uint8_t n = 0;
    for (uint8_t i = 0; i < kAudioNoteKeys; i++) {
        if (!set.has(i)) continue;
        uint8_t j = n++;
        for (; j > 0 && weight[order[j - 1]] < weight[i]; j--) order[j] = order[j - 1];
        order[j] = i;
    }
    return n;
}

/// The keys struck in the last kRecentMs, the few a player strikes in that time: @xref{where-the-player-is-playing-now}.
struct RecentStrikes {
    static constexpr uint8_t kSlots = 8;          ///< the strikes remembered, more than a fast solo lands in kRecentMs
    static constexpr uint32_t kRecentMs = 500;    ///< how long a struck note outweighs the held ones
    static constexpr uint16_t kStruck = 32;       ///< what a recent strike adds per level step: a loud one more than a hand's held keys together, a piano window's 17 at 256 at most, a click with no level nothing

    uint8_t key[kSlots] = {};   ///< the struck keys, oldest overwritten first
    uint8_t lvl[kSlots] = {};   ///< each strike's level
    uint32_t at[kSlots] = {};   ///< when each was struck
    uint8_t used = 0;           ///< a bit per slot holding a strike
    uint8_t next = 0;           ///< the slot the next strike takes

    /// Remember each key of `set` that `hits` strikes at `ms`, with its level in `level`; none without hits.
    void record(const uint8_t* level, const uint8_t* hits, const NoteSet& set, uint32_t ms) MM_NONBLOCKING {
        if (!hits) return;
        for (uint8_t i = 0; i < kAudioNoteKeys; i++) {
            if (!set.has(i) || !hitAt(hits, i) || level[i] == 0) continue;   // a click with no level is no note
            key[next] = i;
            lvl[next] = level[i];
            at[next] = ms;
            used = static_cast<uint8_t>(used | (1u << next));
            next = static_cast<uint8_t>((next + 1) % kSlots);
        }
    }

    /// Record this frame's strikes and answer each key's weight, written into `w`: the two steps a hand takes before it places its keys.
    const uint16_t* weighed(const uint8_t* level, const uint8_t* hits, const NoteSet& set, uint32_t ms, uint16_t (&w)[kAudioNoteKeys]) MM_NONBLOCKING {
        record(level, hits, set, ms);
        weigh(level, ms, w);
        return w;
    }

    /// Each key's weight into `weight`: its level, one more so a struck key without a level counts, and kStruck times its strike's level while that strike is recent, the latest strike of a key counting once.
    void weigh(const uint8_t* level, uint32_t ms, uint16_t (&weight)[kAudioNoteKeys]) const MM_NONBLOCKING {
        for (uint8_t i = 0; i < kAudioNoteKeys; i++) weight[i] = static_cast<uint16_t>(level[i] + 1u);
        NoteSet boosted;   // a key struck twice in kRecentMs counts its latest strike once
        for (uint8_t n = 0; n < kSlots; n++) {
            const uint8_t j = static_cast<uint8_t>((next + kSlots - 1 - n) % kSlots);   // newest first
            if (!((used >> j) & 1u) || ms - at[j] >= kRecentMs || boosted.has(key[j])) continue;
            boosted.add(key[j]);
            weight[key[j]] = static_cast<uint16_t>(weight[key[j]] + kStruck * lvl[j]);
        }
    }
};

/// A pianist's two hands: each plays at most kFingers keys within kSpan semitones, the left hand on the lower cluster and the right on the higher, their windows never overlapping: @xref{why-constraints-rather-than-labels}.
struct PianoHands {
    static constexpr uint8_t kSpan = 16;        ///< a tenth, the widest a hand reaches
    static constexpr uint8_t kFingers = 5;      ///< the keys one hand holds at once
    static constexpr uint32_t kMoveMs = 250;    ///< how long the notes must sit elsewhere before a hand moves

    int16_t window[2] = {-1, -1};   ///< each hand's lowest key, the left hand first; -1 before it played
    int16_t wanted[2] = {-1, -1};   ///< where each hand's best window has stood since `since`
    uint32_t since[2] = {};         ///< when that best window was first seen there
    RecentStrikes strikes;          ///< the keys struck lately, which outweigh the held ones

    /// Keep in `set` the keys the two hands play: each hand's window follows the heaviest cluster, a recent strike outweighing a held key, and keeps its kFingers heaviest keys: @xref{where-the-player-is-playing-now}.
    void filter(const uint8_t* level, NoteSet& set, uint32_t ms, const uint8_t* hits = nullptr) MM_NONBLOCKING {
        uint16_t w[kAudioNoteKeys];
        const uint16_t* weight = strikes.weighed(level, hits, set, ms, w);
        NoteSet taken;
        const int16_t first = bestWindow(weight, set, taken);
        if (first < 0) return;   // nothing sounds: the hands stay where they are
        markWindow(set, first, taken);
        const int16_t second = bestWindow(weight, set, taken);
        if (second >= 0) {
            settle(0, first < second ? first : second, set, ms);
            settle(1, first < second ? second : first, set, ms);
        } else {
            // One cluster: the hand that is nearer takes it, the other stays.
            const bool right = window[1] >= 0 && (window[0] < 0 ? false : distance(window[1], first) < distance(window[0], first));
            settle(right ? 1 : 0, first, set, ms);
        }
        apart();
        NoteSet kept;
        keepFingers(weight, set, window[0], kept);
        keepFingers(weight, set, window[1], kept);
        set = kept;
    }


    /// The keys a hand holds when it plays a chord rather than a line.
    static constexpr uint8_t kChordKeys = 3;

    /// Keep in `set` only the tones of the chord `root` and `quality` name under each hand holding `kChordKeys` keys or more, which plays the chord; a hand holding fewer plays a line and keeps its notes, whose passing notes are rarely chord tones.
    void snapToChord(NoteSet& set, uint8_t root, uint8_t quality) const MM_NONBLOCKING {
        for (uint8_t h = 0; h < 2; h++) {
            const int16_t w = window[h];
            if (w < 0 || keysIn(set, w) < kChordKeys) continue;
            for (int16_t i = w; i <= w + kSpan && i < kAudioNoteKeys; i++) {
                const uint8_t k = static_cast<uint8_t>(i);
                if (set.has(k) && !inChord(static_cast<uint8_t>((21 + k) % 12), root, quality)) set.remove(k);
            }
        }
    }

    /// How many keys of `set` lie in the hand window from `w`.
    static uint8_t keysIn(const NoteSet& set, int16_t w) MM_NONBLOCKING {
        uint8_t n = 0;
        for (int16_t i = w; i <= w + kSpan && i < kAudioNoteKeys; i++) n += set.has(static_cast<uint8_t>(i));
        return n;
    }

private:
    static int16_t distance(int16_t a, int16_t b) MM_NONBLOCKING { return static_cast<int16_t>(a > b ? a - b : b - a); }

    /// Keep the windows apart: the right hand's starts above where the left hand's ends, so overlapping notes are one hand's and the other stays clear; at the top of the keyboard the left hand gives way.
    void apart() MM_NONBLOCKING {
        if (window[0] < 0 || window[1] < 0 || window[1] > window[0] + kSpan) return;
        constexpr int16_t kTop = kAudioNoteKeys - 1 - kSpan;   // the highest window start
        window[1] = static_cast<int16_t>(window[0] + kSpan + 1);
        if (window[1] > kTop) { window[1] = kTop; window[0] = static_cast<int16_t>(kTop - kSpan - 1); }
        wanted[0] = window[0];
        wanted[1] = window[1];
    }

    /// The heaviest key of `set` not in `skip`, -1 when none is left.
    static int16_t strongest(const uint16_t* weight, const NoteSet& set, const NoteSet& skip) MM_NONBLOCKING {
        int16_t best = -1;
        for (uint8_t i = 0; i < kAudioNoteKeys; i++)
            if (set.has(i) && !skip.has(i) && (best < 0 || weight[i] > weight[best])) best = static_cast<int16_t>(i);
        return best;
    }

    /// The weight the keys of `set` not in `skip` hold in the window from `w`.
    static uint32_t windowSum(const uint16_t* weight, const NoteSet& set, const NoteSet& skip, int16_t w) MM_NONBLOCKING {
        uint32_t sum = 0;
        for (int16_t i = w; i <= w + kSpan && i < kAudioNoteKeys; i++)
            if (set.has(static_cast<uint8_t>(i)) && !skip.has(static_cast<uint8_t>(i))) sum += weight[i];
        return sum;
    }

    /// The window of kSpan semitones, around the heaviest key of `set` not in `skip`, that holds the most of their weight, centered on them where several hold it all, so a hand reaches either way; -1 when none is left.
    static int16_t bestWindow(const uint16_t* weight, const NoteSet& set, const NoteSet& skip) MM_NONBLOCKING {
        const int16_t anchor = strongest(weight, set, skip);
        if (anchor < 0) return -1;
        int16_t best = anchor, last = anchor;
        uint32_t bestSum = 0;
        for (int16_t w = anchor - kSpan < 0 ? 0 : static_cast<int16_t>(anchor - kSpan); w <= anchor; w++) {
            const uint32_t sum = windowSum(weight, set, skip, w);
            if (sum > bestSum) { bestSum = sum; best = last = w; }
            else if (sum == bestSum && w == last + 1) last = w;   // only a run of windows that all hold it, so the middle one does too
        }
        return static_cast<int16_t>((best + last) / 2);
    }

    /// Mark the keys of `set` in the window from `w` as taken.
    static void markWindow(const NoteSet& set, int16_t w, NoteSet& taken) MM_NONBLOCKING {
        for (int16_t i = w; i <= w + kSpan && i < kAudioNoteKeys; i++)
            if (set.has(static_cast<uint8_t>(i))) taken.add(static_cast<uint8_t>(i));
    }

    /// Whether any key of `set` lies in the window from `w`.
    static bool anyIn(const NoteSet& set, int16_t w) MM_NONBLOCKING {
        for (int16_t i = w; i <= w + kSpan && i < kAudioNoteKeys; i++)
            if (set.has(static_cast<uint8_t>(i))) return true;
        return false;
    }

    /// Move hand `h` to `target` when it has nothing in reach, or once the best window has stood there for kMoveMs.
    void settle(uint8_t h, int16_t target, const NoteSet& set, uint32_t ms) MM_NONBLOCKING {
        if (window[h] < 0 || !anyIn(set, window[h])) { window[h] = wanted[h] = target; since[h] = ms; return; }
        if (target == window[h]) { wanted[h] = target; return; }
        if (target != wanted[h]) { wanted[h] = target; since[h] = ms; return; }
        if (ms - since[h] >= kMoveMs) window[h] = target;
    }

    /// Add to `kept` the kFingers heaviest keys of `set` in the window from `w` that `kept` does not hold yet.
    static void keepFingers(const uint16_t* weight, const NoteSet& set, int16_t w, NoteSet& kept) MM_NONBLOCKING {
        if (w < 0) return;
        for (uint8_t f = 0; f < kFingers; f++) {
            int16_t best = -1;
            for (int16_t i = w; i <= w + kSpan && i < kAudioNoteKeys; i++) {
                const uint8_t k = static_cast<uint8_t>(i);
                if (set.has(k) && !kept.has(k) && (best < 0 || weight[k] > weight[best])) best = i;
            }
            if (best < 0) return;
            kept.add(static_cast<uint8_t>(best));
        }
    }
};

/// The most strings an instrument the hands play has: a guitar's six.
inline constexpr uint8_t kMaxStrings = 6;

/// A guitarist's fretting hand: one note a string, at the open string or within kReach frets of the position its first finger holds, the position the one that places the most weight, a recent strike outweighing a held note: the constraint form of tablature transcription (Radicioni and Lombardo 2005).
struct FrettingHand {
    static constexpr uint8_t kReach = 4;        ///< the frets the four fingers cover
    static constexpr uint32_t kMoveMs = 250;    ///< how long a better position must hold before the hand shifts; shifting at once puts 17% of GuitarSet's notes on their string against 24%, and 500 ms or 1 s place the same

    int8_t position = -1;   ///< the fret the first finger is at; -1 before the hand played
    int8_t wanted = -1;     ///< the best position since `since`
    uint32_t since = 0;     ///< when that best position was first seen
    int8_t note[kMaxStrings] = {-1, -1, -1, -1, -1, -1};   ///< the key each string plays, -1 for none
    uint8_t fret[kMaxStrings] = {};                         ///< the fret it is played at, 0 for the open string
    RecentStrikes strikes;                                  ///< the keys struck lately, which outweigh the held ones

    /// Place the keys of `order`, heaviest first, on `n` strings tuned to the MIDI notes `open`, one a string, at the open string or a fret from `p` within kReach and at most `frets`; answers the weight placed.
    static uint32_t place(const uint16_t* weight, const uint8_t* order, uint8_t m, const uint8_t* open, uint8_t n, uint8_t frets, uint8_t p,
                          int8_t (&outNote)[kMaxStrings], uint8_t (&outFret)[kMaxStrings]) MM_NONBLOCKING {
        for (uint8_t s = 0; s < kMaxStrings; s++) outNote[s] = -1;
        uint32_t energy = 0;
        for (uint8_t j = 0; j < m; j++) {
            const uint8_t k = order[j];
            for (uint8_t s = 0; s < n; s++) {
                if (outNote[s] >= 0) continue;
                const int16_t fr = static_cast<int16_t>(21 + k) - open[s];
                if (fr == 0 || (fr >= p && fr < p + kReach && fr <= frets)) {
                    outNote[s] = static_cast<int8_t>(k);
                    outFret[s] = static_cast<uint8_t>(fr);
                    energy += weight[k];
                    break;
                }
            }
        }
        return energy;
    }

    /// Keep in `set` the keys the hand places, and record where each string plays: the position follows the best one after kMoveMs, a string that places nothing keeps its last note for its glow to end: @xref{where-the-player-is-playing-now}.
    void update(const uint8_t* level, NoteSet& set, const uint8_t* open, uint8_t n, uint8_t frets, uint32_t ms, const uint8_t* hits = nullptr) MM_NONBLOCKING {
        uint16_t w[kAudioNoteKeys];
        const uint16_t* weight = strikes.weighed(level, hits, set, ms, w);
        uint8_t order[kAudioNoteKeys];
        const uint8_t m = byWeight(weight, set, order);
        int8_t tn[kMaxStrings];
        uint8_t tf[kMaxStrings];
        uint32_t bestE = 0;
        const int8_t bestP = bestPosition(weight, order, m, open, n, frets, bestE);
        set = NoteSet{};
        if (bestP < 0) return;   // nothing a string can play
        follow(bestP, bestE, position < 0 ? 0 : place(weight, order, m, open, n, frets, static_cast<uint8_t>(position), tn, tf), ms);
        place(weight, order, m, open, n, frets, static_cast<uint8_t>(position), tn, tf);
        record(tn, tf, n, set);
    }

    /// Finger the chord on `root` of `quality` as a shape, at the position nearest the hand's whose strings, the root as the bass, sound every chord tone on three or more: a guitarist voices a chord where the hand is.
    bool shape(uint8_t root, uint8_t quality, const uint8_t* open, uint8_t n, uint8_t frets) MM_NONBLOCKING {
        const int16_t from = position < 0 ? 1 : position;
        int16_t best = -1;
        uint16_t bestD = UINT16_MAX;
        for (uint8_t p = 1; p + kReach - 1 <= frets; p++) {
            int8_t tn[kMaxStrings];
            uint8_t tf[kMaxStrings];
            const uint16_t d = static_cast<uint16_t>(p > from ? p - from : from - p);
            if (d >= bestD || !shapeAt(root, quality, open, n, frets, p, tn, tf)) continue;   // a nearer one only, the lowest of equals
            best = p;
            bestD = d;
            for (uint8_t s = 0; s < kMaxStrings; s++) { note[s] = s < n ? tn[s] : -1; fret[s] = tf[s]; }
        }
        if (best < 0) return false;
        position = wanted = static_cast<int8_t>(best);
        return true;
    }

private:
    /// The chord's shape at position `p` into `tn` and `tf`, whether it sounds every chord tone on three strings or more.
    static bool shapeAt(uint8_t root, uint8_t quality, const uint8_t* open, uint8_t n, uint8_t frets, uint8_t p,
                        int8_t (&tn)[kMaxStrings], uint8_t (&tf)[kMaxStrings]) MM_NONBLOCKING {
        uint16_t covered = 0;
        uint8_t sounding = 0;
        bool rooted = false;
        for (uint8_t s = 0; s < kMaxStrings; s++) { tn[s] = -1; tf[s] = 0; }
        for (uint8_t s = 0; s < n; s++) {
            const int16_t f = chordFret(root, quality, open[s], frets, p, !rooted);
            if (f < 0 || open[s] + f - 21 >= kAudioNoteKeys) continue;   // past the keys a frame numbers
            const uint8_t pc = static_cast<uint8_t>((open[s] + f) % 12);
            tn[s] = static_cast<int8_t>(open[s] + f - 21);
            tf[s] = static_cast<uint8_t>(f);
            covered = static_cast<uint16_t>(covered | (1u << pc));
            rooted = rooted || pc == root;
            sounding++;
        }
        uint16_t tones = 0;
        for (uint8_t pc = 0; pc < 12; pc++) if (inChord(pc, root, quality)) tones = static_cast<uint16_t>(tones | (1u << pc));
        return covered == tones && sounding >= (n < 3 ? n : 3);
    }

    /// The lowest fret, open or within reach of `p`, at which a string tuned to `openNote` plays a chord tone, the root alone when `bass`; -1 for none.
    static int16_t chordFret(uint8_t root, uint8_t quality, uint8_t openNote, uint8_t frets, uint8_t p, bool bass) MM_NONBLOCKING {
        for (int16_t f = 0; f < p + kReach && f <= frets; f++) {
            if (f != 0 && f < p) continue;
            const uint8_t pc = static_cast<uint8_t>((openNote + f) % 12);
            if (bass ? pc == root : inChord(pc, root, quality)) return f;
        }
        return -1;
    }

    /// The position that places the most weight, of equals the nearest to the hand's, the movement cost of tablature transcription, the lowest before the hand played; -1 when none places any; `energy` takes its weight.
    int8_t bestPosition(const uint16_t* weight, const uint8_t* order, uint8_t m, const uint8_t* open, uint8_t n, uint8_t frets, uint32_t& energy) const MM_NONBLOCKING {
        int8_t tn[kMaxStrings];
        uint8_t tf[kMaxStrings];
        int8_t best = -1;
        const int16_t from = position;
        const auto far = [from](int16_t p) { return from < 0 ? p : (p > from ? p - from : from - p); };
        for (uint8_t p = 1; p + kReach - 1 <= frets; p++) {
            const uint32_t e = place(weight, order, m, open, n, frets, p, tn, tf);
            if (e > energy || (e == energy && e > 0 && far(p) < far(best))) { energy = e; best = static_cast<int8_t>(p); }
        }
        return best;
    }

    /// Move to `bestP` at once when the current position places nothing, else once it has placed a quarter more than the current `cur` for kMoveMs.
    void follow(int8_t bestP, uint32_t bestE, uint32_t cur, uint32_t ms) MM_NONBLOCKING {
        if (cur == 0) { position = wanted = bestP; since = ms; return; }
        if (bestP == position || bestE <= cur + cur / 4) { wanted = position; return; }
        if (bestP != wanted) { wanted = bestP; since = ms; return; }
        if (ms - since >= kMoveMs) position = bestP;
    }

    /// Record each placed string and put its key in `set`; a string placing nothing keeps its last note unless that key moved to another string.
    void record(const int8_t (&tn)[kMaxStrings], const uint8_t (&tf)[kMaxStrings], uint8_t n, NoteSet& set) MM_NONBLOCKING {
        for (uint8_t s = 0; s < n; s++) if (tn[s] >= 0) set.add(static_cast<uint8_t>(tn[s]));
        for (uint8_t s = 0; s < n; s++) {
            if (tn[s] >= 0) { note[s] = tn[s]; fret[s] = tf[s]; }
            else if (note[s] >= 0 && set.has(static_cast<uint8_t>(note[s]))) note[s] = -1;
        }
    }
};

/// How a string was played: the starts of its notes, so several strings starting together read as a strum with its direction, and each note's level over time, so a bowed note tells from a plucked one.
struct StringPlay {
    static constexpr uint32_t kStrumMs = 80;    ///< starts this close together are one strum
    static constexpr uint8_t kStrumMin = 3;     ///< the fewest strings a strum hits
    static constexpr uint32_t kPluckMs = 300;   ///< by when a plucked note has faded to a quarter, where a bowed one holds

    uint32_t startMs[kMaxStrings] = {};   ///< when each string's note started
    uint8_t peak[kMaxStrings] = {};       ///< the loudest its note has been
    uint8_t started = 0;                  ///< a bit per string that has a note started

    /// The last strum: the strings it hit, when, and its direction, +1 down (the low strings first), -1 up, 0 unknown.
    struct Strum {
        uint8_t strings = 0;   ///< a bit per string it hit
        uint32_t from = 0;     ///< its first start
        int8_t dir = 0;        ///< +1 down, -1 up, 0 when the starts fell together
    } strum;

    /// String `s`'s note starts at `ms` at `level`; when it joins kStrumMin or more starts within kStrumMs, those strings are a strum.
    void start(uint8_t s, uint32_t ms, uint8_t level) MM_NONBLOCKING {
        startMs[s] = ms;
        peak[s] = level;
        started = static_cast<uint8_t>(started | (1u << s));
        uint8_t count = 0;
        uint32_t from = ms;
        const uint8_t hit = recent(ms, count, from);
        if (count >= kStrumMin) strum = Strum{hit, from, direction(hit)};
    }

    /// The strings started within kStrumMs of `ms`, a bit each; `count` takes how many, `from` the earliest start.
    uint8_t recent(uint32_t ms, uint8_t& count, uint32_t& from) const MM_NONBLOCKING {
        uint8_t hit = 0;
        for (uint8_t t = 0; t < kMaxStrings; t++) {
            if (!((started >> t) & 1u) || ms - startMs[t] > kStrumMs) continue;
            hit = static_cast<uint8_t>(hit | (1u << t));
            count++;
            if (startMs[t] < from) from = startMs[t];
        }
        return hit;
    }

    /// The direction the strings in `hit` started in: +1 when the low strings started first, -1 the high ones, 0 when the starts fell together.
    int8_t direction(uint8_t hit) const MM_NONBLOCKING {
        int32_t order = 0;
        for (uint8_t a = 0; a < kMaxStrings; a++)
            for (uint8_t b = static_cast<uint8_t>(a + 1); b < kMaxStrings; b++)
                if (((hit >> a) & 1u) && ((hit >> b) & 1u)) order += startMs[b] > startMs[a] ? 1 : (startMs[b] < startMs[a] ? -1 : 0);
        return static_cast<int8_t>(order > 0 ? 1 : (order < 0 ? -1 : 0));
    }

    /// String `s`'s note sounds at `level`, which a bowed note can swell past its start.
    void sound(uint8_t s, uint8_t level) MM_NONBLOCKING { if (level > peak[s]) peak[s] = level; }

    /// Whether string `s` is bowed: its note started kPluckMs ago or more and still holds over a quarter of its peak.
    bool bowed(uint8_t s, uint32_t ms, uint8_t level) const MM_NONBLOCKING {
        return ((started >> s) & 1u) && ms - startMs[s] >= kPluckMs && level * 4u > peak[s];
    }
};

}  // namespace mm::hands
