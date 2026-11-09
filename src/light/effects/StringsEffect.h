#pragma once

#include "light/effects/AudioNotesBase.h"
#include "light/powerfunctions/playerHands.h"   // FrettingHand, StringPlay: where and how a string is played

namespace mm {

namespace detail {
/// The instruments' open strings, MIDI notes from the lowest string up.
constexpr uint8_t kGuitarStrings[] = {40, 45, 50, 55, 59, 64};   // E A D G B E
constexpr uint8_t kBassStrings[] = {28, 33, 38, 43};             // E A D G
constexpr uint8_t kViolinStrings[] = {55, 62, 69, 76};           // G D A E
constexpr uint8_t kCelloStrings[] = {36, 43, 50, 57};            // C G D A
constexpr uint8_t kMaxFrets = 24;   // the longest fingerboard drawn, two octaves
constexpr int16_t kMinBoard = 12;   // the narrowest fingerboard the strips give way to
constexpr uint8_t kDotFrets[] = {3, 5, 7, 9, 12, 15, 17, 19, 21, 24};   // inlay dots, 12 and 24 doubled
/// 2^(−f/12) in 65536ths for f up to kMaxFrets: where fret `f` stops the string, as a share of its length from the bridge, the equal-tempered rule that puts fret 12 halfway.
constexpr uint32_t kStopShare[kMaxFrets + 1] = {65536, 61858, 58386, 55109, 52016, 49097, 46341, 43740, 41285, 38968, 36781, 34716, 32768,
                                                30929, 29193, 27554, 26008, 24548, 23170, 21870, 20643, 19484, 18390, 17358, 16384};
}  // namespace detail

/// A stringed instrument across the lights, laid out as the real one: the body at the left with its bridge, where the strings are plucked, strummed or bowed, the fingerboard with its frets closing in toward the body, and the head at the right naming each open string. Each sounding note lights its string where a finger stops it.
/// @card StringsEffect.gif
///
/// `instrument` picks guitar, bass, violin or cello, each with its own strings, tuning, bridge and body: a sound hole, a pickup, or f-holes with a tailpiece. Guitar and bass get metal frets, violin and cello thinner pseudo-frets. The mic's semitone notes light up where they are played, a struck note flashing over `decay`.
///
/// Under `notes` `hands` a note lights at one place, one note a string, within the fretting hand's reach. The sound carries no finger and no pick, no palm muting or other technique, and the bow's direction only as a guess from where notes start, so the effect shows none of them; a strum landing within one block has no direction.
///
/// A plucked or bowed string lights cyan from the bridge to where it is stopped, the part that sounds; a strum flashes across the strings where they are struck; on violin and cello the bow crosses the strings by the bridge, moving while it bows. Under the other levels a note lights every string and fret that can play it.
///
/// @technical
///
/// ## Fret spacing and hits
///
/// The frets close in toward the body as on a real fingerboard: fret `f` sits 1 − 2^(−f/12) of the string's length from the nut, the board running from the nut at the right to the instrument's last fret at the body: 19 on the guitar, 20 on the bass, two octaves on violin and cello.
///
/// Fret `f` is `f` semitones above the open string; a note lights in the space behind its fret, where a finger stops it, an open string at the nut.
///
/// Inlay dots mark spaces 3, 5, 7, 9 and 12, the twelfth doubled, and the space numbers run along the bottom, one skipped where it would touch the last. The shared `NoteHits` keeps when each note was last struck, so a frame two ticks read is one hit.
///
/// ## How a string is played
///
/// Under `hands` the fretting hand places each note at one string and fret, within four frets of its position or open, one note a string.
///
/// Three or more strings starting within 80 ms are a strum, its direction the order of the starts, unknown when they fall in one block; on violin and cello a sound still holding a quarter of its peak after 300 ms is bowed, a faded one plucked. A sounding string lights from the bridge to its fret, as only that length vibrates.
///
/// ## How the bow strokes
///
/// The bow moves along its length while a string is bowed, as a player draws it across the strings: from a fifth of a bow length a second on the softest note to a whole length on the loudest.
///
/// It turns at each new note, as a player changes bow, and before the strings reach within 15% of either end, so most of the bow is used, or sooner where an outer string would leave the hair, so every string it lights stays under it; between notes it waits where it is. The frog is at the bottom, so an up-bow lifts it toward the strings.
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
        addNotesSelect();
        addNoteControls();
    }

    /// The fret string `s` plays at, -1 for none, for tests.
    int fretForTest(uint8_t s) const { return s < hands::kMaxStrings && hand_.note[s] >= 0 ? hand_.fret[s] : -1; }
    /// The key string `s` plays, -1 for none, for tests and the note harness.
    int keyForTest(uint8_t s) const { return s < hands::kMaxStrings ? hand_.note[s] : -1; }
    /// Where the strings meet the bow, 0 at the frog to 1 at the tip, for tests.
    float bowForTest() const { return bowAt_; }

    /// Draw the instrument, then light each sounding note where it is played: @xref{fret-spacing-and-hits}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginNotes(kBoardC);
        if (a.w == 0 || a.h == 0) return;
        const Board b = layout(a.w, a.h);
        drawEnclosure(a, b);
        if (b.fits()) {   // no room for a fingerboard leaves the enclosure and the names alone
            drawBody(a, b);
            drawFingerboard(a, b);
            drawNames(a, b);
            drawNotes(a, b);
        }
        endNotes(a);
    }

