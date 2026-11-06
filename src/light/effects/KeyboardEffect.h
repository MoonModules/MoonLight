#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

namespace detail {
/// Whether key `i` is black: the pitch classes C#, D#, F#, G# and A# of MIDI note 21 + i.
constexpr bool pianoIsBlack(uint8_t i) {
    const uint8_t pc = static_cast<uint8_t>((21 + i) % 12);
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
/// How many white keys sit below key `i`.
constexpr uint8_t pianoWhiteBefore(uint8_t i) {
    uint8_t k = 0;
    for (uint8_t j = 0; j < i; j++) if (!pianoIsBlack(j)) k++;
    return k;
}
/// The octaves the keys span.
constexpr uint8_t kPianoOctaves = (kAudioNoteKeys + 11) / 12;
}  // namespace detail

/// A piano keyboard across the lights in two halves: the top shows the selected octaves with each pressed key in its palette color, the bottom the same ivory and ebony picture with one small circle per octave row on every key.
/// @card KeyboardEffect.gif
///
/// The mic's semitone bands are the piano's keys from A0 up; `octave` and `octaves` pick the octaves the top half shows (`auto` lets the detected key pick the first shown key), ivory letters run along the bottom edge, each circle row on the bottom labeled with its octave at the left, and `noteNames` writes the strongest pitch classes and the key at the top corners.
///
/// @technical
///
/// ## Geometry and hits
///
/// Each ivory key takes an equal share of the width and the full half height; each ebony key sits over the boundary before it at three fifths of an ivory key's width and two thirds of its length. Bottom-half circles sit on fixed octave rows, always smaller than the key. The shared `NoteHits` keeps when each key was last struck, so a frame two ticks read is one hit, told by its `seq`.
class KeyboardEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// The keyboard is drawn on a plane; a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// How long a struck key glows, in tens of milliseconds.
    uint8_t decay = 100;
    /// How bright the ivory rests between notes, 0 for dark.
    uint8_t ivory = 128;
    /// The detected key picks the first shown key.
    bool autoKey = false;
    /// The first octave the keyboard shows.
    uint8_t octave = 2;
    /// How many octaves the keyboard shows, the rest only as circles.
    uint8_t octaves = 2;
    /// The strongest pitch classes top left, the key top right.
    bool noteNames = true;

    /// Publish how long a struck key glows, the ivory rest, the shown octaves, auto and the note names.
    void defineControls() override {
        controls_.addControl("decay", decay, 5, 100);
        controls_.addControl("ivory", ivory, 0, 255);
        controls_.addControl("auto", autoKey);
        controls_.addControl("octave", octave, 0, 6);
        controls_.setHidden(controls_.count() - 1, autoKey);   // defineControls reruns on every change, so this re-hides live
        controls_.addControl("octaves", octaves, 1, 7);
        controls_.addControl("noteNames", noteNames);
    }

    /// Draw the two halves from the keys' magnitudes and the hits' ages: @xref{geometry-and-hits}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginAudioTick(*this, decay, hits_);
        if (a.w == 0 || a.h == 0) return;
        const uint8_t shown = static_cast<uint8_t>(std::min<uint32_t>(kAudioNoteKeys, static_cast<uint32_t>(octaves) * 12u));
        uint8_t s0;
        if (autoKey && a.f->key) {
            const uint8_t rootPc = static_cast<uint8_t>((a.f->key - 1) % 12);
            const uint8_t offset = static_cast<uint8_t>((rootPc + 3) % 12);   // the root's key within an octave, A-based
            s0 = static_cast<uint8_t>(std::min<uint32_t>(36u + offset, kAudioNoteKeys - shown));   // the root in the middle octave, at the window's left
        } else {
            s0 = static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>(octave) * 12u, kAudioNoteKeys - shown));
        }
        if (a.h == 1) {
            // One row has no halves, ebony over ivory, letters or circles: each shown semitone takes its own span.
            for (uint8_t j = 0; j < shown; j++) {
                const uint8_t i = static_cast<uint8_t>(s0 + j);
                const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(j) * a.w / shown);
                const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(j + 1) * a.w / shown);
                const uint8_t g = hits_.glow(i, a.ms, a.span);
                const uint8_t lvl = std::max({ivory, magnitude(*a.f, i), g});
                if (x1 > x0 && lvl)
                    draw::fillRect(a.cv, x0, 0, static_cast<lengthType>(x1 - x0), 1,
                                   keyColor(*a.pal, i, lvl, g, detail::pianoIsBlack(i)));
            }
            return;
        }
        const lengthType topH = a.h / 2 > 0 ? static_cast<lengthType>(a.h / 2) : 1;
        const Half top{a.w, 0, topH, s0, shown, a.ms, a.span, a.f};
        const Half bot{a.w, topH, static_cast<lengthType>(a.h - topH), s0, shown, a.ms, a.span, a.f};
        drawKeyboard(a.cv, *a.pal, top, false, true);    // pressed keys colored
        drawKeyboard(a.cv, *a.pal, bot, true, false);    // the piano picture with circles
        if (a.h > fonts::kFont4x6.height) drawOverlays(a.cv, top, bot, a.h);
        if (noteNames && a.h > 1) draw::noteNames(a.cv, *a.f, a.w, 0);   // a single row has no room above the keys
    }

