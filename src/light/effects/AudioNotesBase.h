#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/audioReadout.h"   // NoteHits, draw::noteNames

#include <cstring>   // memcpy

namespace mm {

/// What a note effect's frame starts from, read once: the layer's surface filled with the background, the grid size, the latest audio frame, the effect's clock, the glow span and the active palette.
struct AudioScene {
    draw::Canvas cv;      ///< the layer's surface, filled with the background
    lengthType w, h;      ///< the grid size, both 0 on an empty grid
    const AudioFrame* f;  ///< the latest audio frame
    uint32_t ms;          ///< milliseconds since render start
    uint32_t span;        ///< the glow span from `decay`, in milliseconds
    const Palette* pal;   ///< the active palette
};

/// The base of the effects that light the piano's notes as they sound: which notes they show, how long a struck note glows, the note-names readout, the hit state, and the start and end of their frame, written once.
class AudioNotesBase : public EffectBase {
public:
    /// The note levels `notes` picks from, each keeping fewer: every note heard, without the overtones, or what the player's hands could play.
    enum NotesShown : uint8_t { kNotesAll, kNotesNoOvertones, kNotesHands, kNotesShownCount };
    /// All notes heard, without overtones, or what hands could play.
    uint8_t notes = kNotesHands;
    /// Keep only the notes of the chord being played.
    bool chord = false;
    /// How long a struck note glows, in tens of milliseconds.
    uint8_t decay = 100;
    /// The strongest pitch classes top left, the key top right.
    bool noteNames = true;

    /// Whether this frame keeps note `i`, for tests.
    bool keepsForTest(uint8_t i) const { return shown_.has(i); }

    /// Keep frame `f`'s notes once a frame, so the hands move by frames: under `all` every key heard, from `no overtones` on the notes the core keeps (`keepNotes`), then the hands and the chord's shape; the note harness runs it too.
    void keep(const AudioFrame& f, uint32_t ms) MM_NONBLOCKING {
        if (f.seq == filteredSeq_) return;
        filteredSeq_ = f.seq;
        const bool raw = notes == kNotesAll;   // the raw view: every key heard, an attack's burst included
        if (raw) shown_ = soundingNotes(f);
        else std::memcpy(shown_.bits, f.notesKept, kHitBytes);
        std::memcpy(struck_, raw ? f.notesHits : f.notesKeptHits, kHitBytes);
        if (notes == kNotesHands) playedBy(f, struck_, shown_, ms);
        if (chord && f.chordQuality != kChordNone) snapToChord(f, shown_);   // with no chord named, nothing to snap to
    }

protected:
    NoteHits hits_;   ///< when each note last struck and its flash
    NoteSet shown_;   ///< the notes this frame keeps
    uint8_t struck_[kHitBytes] = {};   ///< this frame's strikes: every key's under `all`, the kept keys' from `no overtones` on

    /// Publish which notes show, where the effect places it among its own controls.
    void addNotesSelect() {
        static constexpr const char* kNotesOptions[] = {"all", "no overtones", "hands"};
        controls_.addSelect("notes", notes, kNotesOptions, kNotesShownCount);
        controls_.addControl("chord", chord);
    }

    /// Publish the glow and the note names, after the effect's own controls.
    void addNoteControls() {
        controls_.addControl("decay", decay, 5, 100);
        controls_.addControl("noteNames", noteNames);
    }

    /// The instrument's hands, keeping in `set` the notes they could play; called on each new frame under `hands`, after the sieve.
    virtual void playedBy(const AudioFrame& f, const uint8_t* hits, NoteSet& set, uint32_t ms) MM_NONBLOCKING { (void)f; (void)hits; (void)set; (void)ms; }

    /// Keep in `set` only the notes of the frame's chord; an effect whose hands play chords their own way overrides it.
    virtual void snapToChord(const AudioFrame& f, NoteSet& set) MM_NONBLOCKING {
        for (uint8_t i = 0; i < kAudioNoteKeys; i++)
            if (set.has(i) && !inChord(static_cast<uint8_t>((21 + i) % 12), f.chordRoot, f.chordQuality)) set.remove(i);
    }

    /// Note `i`'s level when this frame keeps it, else 0.
    uint8_t level(const AudioFrame& f, uint8_t i) const MM_NONBLOCKING { return i < f.notesCount && shown_.has(i) ? f.notes[i] : 0; }

    /// The frame's start: the surface filled with `background`, the scene, and the hits refreshed from the frame. The scene reads 0-sized on an empty grid.
    AudioScene beginNotes(RGB background) MM_NONBLOCKING {
        AudioScene s{canvas(), width(), height(), AudioService::latestFrame(), elapsed(), static_cast<uint32_t>(decay) * 10u, Palettes::active()};
        draw::fill(s.cv, background);
        if (s.w == 0 || s.h == 0) return s;
        keep(*s.f, s.ms);
        hits_.note(*s.f, s.ms, s.span, &shown_, struck_);
        return s;
    }

    /// The frame's end: the note names on the top row, where a single row has no room.
    void endNotes(const AudioScene& a) const MM_NONBLOCKING {
        if (noteNames && a.h > 1) draw::noteNames(a.cv, *a.f, a.w, 0);
    }

private:
    uint16_t filteredSeq_ = 0;   ///< the frame `shown_` was kept from
};

}  // namespace mm
