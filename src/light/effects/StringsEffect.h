#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

namespace detail {
/// The instruments' open strings, MIDI notes from the lowest string up.
constexpr uint8_t kGuitarStrings[] = {40, 45, 50, 55, 59, 64};   // E A D G B E
constexpr uint8_t kBassStrings[] = {28, 33, 38, 43};             // E A D G
constexpr uint8_t kViolinStrings[] = {55, 62, 69, 76};           // G D A E
constexpr uint8_t kCelloStrings[] = {36, 43, 50, 57};            // C G D A
constexpr uint8_t kFrets = 12;   // the fingerboard shows a full octave of frets
constexpr uint8_t kDotFrets[] = {3, 5, 7, 9, 12};   // position dots, 12 doubled
}  // namespace detail

/// A stringed instrument in its rounded wooden enclosure across the lights, each sounding note lighting its string at the fret it is played: a fingerboard with numbered frets, one snare per string at the bridge with its open note named, and the instrument's own emblem on the body.
/// @card StringsEffect.gif
///
/// `instrument` picks guitar, bass, violin or cello, each with its own strings, tuning, snares and body emblem; guitar and bass get metal frets, violin and cello thinner pseudo-frets. The mic's semitone notes light up on every string and fret that plays them, a struck note flashing over `decay`.
///
/// @technical
///
/// ## Fret spacing and hits
///
/// The 12 frets sit equally spaced from nut to bridge and are numbered along the bottom; fret `f` is `f` semitones above the open string, so 12 frets are one octave. Position dots mark frets 3, 5, 7, 9 and 12. The shared `NoteHits` keeps when each note was last struck, so a frame two ticks read is one hit. `noteNames` writes the pitch classes top left and the key top right.
class StringsEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// The fingerboard is drawn on a plane; a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// The instrument whose strings and tuning are drawn, in option order.
    enum Instrument : uint8_t { kGuitar, kBass, kViolin, kCello, kInstrumentCount };
    /// Guitar, bass, violin or cello.
    uint8_t instrument = kGuitar;
    /// How long a struck note glows, in tens of milliseconds.
    uint8_t decay = 30;
    /// The strongest pitch classes top left, the key top right.
    bool noteNames = true;

    /// Publish the instrument, the note names and the glow.
    void defineControls() override {
        static constexpr const char* kInstrumentOptions[] = {"guitar", "bass", "violin", "cello"};
        controls_.addSelect("instrument", instrument, kInstrumentOptions, kInstrumentCount);
        controls_.addControl("decay", decay, 5, 100);
        controls_.addControl("noteNames", noteNames);
    }

    /// Draw the instrument, then light each sounding note on every string and fret that plays it: @xref{fret-spacing-and-hits}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginAudioTick(*this, decay, hits_);
        if (a.w == 0 || a.h == 0) return;
        const draw::Canvas& cv = a.cv;
        const lengthType w = a.w, h = a.h;
        const AudioFrame& f = *a.f;
        const uint32_t ms = a.ms, span = a.span;
        const Palette& pal = *a.pal;

        const uint8_t n = stringCount();
        // The rounded wooden enclosure: board fill, wood frame with rounded corners, body strip beside the bridge.
        const lengthType frame = w / 32 > 2 ? static_cast<lengthType>(w / 32) : 2;
        const lengthType labelW = 3 * fonts::kFont4x6.width + 2;   // room for a name like "C#2"
        const lengthType bodyW = labelW > w / 6 ? labelW : static_cast<lengthType>(w / 6);
        const lengthType nutX = frame + (w / 12 > 2 ? static_cast<lengthType>(w / 12) : 2);
        const lengthType bridgeX = static_cast<lengthType>(w - 1 - frame - bodyW);
        const bool fretted = instrument == kGuitar || instrument == kBass;
        const bool numRoom = h - 1 - frame - 7 > frame + 2;   // six rows under the board for fret numbers
        const lengthType fbTop = static_cast<lengthType>(frame + 2);
        const lengthType fbBot = static_cast<lengthType>(h - 1 - frame - (numRoom ? 7 : 2));
        const RGB wood{200, 120, 45}, board{105, 62, 24}, body{135, 82, 30};
        draw::fill(cv, board);
        draw::rect(cv, 0, 0, w, h, wood);
        draw::fillRect(cv, static_cast<lengthType>(bridgeX + 1), frame, static_cast<lengthType>(w - 1 - frame - bridgeX), static_cast<lengthType>(h - 1 - frame), body);
        roundCorners(cv, w, h, frame, nutX, wood);

        // The frets equally spaced, metal and 2px wide when fretted, thin pseudo-frets otherwise; the strings run over them, as on a real instrument.
        const RGB stringC{230, 190, 90}, fretC{200, 200, 210}, pseudoC{120, 80, 40}, nutC{235, 235, 240};
        const lengthType strW = (instrument == kBass || instrument == kCello) ? 2 : 1;   // bass and cello strings are thicker than violin's
        const lengthType fretW = fretted ? 2 : 1;
        const RGB fretCol = fretted ? fretC : pseudoC;
        for (uint8_t fr = 1; fr <= detail::kFrets; fr++)
            draw::fillRect(cv, fretX(fr, nutX, bridgeX), static_cast<lengthType>(fbTop - 1), fretW, static_cast<lengthType>(fbBot - fbTop + 3), fretCol);
        for (uint8_t s = 0; s < n; s++) {
            const lengthType y = stringY(s, fbTop, fbBot, n);
            draw::fillRect(cv, nutX, static_cast<lengthType>(y > strW / 2 ? y - strW / 2 : 0),
                           static_cast<lengthType>(bridgeX - nutX + 1), strW, stringC);
        }
        draw::fillRect(cv, nutX, frame, 2, static_cast<lengthType>(h - 1 - 2 * frame), nutC);
        draw::bar(cv, bridgeX, frame, static_cast<lengthType>(h - 1 - 2 * frame), draw::Grow::Down, fretC);
        // Fret numbers along the bottom of the board.
        if (numRoom) {
            const RGB numC{225, 220, 200};
            for (uint8_t fr = 1; fr <= detail::kFrets; fr++) {
                char num[3] = {0, 0, 0};
                uint8_t len = 0;
                if (fr >= 10) num[len++] = static_cast<char>('0' + fr / 10);
                num[len++] = static_cast<char>('0' + fr % 10);
                num[len] = 0;
                const lengthType tw = static_cast<lengthType>(len * fonts::kFont4x6.width);
                int32_t tx = static_cast<int32_t>(fretX(fr, nutX, bridgeX)) - static_cast<int32_t>(tw) / 2;
                if (tx < static_cast<int32_t>(frame)) tx = frame;
                if (tx + static_cast<int32_t>(tw) > static_cast<int32_t>(w - frame)) tx = static_cast<int32_t>(w - frame - tw);
                draw::text(cv, fonts::kFont4x6, num, static_cast<lengthType>(tx), static_cast<lengthType>(fbBot + 2), numC);
            }
        }
        // Position dots at 3, 5, 7, 9 and a double at 12, like a real fingerboard.
        const lengthType midY = static_cast<lengthType>((fbTop + fbBot) / 2);
        const RGB dotC{225, 220, 200};
        for (uint8_t d = 0; d < 5; d++) {
            const uint8_t fr = detail::kDotFrets[d];
            draw::fillCircle(cv, fretX(fr, nutX, bridgeX), midY, 1, dotC);
            if (fr == 12) draw::fillCircle(cv, fretX(fr, nutX, bridgeX), static_cast<lengthType>(midY + 3), 1, dotC);
        }
        // One snare per string at the bridge, a thin tick the color of the frets, with the open string's note named beside it.
        const bool labels = w > 2 * (nutX + labelW) + 4;
        for (uint8_t s2 = 0; s2 < n; s2++) {
            const lengthType y = stringY(s2, fbTop, fbBot, n);
            draw::bar(cv, bridgeX, static_cast<lengthType>(y > 2 ? y - 2 : 0), 5, draw::Grow::Down, fretC);   // over the frets
            if (labels) {
                char name[4];
                openName(openString(s2), name);
                lengthType nl = 0;
                while (name[nl]) nl++;
                const lengthType tw = static_cast<lengthType>(nl * fonts::kFont4x6.width);
                int32_t tx = static_cast<int32_t>(bridgeX) + (static_cast<int32_t>(bodyW) - static_cast<int32_t>(tw)) / 2;   // middle of the body strip
                if (tx < static_cast<int32_t>(bridgeX + 1)) tx = static_cast<int32_t>(bridgeX + 1);
                draw::text(cv, fonts::kFont4x6, name, static_cast<lengthType>(tx),
                           static_cast<lengthType>(y > 3 ? y - 3 : 0), RGB{255, 255, 255});
            }
        }
        drawEmblem(cv, bridgeX, bodyW, fbTop, fbBot);

        // Each sounding note lights every string and fret that plays it.
        const lengthType bandH = fbBot - fbTop > 0 ? static_cast<lengthType>(fbBot - fbTop) : 1;
        const lengthType r = std::max<lengthType>(1, std::min<lengthType>(bandH / n / 3, 4));
        for (uint8_t i = 0; i < kAudioNoteKeys; i++) {
            const uint8_t g = hits_.glow(i, ms, span);
            const uint8_t lvl = std::max(static_cast<uint8_t>(i < f.notesCount ? f.notes[i] : 0), g);
            if (!lvl) continue;
            const uint8_t midi = static_cast<uint8_t>(21 + i);
            for (uint8_t s = 0; s < n; s++) {
                const int16_t fret = static_cast<int16_t>(midi) - openString(s);
                if (fret < 0 || fret > detail::kFrets) continue;
                const lengthType cx = fret == 0 ? nutX : fretX(static_cast<uint8_t>(fret), nutX, bridgeX);
                const lengthType cy = stringY(s, fbTop, fbBot, n);
                draw::fillCircle(cv, cx, cy, r, colorFromPalette(pal, static_cast<uint8_t>(i * 256u / kAudioNoteKeys), lvl));
            }
        }
        if (noteNames && h > 1) draw::noteNames(cv, f, w, 0);
    }