protected:
    /// The fretting hand places the notes, one a string, and how each string was played is read from its notes' starts and levels: @xref{how-a-string-is-played}.
    void playedBy(const AudioFrame& f, const uint8_t* hits, NoteSet& set, uint32_t ms) MM_NONBLOCKING override {
        chordLvl_ = 0;   // the chord's shape lights only while snapToChord holds it
        if (instrument != handInstrument_) { hand_ = hands::FrettingHand{}; play_ = hands::StringPlay{}; handInstrument_ = instrument; bowAt_ = 0.5f; bowDir_ = 1; }
        uint8_t open[hands::kMaxStrings] = {};
        const uint8_t n = stringCount();
        for (uint8_t s = 0; s < n; s++) open[s] = openString(s);
        hand_.update(f.notes, set, open, n, frets(), ms, hits);
        for (uint8_t s = 0; s < n; s++) {
            if (hand_.note[s] < 0 || !set.has(static_cast<uint8_t>(hand_.note[s]))) continue;
            const uint8_t k = static_cast<uint8_t>(hand_.note[s]);
            if (hitAt(hits, k)) { play_.start(s, ms, f.notes[k]); newStroke_ = true; }
            else play_.sound(s, f.notes[k]);
        }
    }

    /// Under `hands` the fretting hand plays the chord's shape, lit at the music's level, instead of the notes one by one, once three strings or more sound: a line of single notes, a solo, keeps its notes, as a Keyboard hand of fewer than three keys does.
    void snapToChord(const AudioFrame& f, NoteSet& set) MM_NONBLOCKING override {
        if (notes != kNotesHands) { AudioNotesBase::snapToChord(f, set); return; }
        uint8_t sounding = 0;
        for (uint8_t i = 0; i < kAudioNoteKeys; i++) sounding = static_cast<uint8_t>(sounding + set.has(i));
        if (sounding < 3) return;
        uint8_t open[hands::kMaxStrings] = {};
        const uint8_t n = stringCount();
        for (uint8_t s = 0; s < n; s++) open[s] = openString(s);
        if (!hand_.shape(f.chordRoot, f.chordQuality, open, n, frets())) return;
        set = NoteSet{};
        for (uint8_t s = 0; s < n; s++) if (hand_.note[s] >= 0) set.add(static_cast<uint8_t>(hand_.note[s]));
        chordLvl_ = static_cast<uint8_t>(f.levelSmoothed > 255 ? 255 : f.levelSmoothed);
    }