private:
    NoteHits hits_;   ///< when each key last struck and its flash

    /// What one keyboard half needs, so the drawing helpers stay under the parameter budget.
    struct Half {
        lengthType w, y0, kh;          ///< grid width, top row, height of this half
        uint8_t s0, shown;             ///< first shown key, how many
        uint32_t ms, span;             ///< now, hit flash span
        const AudioFrame* f;
    };

    /// One keyboard half: ivory keys across the width at the full half height, ebony over the boundaries at two thirds of it.
    void drawKeyboard(const draw::Canvas& cv, const Palette& pal, const Half& h, bool circles, bool colorKeys) MM_NONBLOCKING {
        const uint8_t kWhite = static_cast<uint8_t>(detail::pianoWhiteBefore(static_cast<uint8_t>(h.s0 + h.shown)) - detail::pianoWhiteBefore(h.s0));
        const lengthType whiteW = static_cast<lengthType>((h.w + kWhite - 1) / kWhite);
        const lengthType gap = whiteW >= 3 ? 1 : 0;   // a hairline between keys when they are wide enough
        for (uint8_t j = 0; j < h.shown; j++) {
            const uint8_t i = static_cast<uint8_t>(h.s0 + j);
            if (detail::pianoIsBlack(i)) continue;
            const uint8_t k = static_cast<uint8_t>(detail::pianoWhiteBefore(i) - detail::pianoWhiteBefore(h.s0));
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(k) * h.w / kWhite);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(k + 1) * h.w / kWhite);
            const lengthType kw = x1 - x0 > gap ? static_cast<lengthType>(x1 - x0 - gap) : 0;
            if (!kw) continue;
            const uint8_t g = hits_.glow(i, h.ms, h.span);
            const uint8_t lvl = colorKeys ? std::max({ivory, magnitude(*h.f, i), g}) : ivory;
            if (lvl) draw::fillRect(cv, x0, h.y0, kw, h.kh, colorKeys ? keyColor(pal, i, lvl, g, false) : scaled(RGB{255, 255, 255}, ivory));
            if (circles) drawCircles(cv, pal, h, i, x0, kw);
        }
        const lengthType blackW = whiteW * 3 / 5 > 0 ? static_cast<lengthType>(whiteW * 3 / 5) : 1;
        const lengthType blackH = h.kh * 2 / 3 > 0 ? static_cast<lengthType>(h.kh * 2 / 3) : 1;   // ebony keys two thirds as long as ivory
        for (uint8_t j = 0; j < h.shown; j++) {
            const uint8_t i = static_cast<uint8_t>(h.s0 + j);
            if (!detail::pianoIsBlack(i)) continue;
            const int32_t b = static_cast<int32_t>(static_cast<uint32_t>(detail::pianoWhiteBefore(i) - detail::pianoWhiteBefore(h.s0)) * h.w / kWhite);
            const int32_t x0 = b - static_cast<int32_t>(blackW) / 2 > 0 ? b - static_cast<int32_t>(blackW) / 2 : 0;
            const int32_t x1 = b + static_cast<int32_t>(blackW + 1) / 2 < static_cast<int32_t>(h.w) ? b + static_cast<int32_t>(blackW + 1) / 2 : static_cast<int32_t>(h.w);
            if (x1 <= x0) continue;
            const uint8_t g = hits_.glow(i, h.ms, h.span);
            const uint8_t lvl = colorKeys ? std::max({ivory, magnitude(*h.f, i), g}) : ivory;
            if (lvl) draw::fillRect(cv, static_cast<lengthType>(x0), h.y0, static_cast<lengthType>(x1 - x0), blackH,
                                    colorKeys ? keyColor(pal, i, lvl, g, true) : scaled(RGB{255, 255, 255}, static_cast<uint8_t>(ivory / 4u)));
            if (circles) drawCircles(cv, pal, h, i, static_cast<lengthType>(x0), static_cast<lengthType>(x1 - x0));
        }
    }

    /// One small circle per octave this pitch sounds in, stacked from the top of the key; always smaller than the key, so it drops on it.
    void drawCircles(const draw::Canvas& cv, const Palette& pal, const Half& h, uint8_t i, lengthType px0, lengthType pxW) MM_NONBLOCKING {
        if (!pxW) return;
        const lengthType slot = h.kh / detail::kPianoOctaves > 0 ? static_cast<lengthType>(h.kh / detail::kPianoOctaves) : 1;
        const lengthType r = std::min<lengthType>(pxW / 4, slot / 3);
        const lengthType cx = static_cast<lengthType>(px0 + pxW / 2);
        for (uint8_t o = 0; o < detail::kPianoOctaves; o++) {
            const uint16_t hk = static_cast<uint16_t>((i % 12) + 12u * o);   // this pitch in octave o
            if (hk >= kAudioNoteKeys) break;
            const uint8_t g = hits_.glow(static_cast<uint8_t>(hk), h.ms, h.span);
            const uint8_t lvl = std::max(magnitude(*h.f, static_cast<uint8_t>(hk)), g);
            if (!lvl) continue;
            const lengthType cy = static_cast<lengthType>(h.y0 + o * slot + slot / 2);
            if (cy + r >= static_cast<lengthType>(h.y0 + h.kh)) break;
            // The circle shrinks with its level, so the dim tail of the glow is a small dot, not a big black one.
            const lengthType rr = std::max<lengthType>(1, static_cast<lengthType>(r * lvl / 255));
            draw::fillCircle(cv, cx, cy, rr, colorFromPalette(pal, static_cast<uint8_t>(hk * 256u / kAudioNoteKeys), lvl));
        }
    }

    /// The ivory letter strip along the bottom edge and the octave numbers at the left of each circle row and the top half.
    void drawOverlays(const draw::Canvas& cv, const Half& top, const Half& bot, lengthType h) MM_NONBLOCKING {
        const uint8_t kWhite = static_cast<uint8_t>(detail::pianoWhiteBefore(static_cast<uint8_t>(top.s0 + top.shown)) - detail::pianoWhiteBefore(top.s0));
        const lengthType whiteW = static_cast<lengthType>((top.w + kWhite - 1) / kWhite);
        const lengthType gap = whiteW >= 3 ? 1 : 0;
        for (uint8_t j = 0; j < top.shown; j++) {
            const uint8_t i = static_cast<uint8_t>(top.s0 + j);
            if (detail::pianoIsBlack(i)) continue;
            const uint8_t k = static_cast<uint8_t>(detail::pianoWhiteBefore(i) - detail::pianoWhiteBefore(top.s0));
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(k) * top.w / kWhite);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(k + 1) * top.w / kWhite);
            const lengthType kw = x1 - x0 > gap ? static_cast<lengthType>(x1 - x0 - gap) : 0;
            if (kw) drawLetter(cv, i, x0, kw, static_cast<lengthType>(h - fonts::kFont4x6.height), RGB{0, 0, 0});
        }
        const lengthType slot = bot.kh / detail::kPianoOctaves > 0 ? static_cast<lengthType>(bot.kh / detail::kPianoOctaves) : 1;
        for (uint8_t o = 0; o < detail::kPianoOctaves; o++) {
            char num[2] = {static_cast<char>('0' + o), 0};
            int32_t ly = static_cast<int32_t>(bot.y0 + o * slot + slot / 2 - 3);
            if (ly < static_cast<int32_t>(bot.y0)) ly = static_cast<int32_t>(bot.y0);
            if (ly + 6 > static_cast<int32_t>(h)) ly = static_cast<int32_t>(h) - 6;
            draw::text(cv, fonts::kFont4x6, num, 0, static_cast<lengthType>(ly), RGB{255, 255, 255});
        }
        char sel[2] = {static_cast<char>('0' + top.s0 / 12), 0};
        int32_t ty = static_cast<int32_t>(top.kh / 2 - 3) > 0 ? static_cast<int32_t>(top.kh / 2 - 3) : 0;
        draw::text(cv, fonts::kFont4x6, sel, 0, static_cast<lengthType>(ty), RGB{255, 255, 255});
    }

    /// The key's note letter at `y`, centered in the key, when the key is wide enough for a glyph.
    static void drawLetter(const draw::Canvas& cv, uint8_t i, lengthType px0, lengthType pxW, lengthType y, RGB c) MM_NONBLOCKING {
        if (pxW < fonts::kFont4x6.width) return;
        const char* name = kPitchClassNames[(21 + i) % 12];
        const lengthType tw = static_cast<lengthType>((name[1] ? 2 : 1) * fonts::kFont4x6.width);
        const lengthType tx = static_cast<lengthType>(px0 + (pxW > tw ? (pxW - tw) / 2 : 0));
        draw::text(cv, fonts::kFont4x6, name, tx, y, c);
    }

    /// A key's measured magnitude, 0 where the frame measured none.
    static uint8_t magnitude(const AudioFrame& f, uint8_t i) MM_NONBLOCKING {
        return i < f.notesCount ? f.notes[i] : 0;
    }

    /// A key's color: its palette hue while its strike flashes, else ivory or ebony at its level.
    RGB keyColor(const Palette& pal, uint8_t i, uint8_t lvl, uint8_t g, bool black) const MM_NONBLOCKING {
        if (g > 0)
            return colorFromPalette(pal, static_cast<uint8_t>(i * 256u / kAudioNoteKeys), lvl);
        return scaled(RGB{255, 255, 255}, black ? static_cast<uint8_t>(lvl / 4u) : lvl);
    }

    static RGB scaled(RGB c, uint8_t k) MM_NONBLOCKING { return RGB{scale8(c.r, k), scale8(c.g, k), scale8(c.b, k)}; }
};

}  // namespace mm