private:
    NoteHits hits_;   ///< when each note last struck and its flash

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

    /// The string's center row between `fbTop` and `fbBot`.
    static lengthType stringY(uint8_t s, lengthType fbTop, lengthType fbBot, uint8_t n) MM_NONBLOCKING {
        return static_cast<lengthType>(fbTop + (2u * s + 1u) * (fbBot - fbTop) / (2u * n));
    }

    /// The fret's column: equally spaced from the nut to the bridge.
    static lengthType fretX(uint8_t fret, lengthType nutX, lengthType bridgeX) MM_NONBLOCKING {
        return static_cast<lengthType>(nutX + static_cast<uint32_t>(fret) * (bridgeX - nutX) / detail::kFrets);
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

    /// The instrument's abstract emblem on the body: a sound hole for guitar, a pickup for bass, f-holes for violin, f-holes and an endpin for cello.
    void drawEmblem(const draw::Canvas& cv, lengthType bridgeX, lengthType bodyW, lengthType fbTop, lengthType fbBot) const MM_NONBLOCKING {
        const lengthType bx = static_cast<lengthType>(bridgeX + bodyW / 2);
        const lengthType by = static_cast<lengthType>((fbTop + fbBot) / 2);
        const lengthType s = std::min(bodyW, static_cast<lengthType>(fbBot - fbTop)) / 3;
        if (s < 2) return;
        const RGB c{215, 150, 80};
        switch (instrument) {
            case kGuitar:
                draw::circle(cv, bx, by, s, c);   // the sound hole
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
