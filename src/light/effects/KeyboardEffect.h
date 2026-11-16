#pragma once

#include "light/effects/PianoNotesBase.h"

#include <array>

namespace mm {

namespace detail {
/// Whether key `i` is black: the pitch classes C#, D#, F#, G# and A# of MIDI note 21 + i.
constexpr bool pianoIsBlack(uint8_t i) {
    const uint8_t pc = static_cast<uint8_t>((21 + i) % 12);
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
/// How many white keys sit below each key, one past the last included: a table in flash, read per key per frame.
constexpr std::array<uint8_t, kAudioNoteKeys + 1> kPianoWhiteBefore = [] {
    std::array<uint8_t, kAudioNoteKeys + 1> t{};
    for (uint8_t i = 0; i < kAudioNoteKeys; i++) t[i + 1] = static_cast<uint8_t>(t[i] + (pianoIsBlack(i) ? 0 : 1));
    return t;
}();
/// The octaves the keys span.
constexpr uint8_t kPianoOctaves = (kAudioNoteKeys + 11) / 12;
}  // namespace detail

/// A piano keyboard across the lights in two halves: the top shows the selected octaves with each pressed key in its palette color, the bottom the same ivory and ebony picture with one small circle per octave row on every key.
/// @card KeyboardEffect.gif
///
/// The mic's semitone bands are the piano's keys from A0 up; `octave` and `octaves` pick the octaves the top half shows (`auto` lets the detected key pick the first shown key), ivory letters run along the bottom edge, each circle row on the bottom labeled with its octave at the left, and `noteNames` writes the strongest pitch classes and the key at the top corners.
///
/// With `chord` on, every tone of the named chord glows dim in its palette hue across the keys shown, and the played keys stay bright over it.
///
/// `notes` on `hands` shows what two hands could play, at most five keys a hand, each hand within a tenth: the top half follows the left hand's reach and the bottom half the right's, in place of the octaves and the circles, so `auto`, `octave` and `octaves` hide.
///
/// @technical
///
/// ## Geometry and hits
///
/// Each ivory key takes an equal share of the width and the full half height; each ebony key sits over the boundary before it at three fifths of an ivory key's width and two thirds of its length. Bottom-half circles sit on fixed octave rows, always smaller than the key. The shared `NoteHits` keeps when each key was last struck, so a frame two ticks read is one hit, told by its `seq`.
class KeyboardEffect : public PianoNotesBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// The keyboard is drawn on a plane; a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// How bright the ivory rests between notes, 0 for dark.
    uint8_t ivory = 128;
    /// The detected key picks the first shown key.
    bool autoKey = true;
    /// The first octave the keyboard shows.
    uint8_t octave = 2;
    /// How many octaves the keyboard shows, the rest only as circles.
    uint8_t octaves = 2;

    /// Publish the ivory rest, which notes show, the shown octaves and auto, then the glow and the note names; the hands place the halves themselves, so the octave controls hide under `hands`.
    void defineControls() override {
        controls_.addControl("ivory", ivory, 0, 255);
        addNotesSelect();
        const bool byHands = notes == kNotesHands;
        controls_.addControl("auto", autoKey);
        controls_.setHidden(controls_.count() - 1, byHands);
        controls_.addControl("octave", octave, 0, 6);
        controls_.setHidden(controls_.count() - 1, byHands || autoKey);   // defineControls reruns on every change, so this re-hides live
        controls_.addControl("octaves", octaves, 1, 7);
        controls_.setHidden(controls_.count() - 1, byHands);
        addNoteControls();
    }

    /// Draw the two halves from the keys' magnitudes and the hits' ages: @xref{geometry-and-hits}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginNotes(RGB{0, 0, 0});
        if (a.w == 0 || a.h == 0) return;
        const Keys k = layout(a);
        if (a.h == 1) {
            drawRow(a, k);
            return;
        }
        const lengthType topH = a.h / 2 > 0 ? static_cast<lengthType>(a.h / 2) : 1;
        const lengthType botH = static_cast<lengthType>(a.h - topH);
        if (notes == kNotesHands) {   // the left hand's reach on top, the right's below
            const Keys left = handKeys(a, 0), right = handKeys(a, 1);
            drawKeyboard(a, left, 0, topH, false, true);
            drawKeyboard(a, right, topH, botH, false, true);
            drawHandLabels(a, left, 0, topH);
            drawHandLabels(a, right, topH, botH);
            endNotes(a);
            return;
        }
        drawKeyboard(a, k, 0, topH, false, true);      // pressed keys colored
        drawKeyboard(a, k, topH, botH, true, false);   // the piano picture with circles
        if (a.h > fonts::kFont4x6.height) drawOverlays(a, k, topH, botH);
        endNotes(a);
    }

private:
    static constexpr uint8_t kGuideLevel = 72;   ///< how bright a chord tone glows when nobody plays it
    static constexpr uint8_t kHandRest[2] = {15, 39};   ///< where each hand's half starts before it played: C2 and C4 as keys from A0

