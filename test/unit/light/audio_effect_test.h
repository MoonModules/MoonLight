#pragma once

/// The shared fixture for the audio-reactive effect tests: an effect on a grid with pixel readers, the seated audio service, the played frame, and the note-frame builders. One home for what the audio effect tests share.

#include "golden_frame.h"
#include "core/services/AudioService.h"
#include "light/util/Palette.h"
#include "light/powerfunctions/audioReadout.h"

#include <cstring>
#include <vector>

namespace mm {

/// The effect `E` on a `w` by `h` grid, with readers over the layer's pixel buffer.
template <typename E>
struct EffectOnGrid {
    Layouts layouts;   ///< the layout tree holding the grid
    GridLayout grid;   ///< the grid the effect draws on
    Layer layer;       ///< the layer holding the effect and its pixel buffer
    E e;               ///< the effect under test
    /// Put the effect on a `w` by `h` grid and apply the state.
    EffectOnGrid(int w, int h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&e);
        layer.applyState();
    }
    /// A light's brightness, its three channels summed.
    int at(int x, int y) const {
        const auto* px = layer.buffer().data();
        const size_t o = (static_cast<size_t>(y) * grid.width + x) * 3;
        return px[o] + px[o + 1] + px[o + 2];
    }
    /// One channel of a light.
    int ch(int x, int y, int c) const {
        const auto* px = layer.buffer().data();
        const size_t o = (static_cast<size_t>(y) * grid.width + x) * 3;
        return px[o + c];
    }
    /// The brightest light in rows `from` up to `to`.
    int brightest(int from, int to) const {
        return maxIn(0, grid.width, from, to);
    }
    /// The brightest light in a rectangle.
    int maxIn(int x0, int x1, int y0, int y1) const {
        int peak = 0;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) peak = std::max(peak, at(x, y));
        return peak;
    }
    /// The darkest light in a rectangle.
    int minIn(int x0, int x1, int y0, int y1) const {
        int dark = 1 << 30;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) dark = std::min(dark, at(x, y));
        return dark;
    }
    /// Where the brightest light sits, its column.
    int brightestColumn() const {
        int best = -1, peak = 0;
        for (int y = 0; y < grid.height; y++) for (int x = 0; x < grid.width; x++) if (at(x, y) > peak) { peak = at(x, y); best = x; }
        return best;
    }
    /// The columns of the white (255, 255, 255) lights in rows `from` up to `to`, empty when none.
    std::vector<int> whiteColumns(int from, int to) const {
        return whiteColumnsIn(0, grid.width, from, to);
    }
    /// The columns of the white lights in a rectangle, empty when none.
    std::vector<int> whiteColumnsIn(int x0, int x1, int y0, int y1) const {
        std::vector<int> out;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++)
            if (at(x, y) == 3 * 255) out.push_back(x);
        return out;
    }
};

/// The audio service seated as the frame source `latestFrame` reads.
inline AudioService& seat() {
    static AudioService audio;
    audio.defineControls();
    audio.setup();
    audio.tick();   // claims the seat latestFrame reads; the frames below are written straight into it
    return audio;
}

/// One frame carrying `f`'s hits at `ms`, then the hits cleared.
template <typename F>
void play(F& fx, AudioService& audio, const AudioFrame& f, uint32_t ms) {
    const uint16_t seq = audio.frameForTest().seq;
    audio.frameForTest() = f;
    audio.frameForTest().seq = static_cast<uint16_t>(seq + 1);   // a new frame, as a source numbers it
    platform::setTestNowMs(ms);
    fx.layer.tick();
    audio.frameForTest() = AudioFrame{};
    audio.frameForTest().seq = static_cast<uint16_t>(seq + 2);
}

/// A frame whose note `i` (A0 = 21, so A4 is 48) sounds at `mag`, struck when `hit`.
inline AudioFrame noteFrame(uint8_t i, uint8_t mag, bool hit) {
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    f.notes[i] = mag;
    if (hit) f.notesHits[i >> 3] = static_cast<uint8_t>(1u << (i & 7));
    return f;
}

/// A frame whose key `i` is struck at `mag`.
inline AudioFrame struck(uint8_t i, uint8_t mag = 0) { return noteFrame(i, mag, true); }

/// A frame whose note `midi` sounds at `mag`, struck when `hit`.
inline AudioFrame note(uint8_t midi, uint8_t mag, bool hit = false) { return noteFrame(static_cast<uint8_t>(midi - 21), mag, hit); }

/// A frame with every note sounding at 200 and struck.
inline AudioFrame allNotes() {
    AudioFrame f;
    f.notesCount = kAudioNoteKeys;
    for (uint8_t i = 0; i < kAudioNoteKeys; i++) f.notes[i] = 200;
    std::memset(f.notesHits, 0xFF, sizeof(f.notesHits));
    return f;
}

/// The effect survives a 1x1 and a 0x0 grid with every note sounding.
template <typename F>
void survivesTinyGrids(AudioService& audio) {
    for (int side : {1, 0}) {
        F fx(side, side);
        play(fx, audio, allNotes(), 1000);
        platform::setTestNowMs(1100);
        fx.layer.tick();
    }
}

/// The color a note sounding at `lvl` takes: its palette hue.
inline RGB expectedColor(uint8_t i, uint8_t lvl) {
    return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(i * 256u / kAudioNoteKeys), lvl);
}

}  // namespace mm
