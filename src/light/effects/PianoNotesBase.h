#pragma once

#include "light/effects/AudioNotesBase.h"
#include "light/powerfunctions/playerHands.h"   // PianoHands: the keys two hands could play

namespace mm {

/// The base of the note effects a pianist's two hands play: under `notes` at hands the hands keep the keys they could reach, and with `chord` a hand holding a chord snaps to it, as playerHands.h models them.
class PianoNotesBase : public AudioNotesBase {
protected:
    hands::PianoHands hands_;   ///< where each hand is

    /// Two hands keep the keys they could play.
    void playedBy(const AudioFrame& f, const uint8_t* hits, NoteSet& set, uint32_t ms) MM_NONBLOCKING override { hands_.filter(f.notes, set, ms, hits); }

    /// Under `hands` a hand holding a chord snaps to it and a hand playing a line keeps its notes.
    void snapToChord(const AudioFrame& f, NoteSet& set) MM_NONBLOCKING override {
        if (notes != kNotesHands) { AudioNotesBase::snapToChord(f, set); return; }
        hands_.snapToChord(set, f.chordRoot, f.chordQuality);
    }
};

}  // namespace mm