    /// The shown keys' layout across the width, worked out once a frame.
    struct Keys {
        uint8_t s0, shown;       ///< first shown key, how many
        uint8_t white, base;     ///< ivory keys shown, ivory keys below the first shown
        lengthType w, whiteW, gap;   ///< grid width, one ivory key's width, the hairline between keys

        /// Where ivory boundary `b` falls, counted from the first shown ivory key.
        int32_t edge(uint32_t b) const MM_NONBLOCKING { return static_cast<int32_t>(b * w / white); }
        /// Ivory key `i`'s first column and width, the width 0 when the key is too narrow.
        void ivory(uint8_t i, lengthType& x0, lengthType& kw) const MM_NONBLOCKING {
            const uint32_t b = static_cast<uint32_t>(detail::kPianoWhiteBefore[i] - base);
            x0 = static_cast<lengthType>(edge(b));
            const lengthType x1 = static_cast<lengthType>(edge(b + 1));
            kw = x1 - x0 > gap ? static_cast<lengthType>(x1 - x0 - gap) : 0;
        }
        /// Call `f(i, x0, kw)` for each shown ivory key wide enough to draw.
        template <typename F>
        void forEachIvory(F&& f) const MM_NONBLOCKING {
            for (uint8_t j = 0; j < shown; j++) {
                const uint8_t i = static_cast<uint8_t>(s0 + j);
                if (detail::pianoIsBlack(i)) continue;
                lengthType x0, kw;
                ivory(i, x0, kw);
                if (kw) f(i, x0, kw);
            }
        }
    };

    /// The shown window: `octaves` from `octave`, or from the detected key's root in the middle octave under `auto`.
    Keys layout(const AudioScene& a) const MM_NONBLOCKING {
        const uint8_t shown = static_cast<uint8_t>(std::min<uint32_t>(kAudioNoteKeys, static_cast<uint32_t>(octaves) * 12u));
        uint8_t s0;
        if (autoKey && a.f->key) {
            const uint8_t rootPc = static_cast<uint8_t>((a.f->key - 1) % 12);
            const uint8_t offset = static_cast<uint8_t>((rootPc + 3) % 12);   // the root's key within an octave, A-based
            s0 = static_cast<uint8_t>(std::min<uint32_t>(36u + offset, kAudioNoteKeys - shown));   // the root in the middle octave, at the window's left
        } else {
            s0 = static_cast<uint8_t>(std::min<uint32_t>(static_cast<uint32_t>(octave) * 12u, kAudioNoteKeys - shown));
        }
        return keysFor(a.w, s0, shown);
    }

    /// `shown` keys from key `s0` across `w` columns.
    static Keys keysFor(lengthType w, uint8_t s0, uint8_t shown) MM_NONBLOCKING {
        const uint8_t base = detail::kPianoWhiteBefore[s0];
        const uint8_t white = static_cast<uint8_t>(detail::kPianoWhiteBefore[s0 + shown] - base);
        const lengthType whiteW = static_cast<lengthType>((w + white - 1) / white);
        return Keys{s0, shown, white, base, w, whiteW, static_cast<lengthType>(whiteW >= 3 ? 1 : 0)};   // a hairline when keys are wide enough
    }

    /// Hand `h`'s reach as a keyboard: its window of a tenth, started on the ivory key at or below it; before the hand played, around C2 for the left and C4 for the right.
    Keys handKeys(const AudioScene& a, uint8_t h) const MM_NONBLOCKING {
        uint8_t s0 = hands_.window[h] >= 0 ? static_cast<uint8_t>(hands_.window[h]) : kHandRest[h];
        if (s0 > 0 && detail::pianoIsBlack(s0)) s0--;
        const uint8_t shown = static_cast<uint8_t>(std::min<uint32_t>(hands::PianoHands::kSpan + 2u, kAudioNoteKeys - static_cast<uint32_t>(s0)));
        return keysFor(a.w, s0, shown);
    }

    /// A hand's half labeled: the ivory letters along its bottom and the standard octave of its first key at its left, middle C being C4.
    void drawHandLabels(const AudioScene& a, const Keys& k, lengthType y0, lengthType kh) MM_NONBLOCKING {
        if (kh <= fonts::kFont4x6.height) return;
        const lengthType ly = static_cast<lengthType>(y0 + kh - fonts::kFont4x6.height);
        k.forEachIvory([&](uint8_t i, lengthType x0, lengthType kw) MM_NONBLOCKING { drawLetter(a.cv, i, x0, kw, ly, RGB{0, 0, 0}); });
        const char num[2] = {static_cast<char>('0' + (21 + k.s0) / 12 - 1), 0};   // MIDI note / 12 - 1
        draw::text(a.cv, fonts::kFont4x6, num, 0, static_cast<lengthType>(y0 + (kh > 6 ? (kh - 6) / 2 : 0)), RGB{255, 255, 255});
    }