private:
    static constexpr uint32_t kStrumFlashMs = 250;   ///< how long a strummed string's flash where it is struck lasts
    static constexpr float kBowSlowest = 0.2f;   ///< the bow's speed on the softest bowed note, in bow lengths a second
    static constexpr float kBowFastest = 1.0f;   ///< and on the loudest
    static constexpr float kBowEnd = 0.85f;      ///< how far toward either end the strings meet the bow before it turns
    float bowAt_ = 0.5f;      ///< where the strings meet the bow, 0 at the frog to 1 at the tip
    int8_t bowDir_ = 1;       ///< +1 a down-bow, the frog moving away from the strings, -1 an up-bow
    bool newStroke_ = false;  ///< a note started since the last frame, so the bow turns
    uint32_t bowMs_ = 0;      ///< the frame the bow last moved on
    uint8_t chordLvl_ = 0;              ///< the level the chord's shape lights at, 0 while no shape is held
    hands::FrettingHand hand_;          ///< where the fretting hand is and what each string plays
    hands::StringPlay play_;            ///< how each string was played
    uint8_t handInstrument_ = 0xFF;     ///< the instrument the hand was placed for, so a change starts it afresh
    static constexpr RGB kBoardC{105, 62, 24};     ///< the fingerboard's wood, the background
    static constexpr RGB kFretC{200, 200, 210};    ///< metal: the frets and the cello's endpin
    static constexpr RGB kStringC{230, 190, 90};   ///< a resting string, gold
    static constexpr RGB kVibrateC{0, 220, 255};   ///< a sounding string, cyan, apart from the gold strings, the wood and the metal
    static constexpr lengthType kLabelW = 3 * fonts::kFont4x6.width + 2;   ///< room for an open-string name like "C#2"

    /// Where the instrument's parts fall on a grid, worked out once a frame: the body at the left, the head at the right.
    struct Board {
        lengthType frame, bodyW;     ///< the wood frame's width, each side strip's width
        lengthType bridgeX, pickX;   ///< the bridge's column, and where the strings are struck or bowed
        lengthType endX, nutX;       ///< the fingerboard's body end at fret 12, and the nut at the head
        lengthType fbTop, fbBot;     ///< the fingerboard's top and bottom rows
        bool numRoom;                ///< six rows under the board for the space numbers
        /// Whether a fingerboard fits between the strips and the frame.
        bool fits() const MM_NONBLOCKING { return nutX > endX && fbBot > fbTop && bridgeX < endX; }
    };

    /// The geometry on a `w` by `h` grid: the strips give way to the fingerboard on a narrow grid, so a 32-wide panel keeps a column a fret, and the body and the head are as wide, centering the board.
    static Board layout(lengthType w, lengthType h) MM_NONBLOCKING {
        const lengthType frame = w / 32 > 2 ? static_cast<lengthType>(w / 32) : 2;
        const lengthType wideW = kLabelW > w / 6 ? kLabelW : static_cast<lengthType>(w / 6);
        const lengthType fitW = static_cast<lengthType>((w - 1 - 2 * frame - detail::kMinBoard) / 2);
        const lengthType bodyW = std::max<lengthType>(0, std::min(wideW, fitW));
        const bool numRoom = h - 1 - frame - 7 > frame + 2;
        const lengthType endX = static_cast<lengthType>(frame + bodyW);
        const lengthType bridgeX = static_cast<lengthType>(frame + std::max<lengthType>(1, static_cast<lengthType>(bodyW / 3)));
        return Board{frame, bodyW, bridgeX, static_cast<lengthType>((bridgeX + endX) / 2), endX, static_cast<lengthType>(w - 1 - frame - bodyW),
                     static_cast<lengthType>(frame + 2), static_cast<lengthType>(h - 1 - frame - (numRoom ? 7 : 2)), numRoom};
    }

    /// The rounded wooden enclosure over the board-colored background: the wood frame with rounded corners and the body's wood at the left.
    static void drawEnclosure(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        const RGB wood{200, 120, 45}, body{135, 82, 30};
        draw::rect(a.cv, 0, 0, a.w, a.h, wood);
        draw::fillRect(a.cv, b.frame, b.frame, b.bodyW, static_cast<lengthType>(a.h - 1 - b.frame), body);
        roundCorners(a.cv, a.w, a.h, b.frame, b.endX, wood);
    }

    /// The body under the strings: a guitar's sound hole where the strings are strummed, a bass's pickup, a violin's or cello's f-holes beside the bridge, and the cello's endpin at its tail.
    void drawBody(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const uint8_t n = stringCount();
        const lengthType top = stringY(0, b, n), bot = stringY(static_cast<uint8_t>(n - 1), b, n);
        const lengthType midY = static_cast<lengthType>((top + bot) / 2);
        if (instrument == kGuitar) {
            const int32_t r = std::min<int32_t>((b.endX - b.bridgeX) / 2 - 2, (b.fbBot - b.fbTop) / 2 - 1);   // clear of the saddle
            if (r < 2) return;
            draw::fillCircle(a.cv, b.pickX, midY, static_cast<lengthType>(r), RGB{0, 0, 0});   // the hole
            draw::circle(a.cv, b.pickX, midY, static_cast<lengthType>(r), RGB{215, 150, 80});  // its rosette
        } else if (instrument == kBass) {
            draw::fillRect(a.cv, static_cast<lengthType>(b.pickX - 1), static_cast<lengthType>(top - 2), 3, static_cast<lengthType>(bot - top + 5), RGB{25, 25, 25});   // the pickup
        } else {
            const lengthType len = std::max<lengthType>(2, static_cast<lengthType>(b.bodyW / 3));
            const lengthType gap = static_cast<lengthType>((top - b.frame) / 2);   // as far from the outer strings as the top string is from the frame
            fHole(a.cv, b.bridgeX, static_cast<lengthType>(top - gap), len, 1);
            fHole(a.cv, b.bridgeX, static_cast<lengthType>(bot + gap), len, -1);
            if (instrument == kCello) draw::fillRect(a.cv, 0, midY, b.frame, 1, kFretC);   // the endpin, out of the tail
        }
    }

    /// One f-hole along the strings, centered at `cx` on row `y`, `len` long, its curls turned by `turn`, so the pair mirror each other.
    static void fHole(const draw::Canvas& cv, lengthType cx, lengthType y, lengthType len, int8_t turn) MM_NONBLOCKING {
        const RGB hole{20, 10, 5};
        const lengthType l = static_cast<lengthType>(cx - len / 2), r = static_cast<lengthType>(cx + len / 2);
        draw::fillRect(cv, l, y, static_cast<lengthType>(r - l + 1), 1, hole);
        draw::pixel(cv, {static_cast<lengthType>(l - 1), static_cast<lengthType>(y + turn), 0}, hole);
        draw::pixel(cv, {static_cast<lengthType>(r + 1), static_cast<lengthType>(y - turn), 0}, hole);
    }

    /// Whether the instrument is bowed: violin and cello.
    bool bowable() const MM_NONBLOCKING { return instrument == kViolin || instrument == kCello; }

    /// The frets, one pixel each, metal or pseudo, one touching the last left out on a small grid; the bridge, the strings from bridge or tailpiece to the nut, the numbers and the dots.
    void drawFingerboard(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const bool fretted = !bowable();
        const RGB pseudoC{120, 80, 40}, nutC{235, 235, 240};
        const uint8_t n = stringCount();
        const lengthType span = static_cast<lengthType>(b.fbBot - b.fbTop + 3);
        lengthType last = b.nutX;   // the last wire drawn, the nut first
        for (uint8_t fr = 1; fr <= frets(); fr++) {
            const lengthType x = fretX(fr, b);
            if (last - x < 2) continue;   // it would touch the last one, where the frets crowd toward the body
            draw::fillRect(a.cv, x, static_cast<lengthType>(b.fbTop - 1), 1, span, fretted ? kFretC : pseudoC);
            last = x;
        }
        drawBridge(a, b);
        const lengthType from = bowable() ? static_cast<lengthType>(b.frame + 1) : static_cast<lengthType>(b.bridgeX - 1);   // a bowed string runs from the tailpiece
        for (uint8_t s = 0; s < n; s++) drawString(a, b, s, from, b.nutX, kStringC);
        if (bowable()) {   // the tailpiece the strings end in, ebony
            const lengthType top = stringY(0, b, n), bot = stringY(static_cast<uint8_t>(n - 1), b, n);
            draw::fillRect(a.cv, static_cast<lengthType>(b.frame + 1), static_cast<lengthType>(top - 1), std::max<lengthType>(1, static_cast<lengthType>(b.bridgeX - 2 - b.frame)),
                           static_cast<lengthType>(bot - top + 3), RGB{25, 18, 12});
        }
        draw::fillRect(a.cv, b.nutX, static_cast<lengthType>(b.fbTop - 1), 2, span, nutC);
        if (b.numRoom) drawFretNumbers(a, b);
        drawDots(a, b);
    }

    /// The bridge across the strings: a guitar's or bass's rosewood bridge with its bone saddle, a violin's or cello's thin maple bridge.
    void drawBridge(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const uint8_t n = stringCount();
        const lengthType top = static_cast<lengthType>(stringY(0, b, n) - 2);
        const lengthType tall = static_cast<lengthType>(stringY(static_cast<uint8_t>(n - 1), b, n) - stringY(0, b, n) + 5);
        if (bowable()) { draw::fillRect(a.cv, b.bridgeX, top, 1, tall, RGB{225, 185, 120}); return; }
        draw::fillRect(a.cv, static_cast<lengthType>(b.bridgeX - 1), top, 2, tall, RGB{70, 35, 15});
        draw::fillRect(a.cv, static_cast<lengthType>(b.bridgeX + 1), top, 1, tall, RGB{235, 230, 210});
    }

    /// String `s` from column `from` to `to` in `c`, two pixels thick on bass and cello.
    void drawString(const AudioScene& a, const Board& b, uint8_t s, lengthType from, lengthType to, RGB c) const MM_NONBLOCKING {
        const lengthType strW = (instrument == kBass || instrument == kCello) ? 2 : 1;
        const lengthType y = stringY(s, b, stringCount());
        draw::fillRect(a.cv, from, static_cast<lengthType>(y > strW / 2 ? y - strW / 2 : 0), static_cast<lengthType>(to - from + 1), strW, c);
    }

    /// Inlay dots in spaces 3, 5, 7, 9, 15, 17, 19 and 21 between the middle strings, and two at 12 and 24 between the outer pairs, like a real fingerboard.
    void drawDots(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const RGB dotC{225, 220, 200};
        const uint8_t n = stringCount();
        const lengthType midY = static_cast<lengthType>((b.fbTop + b.fbBot) / 2);
        for (const uint8_t fr : detail::kDotFrets) {
            if (fr > frets()) break;
            const lengthType x = stopX(fr, b);
            if (fr % 12 != 0) { draw::fillCircle(a.cv, x, midY, 1, dotC); continue; }
            draw::fillCircle(a.cv, x, static_cast<lengthType>((stringY(0, b, n) + stringY(1, b, n)) / 2), 1, dotC);
            draw::fillCircle(a.cv, x, static_cast<lengthType>((stringY(static_cast<uint8_t>(n - 2), b, n) + stringY(static_cast<uint8_t>(n - 1), b, n)) / 2), 1, dotC);
        }
    }

    /// The space numbers along the bottom of the board, each under its space, skipping one that would touch the last drawn as the spaces narrow toward the body.
    void drawFretNumbers(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const RGB numC{225, 220, 200};
        int32_t lastLeft = a.w;
        for (uint8_t fr = 1; fr <= frets(); fr++) {
            char num[3] = {0, 0, 0};
            uint8_t len = 0;
            if (fr >= 10) num[len++] = static_cast<char>('0' + fr / 10);
            num[len++] = static_cast<char>('0' + fr % 10);
            const int32_t tw = len * fonts::kFont4x6.width;
            const int32_t tx = static_cast<int32_t>(stopX(fr, b)) - tw / 2;
            if (tx < b.frame || tx + tw >= lastLeft) continue;
            draw::text(a.cv, fonts::kFont4x6, num, static_cast<lengthType>(tx), static_cast<lengthType>(b.fbBot + 2), numC);
            lastLeft = tx;
        }
    }

    /// Each open string's note named on the head, beside where its string ends at the nut, where the head holds the name.
    void drawNames(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        if (b.bodyW < kLabelW) return;
        const uint8_t n = stringCount();
        for (uint8_t s = 0; s < n; s++) {
            const lengthType y = stringY(s, b, n);
            char name[4];
            openName(openString(s), name);
            lengthType nl = 0;
            while (name[nl]) nl++;
            const lengthType tw = static_cast<lengthType>(nl * fonts::kFont4x6.width);
            const int32_t tx = std::max<int32_t>(b.nutX + 2, b.nutX + 2 + (b.bodyW - 1 - tw) / 2);   // centered on the head
            draw::text(a.cv, fonts::kFont4x6, name, static_cast<lengthType>(tx), static_cast<lengthType>(y > 3 ? y - 3 : 0), RGB{255, 255, 255});
        }
    }

    /// Each kept note as a dot, where the hand plays it under `hands`, else at every place that can play it; a dot is capped by its space and shrinks with its level, so the glow's dim tail is a small dot.
    void drawNotes(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        if (notes == kNotesHands) { drawSounding(a, b); drawPlaced(a, b); drawPlay(a, b); return; }
        const uint8_t n = stringCount();
        for (uint8_t i = 0; i < kAudioNoteKeys; i++) {
            const uint8_t lvl = std::max(level(*a.f, i), hits_.glow(i, a.ms, a.span));
            if (!lvl) continue;
            for (uint8_t s = 0; s < n; s++) {
                const int16_t fret = static_cast<int16_t>(21 + i) - openString(s);
                if (fret >= 0 && fret <= frets()) drawDot(a, b, i, s, fret, lvl);
            }
        }
    }

    /// The note each string plays, at the fret the hand plays it; a string whose glow ended is free again.
    void drawPlaced(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        for (uint8_t s = 0; s < stringCount(); s++) {
            if (hand_.note[s] < 0) continue;
            const uint8_t i = static_cast<uint8_t>(hand_.note[s]);
            const uint8_t lvl = std::max({level(*a.f, i), hits_.glow(i, a.ms, a.span), chordLvl_});
            if (lvl) drawDot(a, b, i, s, hand_.fret[s], lvl);
            else hand_.note[s] = -1;
        }
    }

    /// Note `i`'s dot on string `s` in the space of `fret`, sized to that space and the string gap and shrinking with its level.
    void drawDot(const AudioScene& a, const Board& b, uint8_t i, uint8_t s, int16_t fret, uint8_t lvl) const MM_NONBLOCKING {
        const uint8_t n = stringCount();
        const lengthType bandH = static_cast<lengthType>(b.fbBot - b.fbTop);
        const uint8_t f = static_cast<uint8_t>(fret > 0 ? fret : 1);
        const lengthType space = static_cast<lengthType>(fretX(static_cast<uint8_t>(f - 1), b) - fretX(f, b));   // this fret's columns
        const lengthType r = std::max<lengthType>(1, std::min<lengthType>({static_cast<lengthType>(bandH / n / 3), lengthType{4}, static_cast<lengthType>(space > 2 ? (space - 2) / 2 : 1)}));
        const lengthType rr = std::max<lengthType>(1, static_cast<lengthType>(r * lvl / 255));
        draw::fillCircle(a.cv, stopX(static_cast<uint8_t>(fret), b), stringY(s, b, n), rr,
                         colorFromPalette(*a.pal, static_cast<uint8_t>(i * 256u / kAudioNoteKeys), lvl));
    }

    /// How much string `s` sounds under `hands`, 0..255: a plucked string fading over the glow from its start, a bowed one at its level.
    uint8_t vibration(const AudioScene& a, uint8_t s) const MM_NONBLOCKING {
        const int8_t k = hand_.note[s];
        if (k < 0 || !((play_.started >> s) & 1u)) return 0;
        const uint8_t lvl = a.f->notes[k];
        if (bowable() && play_.bowed(s, a.ms, lvl)) return lvl;
        const uint32_t age = a.ms - play_.startMs[s];
        return a.span && age < a.span ? static_cast<uint8_t>(255u - 255u * age / a.span) : 0;
    }

    /// Each sounding string cyan from the bridge to its fret, the length a real string vibrates, as much as it sounds.
    void drawSounding(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        for (uint8_t s = 0; s < stringCount(); s++) {
            const uint8_t v = vibration(a, s);
            if (!v) continue;
            const lengthType to = hand_.fret[s] == 0 ? b.nutX : fretX(hand_.fret[s], b);
            drawString(a, b, s, b.bridgeX, to, Palette::lerpRGB(kStringC, kVibrateC, v));
        }
    }

    /// Where the strings are struck: each string a strum hit flashes from its own start, so the flash sweeps the strings in the strum's direction; on violin and cello the bow crosses them there and strokes as a player's does: @xref{how-the-bow-strokes}.
    void drawPlay(const AudioScene& a, const Board& b) MM_NONBLOCKING {
        const uint8_t n = stringCount();
        uint8_t bowLvl = 0;   // the loudest bowed string, 0 while none is bowed
        for (uint8_t s = 0; s < n; s++) {
            const uint32_t age = a.ms - play_.startMs[s];
            if (((play_.strum.strings >> s) & 1u) && age < kStrumFlashMs) {
                const uint8_t v = static_cast<uint8_t>(255u - 255u * age / kStrumFlashMs);
                draw::fillRect(a.cv, static_cast<lengthType>(b.pickX - 1), stringY(s, b, n), 3, 1, scale8(RGB{255, 255, 255}, v));
            }
            const int8_t k = hand_.note[s];
            if (k >= 0 && play_.bowed(s, a.ms, a.f->notes[k])) bowLvl = std::max(bowLvl, a.f->notes[k]);
        }
        if (!bowable() || b.bodyW < 6) return;
        const BowReach r = bowReach(a, b);
        strokeBow(a.ms, bowLvl, r);
        drawBow(a, b, r);
    }

    /// Where the bow can be: the row the strings meet it, its length, and the range of `bowAt_` that keeps every string under its hair.
    struct BowReach {
        int32_t contact, len;   ///< the strings' middle row, and the bow's length in rows
        float lo, hi;           ///< the lowest and highest `bowAt_` with the outer strings still under the hair
    };

    /// The bow as long as the instrument is high, meeting the strings at their middle; it turns at kBowEnd of its length, or sooner where an outer string would leave the hair.
    BowReach bowReach(const AudioScene& a, const Board& b) const MM_NONBLOCKING {
        const uint8_t n = stringCount();
        const int32_t first = stringY(0, b, n) - 2, last = stringY(static_cast<uint8_t>(n - 1), b, n) + 1;   // the outer strings' rows, a thick string's included
        const int32_t len = std::max<int32_t>(1, a.h - 3 - 2 * b.frame);
        const int32_t contact = (first + last) / 2;
        const float lo = std::max(1.0f - kBowEnd, static_cast<float>(last - contact) / static_cast<float>(len));
        const float hi = std::min(kBowEnd, static_cast<float>(first + len - contact) / static_cast<float>(len));
        return lo <= hi ? BowReach{contact, len, lo, hi} : BowReach{contact, len, 0.5f, 0.5f};
    }

    /// Move the bow along its length for the time since the last frame while a string is bowed, faster the louder, turning at each new note and at the ends of its reach `r`: @xref{how-the-bow-strokes}.
    void strokeBow(uint32_t ms, uint8_t lvl, const BowReach& r) MM_NONBLOCKING {
        const uint32_t dt = std::min<uint32_t>(ms - bowMs_, 100);   // a stalled frame does not throw the bow
        bowMs_ = ms;
        if (newStroke_) { bowDir_ = static_cast<int8_t>(-bowDir_); newStroke_ = false; }
        if (!lvl) return;   // between bowed notes the bow waits where it is
        bowAt_ += bowDir_ * (kBowSlowest + (kBowFastest - kBowSlowest) * lvl / 255.0f) * static_cast<float>(dt) / 1000.0f;
        if (bowAt_ >= r.hi) { bowAt_ = r.hi; bowDir_ = -1; }
        if (bowAt_ <= r.lo) { bowAt_ = r.lo; bowDir_ = 1; }
    }

    /// The bow upright across the strings at `bowAt_`: one long red stick beside the white hair, tip on top, black frog with pearl eye and ferrule below, cut off where it leaves the instrument.
    void drawBow(const AudioScene& a, const Board& b, const BowReach& r) const MM_NONBLOCKING {
        const int32_t hair = b.pickX;
        const int32_t innerTop = b.frame + 1, innerBot = a.h - 2 - b.frame;
        const float at = std::min(std::max(bowAt_, r.lo), r.hi);   // a resized grid narrows the reach under a resting bow
        const int32_t bot = r.contact + static_cast<int32_t>(at * static_cast<float>(r.len));   // the frog, `at` of the bow below the strings
        const int32_t top = bot - r.len;
        auto put = [&](int32_t x, int32_t y, RGB c) {
            if (y >= innerTop && y <= innerBot) draw::pixel(a.cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, c);
        };
        const RGB white{255, 255, 255};
        for (int32_t y = top; y <= bot; y++) {
            put(hair - 2, y, RGB{255, 30, 0});   // the stick
            put(hair, y, white);
        }
        for (int32_t x = hair - 2; x <= hair; x++) put(x, top, white);   // the tip, joining stick and hair
        for (int32_t x = hair - 2; x < hair; x++) {
            put(x, bot - 4, kFretC);                                      // the ferrule
            for (int32_t y = bot - 3; y <= bot; y++) put(x, y, RGB{0, 0, 0});   // the frog
        }
        put(hair - 1, bot - 2, white);   // its pearl eye
        put(hair - 2, bot + 1, kFretC);  // the screw
    }

    /// How many frets the selected instrument's board has: a guitar's 19, a bass's 20, two octaves on violin and cello, whose fingerboards reach that far.
    uint8_t frets() const MM_NONBLOCKING {
        return instrument == kGuitar ? 19 : instrument == kBass ? 20 : detail::kMaxFrets;
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

    /// Fret `fret`'s column, closing in from the nut at the right toward the body: @xref{fret-spacing-and-hits}.
    lengthType fretX(uint8_t fret, const Board& b) const MM_NONBLOCKING {
        const uint32_t last = 65536u - detail::kStopShare[frets()];   // the last fret's distance from the nut, the board's length
        return static_cast<lengthType>(b.nutX - static_cast<uint32_t>(b.nutX - b.endX) * (65536u - detail::kStopShare[fret]) / last);
    }

    /// Where a finger stops a note at `fret`: the middle of the space behind the fret, the nut for the open string.
    lengthType stopX(uint8_t fret, const Board& b) const MM_NONBLOCKING {
        return fret == 0 ? b.nutX : static_cast<lengthType>((fretX(static_cast<uint8_t>(fret - 1), b) + fretX(fret, b)) / 2);
    }

    /// One corner of the frame: its anchor pixel, the directions the arc runs in, and the arc's radius.
    struct Corner {
        int32_t x0, y0, sx, sy, cr;
    };

    /// Round the frame's corners: outside the quarter arc goes dark, the arc itself stays wood.
    static void roundCorners(const draw::Canvas& cv, lengthType w, lengthType h, lengthType frame, lengthType endX, RGB wood) MM_NONBLOCKING {
        int32_t cr = 9 * static_cast<int32_t>(frame) / 2;   // rounder, but never past the body
        const int32_t maxCr = static_cast<int32_t>(endX);
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
