#pragma once

#include "light/effects/AudioNotesBase.h"
#include "light/powerfunctions/playerHands.h"   // PianoHands: the keys two hands could play
#include "core/util/ScratchBuffer.h"

#include <cmath>   // the bass clef's curve

namespace mm {

/// The notes as they are played, written on a grand staff, or as a sequencer's piano roll: time scrolls to the left, each note in its pitch class's color.
/// @card SheetMusicEffect.gif
///
/// The newest moment is the right edge, and `speed` columns a second scroll left. `view` picks how pitch runs up:
///
/// - `staff`: the treble and bass staves of piano music with their clefs, middle C on its ledger line between them, each note a head on its line or space with its stem, ledger lines where it sits beyond the staves, and a sharp before its head. How long a note held writes its value: an eighth with its flag, a quarter, a half with a hollow head, a whole without a stem.
/// - `piano roll`: one row a key where the panel is tall enough, `octave` and `octaves` picking the keys from A0 up, all 79 the engine measures (A0 to D#7) by default, a faint line on each C.
///
/// `notes` starts at `no overtones`; switching it between `all`, `no overtones` and `hands` shows what each keeps: an attack's flash is a dot, a late note one that starts late, an overtone the same color an octave or a twelfth up.
///
/// @technical
///
/// ## A column a moment
///
/// A ring of columns, one per light across, each the keys kept and the keys struck while it was the newest. The columns advance by the elapsed time, a remainder carried, so the notes scroll at one pace at any render rate; a column passed over within one frame holds the frame's keys, so a held note stays one line. A strike marks only the newest column, once per audio frame.
///
/// ## The staff
///
/// A note's place is its diatonic step, seven to the octave, C4 the 28th from C0, its sharp sharing its natural's step; the staves' lines are the steps of E4, G4, B4, D5 and F5 and of G2, B2, D3, F3 and A3. Each step takes as many rows as the panel gives the piano's 52, at least one, middle C at the panel's middle.
///
/// Notes starting within a head's width of each other stand on one column, a chord, as notation writes notes struck together, decided once when they start so a chord stays together as it scrolls; the rest keep their time. One pass from the right gives every note its length, and the head's shape is worked out once a frame.
///
/// A note's value comes from how long it held at 120 BPM, a quarter being 500 ms, each value reaching halfway to the next, until the beat gives the tempo. A stem points up on the head's right below its staff's middle line and down on its left above it, three and a half spaces long. The clefs and the line joining the staves draw where a step has two rows; on a smaller panel the staves stand alone.
class SheetMusicEffect : public AudioNotesBase {
public:
    /// Catalog tags: audio, notes.
    const char* tags() const override { return "💫🎶"; }
    /// The music is written on a plane; a cube takes it extruded.
    Dim dimensions() const override { return Dim::D2; }

    /// How pitch runs up: a grand staff, or one row a key.
    enum View : uint8_t { kStaff, kRoll, kViewCount };

    /// Every note the sieves keep, no hand's limit: the engine's picture, `hands` one click away.
    SheetMusicEffect() { notes = kNotesNoOvertones; }

    /// A grand staff, or a piano roll.
    uint8_t view = kStaff;
    /// The piano roll's first octave, counted from A0.
    uint8_t octave = 0;
    /// How many octaves the piano roll shows, all 79 keys from 7.
    uint8_t octaves = 8;
    /// How many columns scroll by a second.
    uint8_t speed = 40;

    /// Publish the view, which notes show, the piano roll's octaves, the scroll speed and the note names; the octaves hide on the staff.
    void defineControls() override {
        static constexpr const char* kViews[] = {"staff", "piano roll"};
        controls_.addSelect("view", view, kViews, kViewCount);
        addNotesSelect();
        controls_.addControl("octave", octave, 0, 6);
        controls_.setHidden(controls_.count() - 1, view == kStaff);   // defineControls reruns on every change, so this re-hides live
        controls_.addControl("octaves", octaves, 1, 8);
        controls_.setHidden(controls_.count() - 1, view == kStaff);
        controls_.addControl("speed", speed, 1, 100);
        controls_.addControl("noteNames", noteNames);
    }

    /// One column a light across, empty, and the music started again.
    void prepare() override {
        columns_.resize(width());
        for (size_t i = 0; i < columns_.count(); i++) columns_[i] = Column{};
        head_ = 0;
        sinceChord_ = UINT8_MAX;
        started_ = false;
    }