    /// One row has no halves, ebony over ivory, letters or circles: each shown semitone takes its own span.
    void drawRow(const AudioScene& a, const Keys& k) MM_NONBLOCKING {
        for (uint8_t j = 0; j < k.shown; j++) {
            const uint8_t i = static_cast<uint8_t>(k.s0 + j);
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(j) * a.w / k.shown);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(j + 1) * a.w / k.shown);
            const uint8_t g = hits_.glow(i, a.ms, a.span);
            const uint8_t lvl = std::max({ivory, level(*a.f, i), g});
            if (x1 > x0 && lvl)
                draw::fillRect(a.cv, x0, 0, static_cast<lengthType>(x1 - x0), 1, keyColor(*a.pal, i, lvl, g, detail::pianoIsBlack(i)));
        }
    }

    /// One keyboard half from row `y0`, `kh` tall: ivory keys across the width at the full height, ebony over the boundaries at two thirds of it.
    void drawKeyboard(const AudioScene& a, const Keys& k, lengthType y0, lengthType kh, bool circles, bool colorKeys) MM_NONBLOCKING {
        k.forEachIvory([&](uint8_t i, lengthType x0, lengthType kw) MM_NONBLOCKING {
            drawKey(a, i, x0, y0, kw, kh, colorKeys);
            if (circles) drawCircles(a, i, y0, kh, x0, kw);
        });
        drawEbony(a, k, y0, kh, circles, colorKeys);
    }

    /// The ebony keys of one half, over the boundaries before them at three fifths of an ivory key's width and two thirds of its length.
    void drawEbony(const AudioScene& a, const Keys& k, lengthType y0, lengthType kh, bool circles, bool colorKeys) MM_NONBLOCKING {
        const lengthType blackW = k.whiteW * 3 / 5 > 0 ? static_cast<lengthType>(k.whiteW * 3 / 5) : 1;
        const lengthType blackH = kh * 2 / 3 > 0 ? static_cast<lengthType>(kh * 2 / 3) : 1;
        for (uint8_t j = 0; j < k.shown; j++) {
            const uint8_t i = static_cast<uint8_t>(k.s0 + j);
            if (!detail::pianoIsBlack(i)) continue;
            const int32_t b = k.edge(static_cast<uint32_t>(detail::kPianoWhiteBefore[i] - k.base));
            const int32_t x0 = std::max<int32_t>(0, b - blackW / 2);
            const int32_t x1 = std::min<int32_t>(a.w, b + (blackW + 1) / 2);
            if (x1 <= x0) continue;
            drawKey(a, i, static_cast<lengthType>(x0), y0, static_cast<lengthType>(x1 - x0), blackH, colorKeys);
            if (circles) drawCircles(a, i, y0, kh, static_cast<lengthType>(x0), static_cast<lengthType>(x1 - x0));
        }
    }

    /// Key `i` filling its rectangle: in the chord guide's color, as played, or plain, as `colorKeys` asks; a plain ebony key at a quarter of `ivory`.
    void drawKey(const AudioScene& a, uint8_t i, lengthType x0, lengthType y0, lengthType w, lengthType h, bool colorKeys) MM_NONBLOCKING {
        const bool ebony = detail::pianoIsBlack(i);
        const uint8_t g = hits_.glow(i, a.ms, a.span);
        const uint8_t lvl = colorKeys ? std::max({ivory, level(*a.f, i), g}) : ivory;
        const bool guide = colorKeys && chordGuide(*a.f, i, g);
        if (!lvl && !guide) return;
        const RGB plain = scale8(RGB{255, 255, 255}, ebony ? static_cast<uint8_t>(ivory / 4u) : ivory);
        draw::fillRect(a.cv, x0, y0, w, h, guide ? guideColor(*a.pal, i) : colorKeys ? keyColor(*a.pal, i, lvl, g, ebony) : plain);
    }

    /// One small circle per octave this pitch sounds in, stacked from the top of the key; always smaller than the key, so it drops on it.
    void drawCircles(const AudioScene& a, uint8_t i, lengthType y0, lengthType kh, lengthType px0, lengthType pxW) MM_NONBLOCKING {
        if (!pxW) return;
        const lengthType slot = kh / detail::kPianoOctaves > 0 ? static_cast<lengthType>(kh / detail::kPianoOctaves) : 1;
        const lengthType r = std::min<lengthType>(pxW / 4, slot / 3);
        const lengthType cx = static_cast<lengthType>(px0 + pxW / 2);
        for (uint8_t o = 0; o < detail::kPianoOctaves; o++) {
            const uint16_t hk = static_cast<uint16_t>((i % 12) + 12u * o);   // this pitch in octave o
            if (hk >= kAudioNoteKeys) break;
            const uint8_t g = hits_.glow(static_cast<uint8_t>(hk), a.ms, a.span);
            const uint8_t lvl = std::max(level(*a.f, static_cast<uint8_t>(hk)), g);
            if (!lvl) continue;
            const lengthType cy = static_cast<lengthType>(y0 + o * slot + slot / 2);
            if (cy + r >= static_cast<lengthType>(y0 + kh)) break;
            // The circle shrinks with its level, so the dim tail of the glow is a small dot, not a big black one.
            const lengthType rr = std::max<lengthType>(1, static_cast<lengthType>(r * lvl / 255));
            draw::fillCircle(a.cv, cx, cy, rr, colorFromPalette(*a.pal, static_cast<uint8_t>(hk * 256u / kAudioNoteKeys), lvl));
        }
    }

    /// The ivory letter strip along the bottom edge and the octave numbers at the left of each circle row and the top half.
    void drawOverlays(const AudioScene& a, const Keys& k, lengthType topH, lengthType botH) MM_NONBLOCKING {
        k.forEachIvory([&](uint8_t i, lengthType x0, lengthType kw) MM_NONBLOCKING {
            drawLetter(a.cv, i, x0, kw, static_cast<lengthType>(a.h - fonts::kFont4x6.height), RGB{0, 0, 0});
        });
        const lengthType slot = botH / detail::kPianoOctaves > 0 ? static_cast<lengthType>(botH / detail::kPianoOctaves) : 1;
        for (uint8_t o = 0; o < detail::kPianoOctaves; o++) {
            char num[2] = {static_cast<char>('0' + o), 0};
            int32_t ly = static_cast<int32_t>(topH + o * slot + slot / 2 - 3);
            if (ly < static_cast<int32_t>(topH)) ly = static_cast<int32_t>(topH);
            if (ly + 6 > static_cast<int32_t>(a.h)) ly = static_cast<int32_t>(a.h) - 6;
            draw::text(a.cv, fonts::kFont4x6, num, 0, static_cast<lengthType>(ly), RGB{255, 255, 255});
        }
        char sel[2] = {static_cast<char>('0' + k.s0 / 12), 0};
        int32_t ty = static_cast<int32_t>(topH / 2 - 3) > 0 ? static_cast<int32_t>(topH / 2 - 3) : 0;
        draw::text(a.cv, fonts::kFont4x6, sel, 0, static_cast<lengthType>(ty), RGB{255, 255, 255});
    }

    /// The key's note letter at `y`, centered in the key, when the key is wide enough for a glyph.
    static void drawLetter(const draw::Canvas& cv, uint8_t i, lengthType px0, lengthType pxW, lengthType y, RGB c) MM_NONBLOCKING {
        if (pxW < fonts::kFont4x6.width) return;
        const char* name = kPitchClassNames[(21 + i) % 12];
        const lengthType tw = static_cast<lengthType>((name[1] ? 2 : 1) * fonts::kFont4x6.width);
        const lengthType tx = static_cast<lengthType>(px0 + (pxW > tw ? (pxW - tw) / 2 : 0));
        draw::text(cv, fonts::kFont4x6, name, tx, y, c);
    }

    /// Whether key `i` shows the chord guide: `chord` is on, a chord is named, the key is one of its tones in any octave, and it is neither sounding nor flashing, a played key staying bright over the guide.
    bool chordGuide(const AudioFrame& f, uint8_t i, uint8_t g) const MM_NONBLOCKING {
        return chord && f.chordQuality != kChordNone && g == 0 && level(f, i) == 0
            && inChord(static_cast<uint8_t>((21 + i) % 12), f.chordRoot, f.chordQuality);
    }

    /// The chord guide's color: the key's palette hue, dim.
    static RGB guideColor(const Palette& pal, uint8_t i) MM_NONBLOCKING {
        return colorFromPalette(pal, static_cast<uint8_t>(i * 256u / kAudioNoteKeys), kGuideLevel);
    }

    /// A key's color: its palette hue while its strike flashes, else ivory or ebony at its level.
    static RGB keyColor(const Palette& pal, uint8_t i, uint8_t lvl, uint8_t g, bool black) MM_NONBLOCKING {
        if (g > 0)
            return colorFromPalette(pal, static_cast<uint8_t>(i * 256u / kAudioNoteKeys), lvl);
        return scale8(RGB{255, 255, 255}, black ? static_cast<uint8_t>(lvl / 4u) : lvl);
    }
};

}  // namespace mm
