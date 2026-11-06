#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/audioReadout.h"   // NoteHits, draw::noteNames

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

/// The base of the effects that light the piano's notes as they sound: how long a struck note glows, the note-names readout, the hit state, and the start and end of their frame, written once.
class AudioNotesBase : public EffectBase {
public:
    /// How long a struck note glows, in tens of milliseconds.
    uint8_t decay = 100;
    /// The strongest pitch classes top left, the key top right.
    bool noteNames = true;

protected:
    NoteHits hits_;   ///< when each note last struck and its flash

    /// Publish the glow and the note names, after the effect's own controls.
    void addNoteControls() {
        controls_.addControl("decay", decay, 5, 100);
        controls_.addControl("noteNames", noteNames);
    }

    /// The frame's start: the surface filled with `background`, the scene, and the hits refreshed from the frame. The scene reads 0-sized on an empty grid.
    AudioScene beginNotes(RGB background) MM_NONBLOCKING {
        AudioScene s{canvas(), width(), height(), AudioService::latestFrame(), elapsed(), static_cast<uint32_t>(decay) * 10u, Palettes::active()};
        draw::fill(s.cv, background);
        if (s.w != 0 && s.h != 0) hits_.note(*s.f, s.ms, s.span);
        return s;
    }

    /// The frame's end: the note names on the top row, where a single row has no room.
    void endNotes(const AudioScene& a) const MM_NONBLOCKING {
        if (noteNames && a.h > 1) draw::noteNames(a.cv, *a.f, a.w, 0);
    }
};

}  // namespace mm