    /// Advance by the elapsed time, add this frame's keys to the newest column, and draw every column: @xref{a-column-a-moment}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginNotes(RGB{0, 0, 0});
        if (a.w == 0 || a.h == 0 || columns_.count() < static_cast<size_t>(a.w)) return;
        advance(a.ms, a.w);
        Column& now = columns_[head_];
        for (uint8_t b = 0; b < kHitBytes; b++) now.kept.bits[b] = static_cast<uint8_t>(now.kept.bits[b] | shown_.bits[b]);
        if (a.f->seq != seenSeq_) {   // a frame's strikes mark one column, however many ticks read the frame
            seenSeq_ = a.f->seq;
            for (uint8_t b = 0; b < kHitBytes; b++) now.struck.bits[b] = static_cast<uint8_t>(now.struck.bits[b] | (struck_[b] & shown_.bits[b]));
        }
        placeChord(now, columns_[head_ > 0 ? head_ - 1 : static_cast<size_t>(a.w) - 1], staffFor(a.h).head);
        if (view == kStaff) drawStaff(a);
        else drawRoll(a);
        endNotes(a);
    }

    /// Key `k`'s diatonic step from C0, C4 being 28: @xref{the-staff}.
    static constexpr uint8_t stepOf(uint8_t k) {
        constexpr uint8_t kStepInOctave[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};   // C C# D D# E F F# G G# A A# B
        const uint8_t midi = static_cast<uint8_t>(21 + k);
        return static_cast<uint8_t>((midi / 12 - 1) * 7 + kStepInOctave[midi % 12]);
    }
    /// Whether key `k` is a sharp, written on its natural's step.
    static constexpr bool isSharp(uint8_t k) {
        const uint8_t pc = static_cast<uint8_t>((21 + k) % 12);
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

protected:
    /// Two hands keep the keys they could play, as Keyboard's do.
    void playedBy(const AudioFrame& f, const uint8_t* hits, NoteSet& set, uint32_t ms) MM_NONBLOCKING override { hands_.filter(f.notes, set, ms, hits); }

private:
    /// One moment of the music.
    struct Column {
        NoteSet kept;     ///< the keys kept while this column was the newest
        NoteSet struck;   ///< the keys struck while it was
        uint8_t back = kUnplaced;   ///< how many columns back the notes starting here stand, the chord they join, decided once when they start
    };
    static constexpr uint8_t kUnplaced = UINT8_MAX;   ///< a column no note has started in yet

    static constexpr uint8_t kBody = 140;        ///< a held note's brightness, below the strike's full
    static constexpr RGB kLine{14, 14, 14};      ///< the piano roll's faint line on each C
    static constexpr RGB kStaffLine{160, 160, 160};   ///< a staff line and a ledger line, bright enough for a panel in daylight
    static constexpr RGB kClef{220, 220, 220};        ///< the clefs
    static constexpr RGB kMark{255, 255, 255};        ///< a sharp
    static constexpr uint8_t kTrebleMiddle = 34, kBassMiddle = 22;   ///< each staff's middle line, B4 and D3, where a stem turns
    static constexpr int32_t kStemSteps = 7;          ///< a stem's length, three and a half spaces
    static constexpr uint32_t kQuarterMs = 375, kHalfMs = 750, kWholeMs = 1500;   ///< note values at 120 BPM, a quarter 500 ms, split halfway: shorter than these an eighth, a quarter, a half, else a whole
    static constexpr uint8_t kMiddleC = 28;      ///< middle C's step, between the staves
    static constexpr uint8_t kBassLow = 18, kTrebleHigh = 38;   ///< the bass staff's lowest line, G2, and the treble's highest, F5
    static constexpr uint8_t kPianoSteps = 52;   ///< A0 to C8, the steps the rows are shared over
    static constexpr int32_t kMaxRows = 16;      ///< the most rows a step takes, a head 31 rows tall

    ScratchBuffer<Column> columns_{*this};   ///< the ring, `head_` the newest, drawn at the right edge
    hands::PianoHands hands_;                ///< where each hand is
    size_t head_ = 0;                        ///< the newest column
    uint32_t lastMs_ = 0;                    ///< the time the music last advanced to
    uint32_t carry_ = 0;                     ///< column-milliseconds not yet a whole column
    uint16_t seenSeq_ = 0;                   ///< the audio frame whose strikes are marked
    uint8_t sinceChord_ = UINT8_MAX;         ///< columns since the last chord began, at most 255
    bool started_ = false;                   ///< whether `lastMs_` holds a time

    /// Move the newest column on by `speed` a second since the last frame; a stall of a whole width clears the music.
    void advance(uint32_t ms, lengthType w) MM_NONBLOCKING {
        if (!started_) { lastMs_ = ms; carry_ = 0; started_ = true; }
        const uint32_t dt = ms - lastMs_;
        lastMs_ = ms;
        const uint64_t moved = static_cast<uint64_t>(carry_) + static_cast<uint64_t>(dt) * speed;
        carry_ = static_cast<uint32_t>(moved % 1000u);
        const uint64_t steps = moved / 1000u;
        const uint32_t n = steps < static_cast<uint64_t>(w) ? static_cast<uint32_t>(steps) : static_cast<uint32_t>(w);
        for (uint32_t i = 0; i < n; i++) {
            head_ = head_ + 1 < static_cast<size_t>(w) ? head_ + 1 : 0;
            columns_[head_] = Column{shown_, NoteSet{}};   // a held note runs on through every column passed
            if (sinceChord_ < UINT8_MAX) sinceChord_++;
        }
    }

    /// Once a note starts in the newest column `now`, after `before`, decide where it stands: with the chord begun under a head's width `head` back, else as a chord of its own; decided once, so a chord stays together as it scrolls.
    void placeChord(Column& now, const Column& before, int32_t head) MM_NONBLOCKING {
        if (now.back != kUnplaced) return;
        bool starts = false;
        for (uint8_t b = 0; b < kHitBytes && !starts; b++) starts = (now.kept.bits[b] & (now.struck.bits[b] | static_cast<uint8_t>(~before.kept.bits[b]))) != 0;
        if (!starts) return;
        if (sinceChord_ < head) now.back = sinceChord_;
        else { now.back = 0; sinceChord_ = 0; }
    }

    /// The column drawn at `x`, the newest at the right edge.
    const Column& columnAt(lengthType x, lengthType w) const MM_NONBLOCKING {
        const size_t back = static_cast<size_t>(w - 1 - x);
        return columns_[(head_ + w - back) % w];
    }

    /// Light (`x`, `y`) when it is on the panel.
    static void put(const AudioScene& a, int32_t x, int32_t y, RGB c) MM_NONBLOCKING {
        if (x >= 0 && y >= 0 && x < a.w && y < a.h) draw::pixel(a.cv, Coord3D{static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, c);
    }

    /// The staff's measures for a panel `h` rows tall: rows a step, middle C's row, and a note head's width.
    struct Staff {
        int32_t rows, mid, head;
        int32_t rowOf(int32_t step) const { return mid - (step - kMiddleC) * rows; }
    };
    static Staff staffFor(lengthType h) {
        const int32_t fit = h / kPianoSteps > 0 ? h / kPianoSteps : 1;   // rows a step
        const int32_t rows = fit < kMaxRows ? fit : kMaxRows;
        return Staff{rows, h / 2, rows > 1 ? 2 * rows + 1 : 2};
    }

    /// A note head's shape for a staff, worked out once a frame: per row from its top, the columns from the head's left it fills, and those a hollow head leaves dark.
    struct Head {
        int8_t lo[2 * kMaxRows - 1], hi[2 * kMaxRows - 1];     ///< the filled columns, none where lo > hi
        int8_t inLo[2 * kMaxRows - 1], inHi[2 * kMaxRows - 1]; ///< the hollow middle, none where inLo > inHi
    };
    static Head headFor(const Staff& st) {
        Head h{};
        const float rx = st.head / 2.0f, ry = st.rows - 0.5f > 0.5f ? st.rows - 0.5f : 0.5f, c = rx - 0.5f;
        for (int32_t r = 0; r < 2 * st.rows - 1; r++) {
            const float v = (r - (st.rows - 1)) / (ry + 0.5f);
            h.lo[r] = h.inLo[r] = static_cast<int8_t>(st.head);
            h.hi[r] = h.inHi[r] = -1;
            for (int32_t j = 0; j < st.head; j++) {
                const float u = (j - c) / rx, d = u * u + v * v;
                if (d <= 1.0f) widen(h.lo[r], h.hi[r], j);
                if (st.rows > 1 && d < 0.35f) widen(h.inLo[r], h.inHi[r], j);
            }
        }
        return h;
    }

    /// Take column `j`, scanned left to right, into the span `lo` to `hi`.
    static void widen(int8_t& lo, int8_t& hi, int32_t j) {
        if (j < lo) lo = static_cast<int8_t>(j);
        hi = static_cast<int8_t>(j);
    }

    /// The grand staff: its lines, the notes as heads with stems by how long each held, ledger lines and sharps, then the clefs and the system's line: @xref{the-staff}.
    void drawStaff(const AudioScene& a) MM_NONBLOCKING {
        const Staff st = staffFor(a.h);
        const Head head = headFor(st);
        for (int32_t s = kBassLow; s <= kTrebleHigh; s += 2)
            if (s != kMiddleC) for (int32_t x = 0; x < a.w; x++) put(a, x, st.rowOf(s), kStaffLine);   // middle C's line is a ledger line, drawn at its notes
        // Right to left: each key's run of columns held unstruck from here, so a note's length is known where it starts, one pass for all.
        uint16_t run[kAudioNoteKeys] = {};
        NoteSet onward;   // the keys the column to the right holds on, kept and not struck again
        for (int32_t x = a.w - 1; x >= 0; x--) drawColumn(a, st, head, x, run, onward);
        drawClefs(a, st);
    }

    /// Column `x` of the right-to-left pass: each kept key's `run` grows where the column to the right holds it on (`onward`), and a note starting here is drawn with its length.
    void drawColumn(const AudioScene& a, const Staff& st, const Head& head, int32_t x, uint16_t (&run)[kAudioNoteKeys], NoteSet& onward) const MM_NONBLOCKING {
        const Column& c = columnAt(static_cast<lengthType>(x), a.w);
        const Column* before = x > 0 ? &columnAt(static_cast<lengthType>(x - 1), a.w) : nullptr;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            if (!c.kept.has(k)) { run[k] = 0; continue; }
            run[k] = static_cast<uint16_t>(onward.has(k) && run[k] < UINT16_MAX ? run[k] + 1 : 1);
            if (startsAt(c, before, k))
                drawNote(a, st, head, x - (c.back == kUnplaced ? 0 : c.back), k, static_cast<uint32_t>(run[k]) * 1000u / speed);
        }
        for (uint8_t b = 0; b < kHitBytes; b++) onward.bits[b] = static_cast<uint8_t>(c.kept.bits[b] & ~c.struck.bits[b]);
    }

    /// One note at column `x`, its head ending there: the ledger lines it needs, its head filled or hollow, its stem and flag by its value, and its sharp.
    void drawNote(const AudioScene& a, const Staff& st, const Head& head, int32_t x, uint8_t k, uint32_t ms) const MM_NONBLOCKING {
        const int32_t s = stepOf(k), x0 = x - st.head + 1;
        drawLedgers(a, st, s, x);
        const RGB col = colorFromPalette(*a.pal, static_cast<uint8_t>((21 + k) % 12 * 256u / 12u), 255);
        drawHead(a, st, head, x0, st.rowOf(s), ms >= kHalfMs, col);   // hollow from a half note on, where the head is tall enough to show it
        if (ms < kWholeMs) drawStem(a, st, s, x, ms, col);
        if (isSharp(k)) drawSharp(a, st, x0, st.rowOf(s));
    }

    /// Whether key `k`'s note starts in column `c`: where struck or first kept; at the left edge, with no column `before` it on the panel, only where struck.
    static bool startsAt(const Column& c, const Column* before, uint8_t k) {
        return c.struck.has(k) || (before && !before->kept.has(k));
    }

    /// The ledger lines a note on step `s`, its head ending at column `x`, needs: those between it and the staff, and middle C's own.
    static void drawLedgers(const AudioScene& a, const Staff& st, int32_t s, int32_t x) MM_NONBLOCKING {
        const auto ledger = [&](int32_t step) { for (int32_t lx = x - st.head; lx <= x + 1; lx++) put(a, lx, st.rowOf(step), kStaffLine); };
        for (int32_t l = s + (s & 1); l < kBassLow; l += 2) ledger(l);
        for (int32_t l = s - (s & 1); l > kTrebleHigh; l -= 2) ledger(l);
        if (s == kMiddleC) ledger(s);
    }

    /// A note head from column `x0`, centered on row `y`, filled or `hollow`.
    static void drawHead(const AudioScene& a, const Staff& st, const Head& head, int32_t x0, int32_t y, bool hollow, RGB col) MM_NONBLOCKING {
        for (int32_t r = 0; r < 2 * st.rows - 1; r++)
            for (int32_t j = head.lo[r]; j <= head.hi[r]; j++)
                if (!(hollow && j >= head.inLo[r] && j <= head.inHi[r])) put(a, x0 + j, y + r - (st.rows - 1), col);
    }

    /// A stem, up on the right below the staff's middle line, down on the left above it, and an eighth's flag down from its tip on its right.
    static void drawStem(const AudioScene& a, const Staff& st, int32_t s, int32_t x, uint32_t ms, RGB col) MM_NONBLOCKING {
        const bool up = s < (s >= kMiddleC ? kTrebleMiddle : kBassMiddle);
        const int32_t y = st.rowOf(s), sx = up ? x : x - st.head + 1, dir = up ? -1 : 1, len = kStemSteps * st.rows;
        for (int32_t i = 1; i <= len; i++) put(a, sx, y + dir * i, col);
        if (ms >= kQuarterMs) return;
        for (int32_t i = 0; i <= st.rows; i++) put(a, sx + 1 + i / 2, y + dir * (len - i), col);
    }

    /// The sharp before a head starting at column `x0` on row `y`: a dot where a step is one row, else its four strokes.
    static void drawSharp(const AudioScene& a, const Staff& st, int32_t x0, int32_t y) MM_NONBLOCKING {
        if (st.rows < 2) { put(a, x0 - 1, y, kMark); return; }
        for (int32_t dy = -2; dy <= 2; dy++) {
            put(a, x0 - 4, y + dy, kMark);
            put(a, x0 - 2, y + dy, kMark);
            if (dy != -1 && dy != 1) continue;
            put(a, x0 - 5, y + dy, kMark); put(a, x0 - 3, y + dy, kMark); put(a, x0 - 1, y + dy, kMark);
        }
    }

    /// The treble and bass clefs at the left over a cleared margin, and the line joining the staves, where a step has two rows or more to draw them.
    static void drawClefs(const AudioScene& a, const Staff& st) MM_NONBLOCKING {
        const int32_t top = st.rowOf(kTrebleHigh), bottom = st.rowOf(kBassLow);
        for (int32_t y = top; y <= bottom; y++) put(a, 0, y, kStaffLine);
        if (st.rows < 2) return;
        clearMargin(a, st);
        drawTreble(a, st);
        drawBass(a, st);
    }

    /// The clefs' margin, three spaces wide, cleared of notes with the staff lines kept.
    static void clearMargin(const AudioScene& a, const Staff& st) MM_NONBLOCKING {
        for (int32_t y = 0; y < a.h; y++) {
            const int32_t s = kMiddleC + (st.mid - y) / st.rows;
            const bool line = (st.mid - y) % st.rows == 0 && ((s >= kBassLow && s <= 26) || (s >= 30 && s <= kTrebleHigh)) && s % 2 == 0;
            for (int32_t x = 1; x < 6 * st.rows; x++) put(a, x, y, line ? kStaffLine : RGB{0, 0, 0});
        }
    }

    /// A 3 by 3 dot centered on (`x`, `y`) in the clef's color.
    static void dot(const AudioScene& a, int32_t x, int32_t y) MM_NONBLOCKING {
        for (int32_t dy = -1; dy <= 1; dy++) for (int32_t dx = -1; dx <= 1; dx++) put(a, x + dx, y + dy, kClef);
    }

    /// Treble: the belly round the G line, the stroke through it from below the staff to above, a loop at its top and a blob at its foot.
    static void drawTreble(const AudioScene& a, const Staff& st) MM_NONBLOCKING {
        const int32_t sp = 2 * st.rows, top = st.rowOf(kTrebleHigh), cx = 1 + sp, g = st.rowOf(32);
        ring(a, static_cast<float>(cx), static_cast<float>(g - st.rows / 2), static_cast<float>(sp), static_cast<float>(sp) * 1.15f);
        const int32_t stemX = cx + st.rows / 2;
        for (int32_t y = top - sp; y <= st.rowOf(30) + sp; y++) put(a, stemX, y, kClef);
        ring(a, static_cast<float>(stemX - st.rows / 2), static_cast<float>(top - sp + st.rows / 2), st.rows * 0.9f, st.rows * 1.1f);
        dot(a, stemX - st.rows, st.rowOf(30) + sp + st.rows / 2);
    }

    /// Bass: the dot on the F line, the curve right and down from it, and the two dots either side of the F line.
    static void drawBass(const AudioScene& a, const Staff& st) MM_NONBLOCKING {
        const int32_t sp = 2 * st.rows, f = st.rowOf(24), bx = 2 + st.rows;
        dot(a, bx, f);
        for (int32_t i = 0; i <= 24; i++) {   // three quarters of a turn, from above the dot round to below the staff's middle
            const float t = -1.9f + 3.3f * i / 24.0f;
            put(a, static_cast<int32_t>(bx + sp * 0.9f * (1.0f + std::sin(t)) * 0.8f + 0.5f), static_cast<int32_t>(f + sp * 0.9f * -std::cos(t) + 0.5f), kClef);
        }
        put(a, bx + 2 * sp, f - st.rows, kClef);
        put(a, bx + 2 * sp, f + st.rows, kClef);
    }

    /// An oval outline centered at (`cx`, `cy`), `rx` by `ry`, in the clef's color.
    static void ring(const AudioScene& a, float cx, float cy, float rx, float ry) MM_NONBLOCKING {
        for (int32_t y = static_cast<int32_t>(cy - ry - 1); y <= static_cast<int32_t>(cy + ry + 1); y++)
            for (int32_t x = static_cast<int32_t>(cx - rx - 1); x <= static_cast<int32_t>(cx + rx + 1); x++) {
                const float u = (x - cx) / rx, v = (y - cy) / ry, d = u * u + v * v;
                if (d >= 0.7f && d <= 1.3f) put(a, x, y, kClef);
            }
    }

    /// The piano roll: every column, each row its keys, bottom the lowest.
    void drawRoll(const AudioScene& a) const MM_NONBLOCKING {
        const uint8_t shown = rollKeys(), s0 = rollFirstKey(shown);
        for (lengthType y = 0; y < a.h; y++) {
            const uint32_t rb = static_cast<uint32_t>(a.h - 1 - y);   // rows counted from the bottom
            const uint8_t lo = static_cast<uint8_t>(s0 + rb * shown / a.h);
            const uint8_t hi = static_cast<uint8_t>(s0 + std::max<uint32_t>(rb * shown / a.h + 1, (rb + 1) * shown / a.h));
            // One row a key or more: a line on the lowest row of each C.
            const bool line = a.h >= shown && (21 + lo) % 12 == 0 && (rb == 0 || (rb - 1) * shown / a.h != rb * shown / a.h);
            for (lengthType x = 0; x < a.w; x++) {
                const RGB px = rollPixel(a, columnAt(x, a.w), lo, hi, line ? kLine : RGB{0, 0, 0});
                if (px.r || px.g || px.b) draw::pixel(a.cv, Coord3D{x, y, 0}, px);
            }
        }
    }

    /// How many keys the piano roll shows: `octaves` of them, at most every key.
    uint8_t rollKeys() const { return static_cast<uint8_t>(octaves * 12u < kAudioNoteKeys ? octaves * 12u : kAudioNoteKeys); }
    /// The lowest key the piano roll shows: `octave`'s first, moved down so all `shown` keys exist.
    uint8_t rollFirstKey(uint8_t shown) const { return static_cast<uint8_t>(octave * 12u + shown <= kAudioNoteKeys ? octave * 12u : kAudioNoteKeys - shown); }

    /// A piano-roll pixel of column `c` over keys `lo` to `hi`: the last kept key's color, a strike shown over a held note, else `back`.
    static RGB rollPixel(const AudioScene& a, const Column& c, uint8_t lo, uint8_t hi, RGB back) MM_NONBLOCKING {
        RGB px = back;
        for (uint8_t k = lo; k < hi; k++) {
            if (!c.kept.has(k)) continue;
            const bool struck = c.struck.has(k);
            px = colorFromPalette(*a.pal, static_cast<uint8_t>((21 + k) % 12 * 256u / 12u), struck ? 255 : kBody);
            if (struck) break;
        }
        return px;
    }
};

}  // namespace mm
