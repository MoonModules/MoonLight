#pragma once

#include "light/effects/AudioNotesBase.h"

namespace mm {

namespace detail {
/// The instruments' open strings, MIDI notes from the lowest string up.
constexpr uint8_t kGuitarStrings[] = {40, 45, 50, 55, 59, 64};   // E A D G B E
constexpr uint8_t kBassStrings[] = {28, 33, 38, 43};             // E A D G
constexpr uint8_t kViolinStrings[] = {55, 62, 69, 76};           // G D A E
constexpr uint8_t kCelloStrings[] = {36, 43, 50, 57};            // C G D A
constexpr uint8_t kFrets = 12;   // the fingerboard shows a full octave of frets
constexpr int16_t kMinBoard = kFrets;   // the narrowest fingerboard the strips give way to: a column a fret
constexpr uint8_t kDotFrets[] = {3, 5, 7, 9, 12};   // position dots, 12 doubled
}  // namespace detail

/// A stringed instrument in its rounded wooden enclosure across the lights, each sounding note lighting its string at the fret it is played: a fingerboard with numbered frets, one snare per string at the bridge with its open note named, and the instrument's own emblem beside it.
/// @card StringsEffect.gif
///
/// `instrument` picks guitar, bass, violin or cello, each with its own strings, tuning, snares and body emblem; guitar and bass get metal frets, violin and cello thinner pseudo-frets. The mic's semitone notes light up on every string and fret that plays them, a struck note flashing over `decay`.
///
/// @technical
///
/// ## Fret spacing and hits
///
/// The 12 frets sit equally spaced from nut to bridge and are numbered along the bottom; fret `f` is `f` semitones above the open string, so 12 frets are one octave. Position dots mark frets 3, 5, 7, 9 and 12. The shared `NoteHits` keeps when each note was last struck, so a frame two ticks read is one hit. `noteNames` writes the pitch classes top left and the key top right.
class StringsEffect : public AudioNotesBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// The fingerboard is drawn on a plane; a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// The instrument whose strings and tuning are drawn, in option order.
    enum Instrument : uint8_t { kGuitar, kBass, kViolin, kCello, kInstrumentCount };
    /// Guitar, bass, violin or cello.
    uint8_t instrument = kGuitar;

    /// Publish the instrument, then the glow and the note names.
    void defineControls() override {
        static constexpr const char* kInstrumentOptions[] = {"guitar", "bass", "violin", "cello"};
        controls_.addSelect("instrument", instrument, kInstrumentOptions, kInstrumentCount);
        addNoteControls();
    }

    /// Draw the instrument, then light each sounding note on every string and fret that plays it: @xref{fret-spacing-and-hits}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginNotes(kBoardC);
        if (a.w == 0 || a.h == 0) return;
        const Board b = layout(a.w, a.h);
        drawEnclosure(a, b);
        if (b.fits()) {   // no room for a fingerboard leaves the enclosure and the names alone
            drawFingerboard(a, b);
            drawSnares(a, b);
            drawEmblem(a.cv, static_cast<lengthType>((b.frame + b.nutX) / 2 - 1), b.bodyW, b.frame, static_cast<lengthType>(a.h - 1 - b.frame));   // in the left margin, clear of the note names and the nut
            drawNotes(a, b);
        }
        endNotes(a);
    }

private:
    static constexpr RGB kBoardC{105, 62, 24};    ///< the fingerboard's wood, the background
    static constexpr RGB kFretC{200, 200, 210};   ///< metal: frets, the bridge and the snares
    static constexpr lengthType kLabelW = 3 * fonts::kFont4x6.width + 2;   ///< room for an open-string name like "C#2"

    /// Where the instrument's parts fall on a grid, worked out once a frame.
    struct Board {
        lengthType frame, bodyW;     ///< the wood frame's width, each side strip's width
        lengthType nutX, bridgeX;    ///< the fingerboard's left and right columns
        lengthType fbTop, fbBot;     ///< the fingerboard's top and bottom rows
        bool numRoom;                ///< six rows under the board for fret numbers
        /// Whether a fingerboard fits between the strips and the frame.
        bool fits() const MM_NONBLOCKING { return bridgeX > nutX && fbBot > fbTop; }
    };

    /// The geometry on a `w` by `h` grid: the strips give way to the fingerboard on a narrow grid, so a 32-wide panel keeps a fret a column, and the left margin equals the body strip, centering the board.
    static Board layout(lengthType w, lengthType h) MM_NONBLOCKING {
        const lengthType frame = w / 32 > 2 ? static_cast<lengthType>(w / 32) : 2;
        const lengthType wideW = kLabelW > w / 6 ? kLabelW : static_cast<lengthType>(w / 6);
        const lengthType fitW = static_cast<lengthType>((w - 1 - 2 * frame - detail::kMinBoard) / 2);
        const lengthType bodyW = std::max<lengthType>(0, std::min(wideW, fitW));
        const bool numRoom = h - 1 - frame - 7 > frame + 2;
        return Board{frame, bodyW, static_cast<lengthType>(frame + bodyW), static_cast<lengthType>(w - 1 - frame - bodyW),
                     static_cast<lengthType>(frame + 2), static_cast<lengthType>(h - 1 - frame - (numRoom ? 7 : 2)), numRoom};
    }

    /// The rounded wooden enclosure over the board-colored background: the wood frame with rounded corners and the body strip beside the bridge.
    static void drawEnclosure(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        const RGB wood{200, 120, 45}, body{135, 82, 30};
        draw::rect(a.cv, 0, 0, a.w, a.h, wood);
        draw::fillRect(a.cv, static_cast<lengthType>(b.bridgeX + 1), b.frame, static_cast<lengthType>(a.w - 1 - b.frame - b.bridgeX), static_cast<lengthType>(a.h - 1 - b.frame), body);
        roundCorners(a.cv, a.w, a.h, b.frame, b.nutX, wood);
    }

    /// The frets equally spaced, metal and 2px wide when fretted, thin pseudo-frets otherwise, the strings over them as on a real instrument, the nut, the bridge, the fret numbers and the dots.
    void drawFingerboard(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const bool fretted = instrument == kGuitar || instrument == kBass;
        const RGB stringC{230, 190, 90}, pseudoC{120, 80, 40}, nutC{235, 235, 240};
        const uint8_t n = stringCount();
        const lengthType strW = (instrument == kBass || instrument == kCello) ? 2 : 1;   // bass and cello strings are thicker than violin's
        for (uint8_t fr = 1; fr <= detail::kFrets; fr++)
            draw::fillRect(a.cv, fretX(fr, b), static_cast<lengthType>(b.fbTop - 1), fretted ? 2 : 1, static_cast<lengthType>(b.fbBot - b.fbTop + 3), fretted ? kFretC : pseudoC);
        for (uint8_t s = 0; s < n; s++) {
            const lengthType y = stringY(s, b, n);
            draw::fillRect(a.cv, b.nutX, static_cast<lengthType>(y > strW / 2 ? y - strW / 2 : 0), static_cast<lengthType>(b.bridgeX - b.nutX + 1), strW, stringC);
        }
        draw::fillRect(a.cv, b.nutX, b.frame, 2, static_cast<lengthType>(a.h - 1 - 2 * b.frame), nutC);
        draw::bar(a.cv, b.bridgeX, b.frame, static_cast<lengthType>(a.h - 1 - 2 * b.frame), draw::Grow::Down, kFretC);
        if (b.numRoom) drawFretNumbers(a, b);
        drawDots(a, b);
    }

    /// Position dots at frets 3, 5, 7, 9 and a double at 12, like a real fingerboard.
    static void drawDots(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        const RGB dotC{225, 220, 200};
        const lengthType midY = static_cast<lengthType>((b.fbTop + b.fbBot) / 2);
        for (const uint8_t fr : detail::kDotFrets) {
            draw::fillCircle(a.cv, fretX(fr, b), midY, 1, dotC);
            if (fr == 12) draw::fillCircle(a.cv, fretX(fr, b), static_cast<lengthType>(midY + 3), 1, dotC);
        }
    }

    /// The fret numbers along the bottom of the board, each kept inside the frame.
    static void drawFretNumbers(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        const RGB numC{225, 220, 200};
        for (uint8_t fr = 1; fr <= detail::kFrets; fr++) {
            char num[3] = {0, 0, 0};
            uint8_t len = 0;
            if (fr >= 10) num[len++] = static_cast<char>('0' + fr / 10);
            num[len++] = static_cast<char>('0' + fr % 10);
            const lengthType tw = static_cast<lengthType>(len * fonts::kFont4x6.width);
            int32_t tx = static_cast<int32_t>(fretX(fr, b)) - static_cast<int32_t>(tw) / 2;
            if (tx < b.frame) tx = b.frame;
            if (tx + tw > a.w - b.frame) tx = a.w - b.frame - tw;   // the right edge wins on a grid too narrow for both
            draw::text(a.cv, fonts::kFont4x6, num, static_cast<lengthType>(tx), static_cast<lengthType>(b.fbBot + 2), numC);
        }
    }

    /// One snare per string at the bridge, a thin tick the color of the frets, with the open string's note named beside it where the body strip holds it.
    void drawSnares(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const uint8_t n = stringCount();
        for (uint8_t s = 0; s < n; s++) {
            const lengthType y = stringY(s, b, n);
            draw::bar(a.cv, b.bridgeX, static_cast<lengthType>(y > 2 ? y - 2 : 0), 5, draw::Grow::Down, kFretC);   // over the frets
            if (b.bodyW < kLabelW) continue;
            char name[4];
            openName(openString(s), name);
            lengthType nl = 0;
            while (name[nl]) nl++;
            const lengthType tw = static_cast<lengthType>(nl * fonts::kFont4x6.width);
            const int32_t tx = std::max<int32_t>(b.bridgeX + 1, b.bridgeX + 1 + (b.bodyW - 1 - tw) / 2);   // centered in the body strip
            draw::text(a.cv, fonts::kFont4x6, name, static_cast<lengthType>(tx), static_cast<lengthType>(y > 3 ? y - 3 : 0), RGB{255, 255, 255});
        }
    }

    /// Each sounding note lights every string and fret that plays it; a dot's radius is capped by the fret spacing, so adjacent frets' dots do not overlap, and shrinks with its level, so the dim tail of the glow is a small dot.
    void drawNotes(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        const uint8_t n = stringCount();
        const lengthType bandH = static_cast<lengthType>(b.fbBot - b.fbTop);
        const lengthType fretGap = static_cast<lengthType>((b.bridgeX - b.nutX) / detail::kFrets);   // one fret's columns
        const lengthType r = std::max<lengthType>(1, std::min<lengthType>({static_cast<lengthType>(bandH / n / 3), lengthType{4}, static_cast<lengthType>(fretGap > 2 ? (fretGap - 2) / 2 : 1)}));
        for (uint8_t i = 0; i < kAudioNoteKeys; i++) {
            const uint8_t g = hits_.glow(i, a.ms, a.span);
            const uint8_t lvl = std::max(static_cast<uint8_t>(i < a.f->notesCount ? a.f->notes[i] : 0), g);
            if (!lvl) continue;
            const lengthType rr = std::max<lengthType>(1, static_cast<lengthType>(r * lvl / 255));
            const RGB c = colorFromPalette(*a.pal, static_cast<uint8_t>(i * 256u / kAudioNoteKeys), lvl);
            for (uint8_t s = 0; s < n; s++) {
                const int16_t fret = static_cast<int16_t>(21 + i) - openString(s);
                if (fret < 0 || fret > detail::kFrets) continue;
                draw::fillCircle(a.cv, fret == 0 ? b.nutX : fretX(static_cast<uint8_t>(fret), b), stringY(s, b, n), rr, c);
            }
        }
    }

    /// How many strings the selected instrument has.
    uint8_t stringCount() const MM_NONBLOCKING {
        return instrument == kGuitar ? 6 : 4;
    }

    /// The open MIDI note of string `s`, lowest string first.
    uint8_t openString(uint8_t s) const MM_NONBLOCKING {
        switch (instrument) {
            case kBass: return detail::kBassStrings[s];
            case kViolin: return detail::kViolinStrings[s];
            case kCello: return detail::kCelloStrings[s];
            default: return detail::kGuitarStrings[s];
        }
    }

    /// String `s`'s center row, the `n` strings spread evenly over the fingerboard.
    static lengthType stringY(uint8_t s, const Board& b, uint8_t n) MM_NONBLOCKING {
        return static_cast<lengthType>(b.fbTop + (2 * s + 1) * (b.fbBot - b.fbTop) / (2 * n));
    }

    /// The fret's column: equally spaced from the nut to the bridge.
    static lengthType fretX(uint8_t fret, const Board& b) MM_NONBLOCKING {
        return static_cast<lengthType>(b.nutX + fret * (b.bridgeX - b.nutX) / detail::kFrets);
    }

    /// One corner of the frame: its anchor pixel, the directions the arc runs in, and the arc's radius.
    struct Corner {
        int32_t x0, y0, sx, sy, cr;
    };

    /// Round the frame's corners: outside the quarter arc goes dark, the arc itself stays wood.
    static void roundCorners(const draw::Canvas& cv, lengthType w, lengthType h, lengthType frame, lengthType nutX, RGB wood) MM_NONBLOCKING {
        int32_t cr = 9 * static_cast<int32_t>(frame) / 2;   // rounder, but never reaching the strings
        const int32_t maxCr = static_cast<int32_t>(nutX);   // the arc may reach the nut but never the strings
        if (cr > maxCr) cr = maxCr;
        if (cr < 1) cr = 1;
        for (uint8_t c = 0; c < 4; c++)
            cornerArc(cv, {static_cast<int32_t>((c & 1u) != 0 ? w - 1 : 0), static_cast<int32_t>((c & 2u) != 0 ? h - 1 : 0),
                           static_cast<int32_t>((c & 1u) != 0 ? -1 : 1), static_cast<int32_t>((c & 2u) != 0 ? -1 : 1), cr},
                      w, h, wood);
    }

    /// One corner's quarter arc: pixels outside the arc go dark, the arc's rim stays wood.
    static void cornerArc(const draw::Canvas& cv, Corner c, lengthType w, lengthType h, RGB wood) MM_NONBLOCKING {
        for (int32_t dx = 0; dx <= c.cr; dx++)
            for (int32_t dy = 0; dy <= c.cr; dy++) {
                const int32_t d2 = (c.cr - dx) * (c.cr - dx) + (c.cr - dy) * (c.cr - dy);
                const int32_t x = c.x0 + c.sx * dx, y = c.y0 + c.sy * dy;
                if (x < 0 || y < 0 || x >= static_cast<int32_t>(w) || y >= static_cast<int32_t>(h)) continue;
                if (d2 > c.cr * c.cr) draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, RGB{0, 0, 0});
                else if (d2 >= (c.cr - 1) * (c.cr - 1)) draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, wood);
            }
    }

    /// The instrument's abstract emblem centered in its strip at `cx`, as large as the strip fits: a sound hole for guitar, a pickup for bass, f-holes for violin, f-holes and an endpin for cello.
    void drawEmblem(const draw::Canvas& cv, lengthType cx, lengthType stripW, lengthType top, lengthType bottom) const MM_NONBLOCKING {
        const lengthType by = static_cast<lengthType>((top + bottom) / 2);
        const lengthType s = (std::min(stripW, static_cast<lengthType>(bottom - top)) - 1) / 2;   // diameter fits the strip
        if (s < 2) return;
        const RGB c{215, 150, 80};
        const lengthType bx = cx;
        switch (instrument) {
            case kGuitar:
                draw::circle(cv, bx, by, s > 3 ? s - 2 : s, c);   // the sound hole, 2 px clear of the nut
                break;
            case kBass:
                draw::fillRect(cv, static_cast<lengthType>(bx - s / 2), static_cast<lengthType>(by - s / 3),
                               s, static_cast<lengthType>(s * 2 / 3 > 0 ? s * 2 / 3 : 1), c);   // the pickup
                break;
            case kViolin:
            case kCello:
                draw::line(cv, {static_cast<lengthType>(bx - s / 2), static_cast<lengthType>(by - s / 2)},
                           {static_cast<lengthType>(bx - s / 4), static_cast<lengthType>(by + s / 2)}, c);   // f-holes
                draw::line(cv, {static_cast<lengthType>(bx + s / 4), static_cast<lengthType>(by - s / 2)},
                           {static_cast<lengthType>(bx + s / 2), static_cast<lengthType>(by + s / 2)}, c);
                if (instrument == kCello)
                    draw::fillCircle(cv, bx, static_cast<lengthType>(by + s + 2), 1, c);   // the endpin
                break;
            default: break;
        }
    }

    /// The open note's name with its octave, "E2" or "C#3", into `out` (4 chars).
    static void openName(uint8_t midi, char* out) MM_NONBLOCKING {
        const char* n = kPitchClassNames[midi % 12];
        uint8_t len = 0;
        out[len++] = n[0];
        if (n[1]) out[len++] = n[1];
        out[len++] = static_cast<char>('0' + midi / 12 - 1);
        out[len] = 0;
    }
};

}  // namespace mm
