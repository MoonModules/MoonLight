#pragma once

#include "light/effects/AudioNotesBase.h"
#include "light/powerfunctions/playerHands.h"   // PianoHands: the keys two hands could play
#include "core/util/ScratchBuffer.h"

#include <cmath>   // the bass clef's curve

namespace mm {

/// The notes as they are played, written on a grand staff, or as a sequencer's piano roll: time scrolls to the left, each note in its pitch class's color.
/// @card SheetMusicEffect.gif
///
/// A note appears at the right edge the moment it starts and rolls left; the more notes come, the faster the sheet rolls to make room. `view` picks how pitch runs up:
///
/// - `staff`: the treble and bass staves of piano music with their clefs, middle C on its ledger line between them, each note a head with its stem, ledger lines beyond the staves, the key signature after the clefs, and an accidental only on a note outside the key. How long a note held writes its value: an eighth with its flag, a quarter, a half with a hollow head, a whole without a stem.
/// - `piano roll`: one row a key where the panel is tall enough, `octave` and `octaves` picking the keys from A0 up, all 79 the engine measures (A0 to D#7) by default, a faint line on each C.
///
/// `notes` starts at `no overtones`; switching it between `all`, `no overtones` and `hands` shows what each keeps: an attack's flash is a dot, a late note one that starts late, an overtone the same color an octave or a twelfth up.
///
/// @technical
///
/// ## A rolling sheet
///
/// The notes are a list, each with the sheet's position and the time where it started and ended, so a note's value and its place come from its own record.
///
/// A note starts where a key is struck or first kept and ends where it is no longer kept or is struck again; notes starting within one audio frame, 50 ms, stand together, a chord. Each frame redraws the notes on screen, since a held note's value changes while it sounds.
///
/// The sheet rolls left by whole pixels, a remainder carried, so it rolls at one pace at any render rate. It drifts across the panel's width in four seconds, and faster as notes come: the chords a second, followed over two seconds, times the room a chord needs. A chord that comes sooner than that room pushes the sheet on at once, so no two chords overlap.
///
/// ## The staff
///
/// A note's place is its diatonic step, seven to the octave, C4 the 28th from C0, spelled in the frame's key; the staves' lines are the steps of E4, G4, B4, D5 and F5 and of G2, B2, D3, F3 and A3. Each step takes as many rows as the panel gives the piano's 52, at least one, middle C at the panel's middle.
///
/// A note is spelled as notation spells it in the frame's key: a note of the key on its letter unmarked, a minor key's leading tone on its seventh letter raised, a white key the signature alters with a natural, and a black key outside the key sharpened in a sharp key or C, flattened in a flat key.
///
/// The signature stands after the clefs in notation's order and places. Where a step has one row there is no room for it, and every black key carries its own mark.
///
/// The room a chord needs is a head, the reach of its sharp and its flag, and half a head of air, the spacing of printed music; the head's shape is worked out once a frame.
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
    /// The time the sheet takes to drift across the panel's width with no notes coming.
    static constexpr uint32_t kAcrossMs = 4000;

    /// Publish the view, which notes show, the piano roll's octaves and the note names; the octaves hide on the staff.
    void defineControls() override {
        static constexpr const char* kViews[] = {"staff", "piano roll"};
        controls_.addSelect("view", view, kViews, kViewCount);
        addNotesSelect();
        controls_.addControl("octave", octave, 0, 6);
        controls_.setHidden(controls_.count() - 1, view == kStaff);   // defineControls reruns on every change, so this re-hides live
        controls_.addControl("octaves", octaves, 1, 8);
        controls_.setHidden(controls_.count() - 1, view == kStaff);
        controls_.addControl("noteNames", noteNames);
    }

    /// Room for two notes a light across, none yet, and the music started again.
    void prepare() override {
        AudioNotesBase::prepare();
        notes_.resize(2 * static_cast<size_t>(width()));
        first_ = count_ = 0;
        for (uint16_t& h : held_) h = kNone;
        started_ = hasChord_ = false;
        rate_ = 0.0f;
    }

    /// Roll the sheet on by the elapsed time, start and end this frame's notes, and draw the notes on screen: @xref{a-rolling-sheet}.
    void tick() MM_NONBLOCKING override {
        const AudioScene a = beginNotes(RGB{0, 0, 0});
        if (a.w == 0 || a.h == 0 || notes_.count() == 0) return;
        const Staff st = staffFor(a.h);
        const int32_t room = view == kStaff ? st.head + st.marks() + st.head / 2 : 2;   // the roll's bars need only to start apart
        advance(a.ms, a.w, room);
        track(a, room);
        if (view == kStaff) drawStaff(a, st);
        else drawRoll(a);
        endNotes(a);
    }

    /// How a note is written: its diatonic step from C0, C4 being 28, and the mark before its head.
    struct Spelled {
        uint8_t step;         ///< the line or space
        uint8_t accidental;   ///< the mark before the head, an Accidental
    };
    /// The marks a note can carry.
    enum Accidental : uint8_t { kNoMark, kSharp, kFlat, kNatural };

    /// Frame key `key` on the circle of fifths: its signature's sharps above 0, flats below; 0 for C major, A minor or no key yet.
    static constexpr int8_t fifthsOf(uint8_t key) {
        if (key < 1 || key > 24) return 0;
        const uint8_t major = static_cast<uint8_t>(key <= 12 ? key - 1 : (key - 13 + 3) % 12);   // a minor key's relative major
        const uint8_t s = static_cast<uint8_t>(major * 7 % 12);
        return static_cast<int8_t>(s <= 6 ? s : s - 12);
    }

    /// The alteration the signature of `fifths` gives each letter, C to B: sharps from F up the fifths, flats from B down.
    static constexpr void signatureOf(int8_t fifths, int8_t (&alt)[7]) {
        constexpr int8_t kSharpOrder[7] = {3, 0, 4, 1, 5, 2, 6};   // F C G D A E B
        for (int i = 0; i < 7; i++) {
            if (i < fifths) alt[kSharpOrder[i]] = 1;
            if (i < -fifths) alt[kSharpOrder[6 - i]] = -1;
        }
    }
    /// The letter, C to B, that pitch class `pc` is with the letters' alterations `alt`; -1 for none.
    static constexpr int letterOf(int pc, const int8_t (&alt)[7]) {
        constexpr int8_t kLetterPc[7] = {0, 2, 4, 5, 7, 9, 11};
        for (int l = 0; l < 7; l++) if ((kLetterPc[l] + alt[l] + 12) % 12 == (pc + 12) % 12) return l;
        return -1;
    }
    /// MIDI note `midi` written on `letter` altered by `shift`, with `mark` before it.
    static constexpr Spelled spelledAs(int midi, int letter, int shift, uint8_t mark) {
        const int natural = midi - shift;   // the letter's own key, an octave apart from `midi` only for a B sharp or a C flat
        return Spelled{static_cast<uint8_t>((natural / 12 - 1) * 7 + letter), mark};
    }

    /// Key `k` written in frame key `key` as notation spells it: @xref{the-staff}.
    static constexpr Spelled spell(uint8_t k, uint8_t key) {
        const int8_t fifths = fifthsOf(key);
        int8_t alt[7] = {};
        signatureOf(fifths, alt);
        const int midi = 21 + k, pc = midi % 12;
        if (const int letter = letterOf(pc, alt); letter >= 0) return spelledAs(midi, letter, alt[letter], kNoMark);
        if (key > 12 && pc == (key - 13 + 11) % 12) {   // a minor key's leading tone: its seventh letter, raised
            const int letter = (letterOf(key - 13, alt) + 6) % 7, shift = alt[letter] + 1;
            return spelledAs(midi, letter, shift, shift > 0 ? kSharp : kNatural);
        }
        constexpr int8_t kPlain[7] = {};
        if (const int letter = letterOf(pc, kPlain); letter >= 0) return spelledAs(midi, letter, 0, kNatural);
        const bool flat = fifths < 0;
        return spelledAs(midi, letterOf(flat ? pc + 1 : pc - 1, kPlain), flat ? -1 : 1, flat ? kFlat : kSharp);
    }

protected:
    /// Two hands keep the keys they could play, as Keyboard's do.
    void playedBy(const AudioFrame& f, const uint8_t* hits, NoteSet& set, uint32_t ms) MM_NONBLOCKING override { hands_.filter(f.notes, set, ms, hits); }

private:
    /// One note: where on the sheet and when it started and ended.
    struct Note {
        int32_t at = 0;          ///< the sheet's position when it started: its head's last column, then the right edge
        int32_t until = 0;       ///< the sheet's position when it ended
        uint32_t startMs = 0;    ///< when it started
        uint16_t heldMs = 0;     ///< how long it sounded, once it ended
        uint8_t key = 0;         ///< its key, A0 = 0
        bool open = false;       ///< still sounding
    };
    static_assert(sizeof(Note) == 16, "two notes a light across stay within 32 bytes a column");
    static constexpr uint16_t kNone = UINT16_MAX;      ///< no note open on a key
    static constexpr uint32_t kChordMs = 50;           ///< notes starting this close stand together, a chord: about two audio frames
    static constexpr float kRateTau = 2.0f;            ///< seconds the chord rate follows over
    static constexpr float kHeadroom = 1.25f;          ///< how far ahead of the chord rate the sheet rolls, so a push is rare

    static constexpr uint8_t kBody = 140;        ///< a held note's brightness, below the strike's full
    static constexpr RGB kLine{14, 14, 14};      ///< the piano roll's faint line on each C
    static constexpr RGB kStaffLine{160, 160, 160};   ///< a staff line and a ledger line, bright enough for a panel in daylight
    static constexpr RGB kClef{220, 220, 220};        ///< the clefs
    static constexpr RGB kMark = kStaffLine;          ///< an accidental, as gray as the lines, so the notes stand out
    static constexpr uint8_t kTrebleMiddle = 34, kBassMiddle = 22;   ///< each staff's middle line, B4 and D3, where a stem turns
    static constexpr int32_t kSignatureStep = 6;     ///< the columns a signature's sharp or flat takes, its glyph and a gap
    static constexpr int32_t kStemSteps = 7;          ///< a stem's length, three and a half spaces
    static constexpr uint32_t kQuarterMs = 375, kHalfMs = 750, kWholeMs = 1500;   ///< note values at 120 BPM, a quarter 500 ms, split halfway: shorter than these an eighth, a quarter, a half, else a whole
    static constexpr uint8_t kMiddleC = 28;      ///< middle C's step, between the staves
    static constexpr uint8_t kBassLow = 18, kTrebleHigh = 38;   ///< the bass staff's lowest line, G2, and the treble's highest, F5
    static constexpr uint8_t kPianoSteps = 52;   ///< A0 to C8, the steps the rows are shared over
    static constexpr int32_t kMaxRows = 16;      ///< the most rows a step takes, a head 31 rows tall

    ScratchBuffer<Note> notes_{*this};       ///< a ring, `first_` the oldest, the newest at the right edge
    hands::PianoHands hands_;                ///< where each hand is
    size_t first_ = 0, count_ = 0;           ///< the oldest note and how many there are
    uint16_t held_[kAudioNoteKeys] = {};     ///< each key's open note, kNone for none
    int32_t sheet_ = 0;                      ///< how far the sheet has rolled, in pixels: the right edge's position
    int32_t chordAt_ = 0;                    ///< the last chord's position on the sheet
    uint32_t chordMs_ = 0;                   ///< when the last chord started
    uint32_t lastMs_ = 0;                    ///< the time the sheet last rolled to
    uint32_t carry_ = 0;                     ///< thousandths of a pixel rolled, short of a whole one
    float rate_ = 0.0f;                      ///< chords a second, followed over kRateTau
    uint16_t seenSeq_ = 0;                   ///< the audio frame whose strikes are taken
    bool started_ = false;                   ///< whether `lastMs_` holds a time
    bool hasChord_ = false;                  ///< whether a chord has started
    uint8_t key_ = 0;                        ///< this frame's key, as AudioFrame numbers it, 0 for none

    /// Roll the sheet on since the last frame: across the width in kAcrossMs, or faster while chords come, the chord rate times `room` and some headroom: @xref{a-rolling-sheet}.
    void advance(uint32_t ms, lengthType w, int32_t room) MM_NONBLOCKING {
        if (!started_) { lastMs_ = ms; carry_ = 0; started_ = true; }
        const uint32_t dt = ms - lastMs_ < 1000u ? ms - lastMs_ : 1000u;   // a stall rolls at most a second's worth
        lastMs_ = ms;
        const float seconds = static_cast<float>(dt) / 1000.0f;
        rate_ -= rate_ * (seconds < kRateTau ? seconds / kRateTau : 1.0f);
        const float drift = static_cast<float>(w) * 1000.0f / static_cast<float>(kAcrossMs);
        const float busy = rate_ * static_cast<float>(room) * kHeadroom;
        carry_ += static_cast<uint32_t>(std::lround((busy > drift ? busy : drift) * static_cast<float>(dt)));   // pixels a second times milliseconds: thousandths, exact for the drift
        sheet_ += static_cast<int32_t>(carry_ / 1000u);
        carry_ %= 1000u;
    }

    /// Start and end this frame's notes: a key struck or newly kept starts one, a key no longer kept or struck again ends its own.
    void track(const AudioScene& a, int32_t room) MM_NONBLOCKING {
        const bool fresh = a.f->seq != seenSeq_;   // a frame's strikes count once, however many ticks read it
        seenSeq_ = a.f->seq;
        for (uint8_t k = 0; k < kAudioNoteKeys; k++) {
            const bool kept = shown_.has(k);
            const bool struck = fresh && kept && (struck_[k >> 3] & (1u << (k & 7)));
            if (held_[k] != kNone && (!kept || struck)) end(held_[k], a.ms);
            if (kept && held_[k] == kNone) held_[k] = start(k, a.ms, room);
        }
    }

    /// A note on key `k` starting at `ms`, with the chord that started within kChordMs or as a chord of its own, `room` past the last; answers its index.
    uint16_t start(uint8_t k, uint32_t ms, int32_t room) MM_NONBLOCKING {
        if (!hasChord_ || ms - chordMs_ >= kChordMs) {
            if (hasChord_ && sheet_ - chordAt_ < room) sheet_ = chordAt_ + room;   // a chord sooner than its room pushes the sheet on
            chordAt_ = sheet_;
            chordMs_ = ms;
            hasChord_ = true;
            rate_ += 1.0f / kRateTau;
        }
        if (count_ == notes_.count()) drop();
        const size_t i = (first_ + count_++) % notes_.count();
        notes_[i] = Note{chordAt_, chordAt_, ms, 0, k, true};
        return static_cast<uint16_t>(i);
    }

    /// End note `i` at `ms`.
    void end(uint16_t& i, uint32_t ms) MM_NONBLOCKING {
        Note& n = notes_[i];
        const uint32_t held = ms - n.startMs;
        n.heldMs = static_cast<uint16_t>(held < UINT16_MAX ? held : UINT16_MAX);
        n.until = sheet_;
        n.open = false;
        i = kNone;
    }

    /// Drop the oldest note, ending it first if it still sounds.
    void drop() MM_NONBLOCKING {
        const Note& n = notes_[first_];
        if (n.open && held_[n.key] == first_) held_[n.key] = kNone;
        first_ = (first_ + 1) % notes_.count();
        count_--;
    }

    /// The column sheet position `at` stands on, the right edge being the newest.
    int32_t columnOf(int32_t at, lengthType w) const MM_NONBLOCKING { return static_cast<int32_t>(w) - 1 - (sheet_ - at); }

    /// How long note `n` has sounded by `ms`.
    static uint32_t heldFor(const Note& n, uint32_t ms) MM_NONBLOCKING { return n.open ? ms - n.startMs : n.heldMs; }

    /// Light (`x`, `y`) when it is on the panel.
    static void put(const AudioScene& a, int32_t x, int32_t y, RGB c) MM_NONBLOCKING {
        if (x >= 0 && y >= 0 && x < a.w && y < a.h) draw::pixel(a.cv, Coord3D{static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, c);
    }

    /// The staff's measures for a panel `h` rows tall: rows a step, middle C's row, and a note head's width.
    struct Staff {
        int32_t rows, mid, head;
        int32_t rowOf(int32_t step) const { return mid - (step - kMiddleC) * rows; }
        /// The columns a note's marks reach beyond its head: a sharp before it, a flag after.
        int32_t marks() const { return (rows < 2 ? 1 : 5) + rows / 2 + 1; }
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

    /// The grand staff: its lines, the notes as heads with stems by how long each held, ledger lines and accidentals, then the clefs, the signature and the system's line: @xref{the-staff}.
    void drawStaff(const AudioScene& a, const Staff& st) MM_NONBLOCKING {
        key_ = st.rows >= 2 && a.f ? a.f->key : 0;   // where no signature fits, every note carries its own mark
        const Head head = headFor(st);
        for (int32_t s = kBassLow; s <= kTrebleHigh; s += 2)
            if (s != kMiddleC) for (int32_t x = 0; x < a.w; x++) put(a, x, st.rowOf(s), kStaffLine);   // middle C's line is a ledger line, drawn at its notes
        for (size_t n = 0; n < count_; n++) {
            const Note& note = notes_[(first_ + n) % notes_.count()];
            const int32_t x = columnOf(note.at, a.w);
            if (x + st.head >= 0) drawNote(a, st, head, x, note.key, heldFor(note, a.ms));
        }
        drawClefs(a, st, fifthsOf(key_));
    }

    /// One note at column `x`, its head ending there: the ledger lines it needs, its head filled or hollow, its stem and flag by its value, and its sharp.
    void drawNote(const AudioScene& a, const Staff& st, const Head& head, int32_t x, uint8_t k, uint32_t ms) const MM_NONBLOCKING {
        const Spelled sp = spell(k, key_);
        const int32_t s = sp.step, x0 = x - st.head + 1;
        drawLedgers(a, st, s, x);
        const RGB col = colorFromPalette(*a.pal, static_cast<uint8_t>((21 + k) % 12 * 256u / 12u), 255);
        drawHead(a, st, head, x0, st.rowOf(s), ms >= kHalfMs, col);   // hollow from a half note on, where the head is tall enough to show it
        if (ms < kWholeMs) drawStem(a, st, s, x, ms, col);
        if (sp.accidental != kNoMark) drawAccidental(a, st, x0, st.rowOf(s), sp.accidental);
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

    /// Accidental `mark` before a head starting at column `x0` on row `y`: a dot where a step is one row, else its glyph.
    static void drawAccidental(const AudioScene& a, const Staff& st, int32_t x0, int32_t y, uint8_t mark) MM_NONBLOCKING {
        if (st.rows < 2) { put(a, x0 - 1, y, kMark); return; }
        if (mark == kSharp) drawSharp(a, x0, y);
        else if (mark == kFlat) drawFlat(a, x0, y);
        else drawNatural(a, x0, y);
    }

    /// A flat ending a column before `x0`, its bowl on row `y`: a stem and a bowl on its right.
    static void drawFlat(const AudioScene& a, int32_t x0, int32_t y) MM_NONBLOCKING {
        for (int32_t dy = -4; dy <= 2; dy++) put(a, x0 - 4, y + dy, kMark);
        put(a, x0 - 3, y - 1, kMark); put(a, x0 - 2, y, kMark); put(a, x0 - 2, y + 1, kMark); put(a, x0 - 3, y + 2, kMark);
    }

    /// A natural ending a column before `x0`, centered on row `y`: two offset stems joined by two bars.
    static void drawNatural(const AudioScene& a, int32_t x0, int32_t y) MM_NONBLOCKING {
        for (int32_t dy = -3; dy <= 1; dy++) put(a, x0 - 4, y + dy, kMark);
        for (int32_t dy = -1; dy <= 3; dy++) put(a, x0 - 2, y + dy, kMark);
        put(a, x0 - 3, y - 1, kMark); put(a, x0 - 3, y + 1, kMark);
    }

    /// A sharp ending a column before `x0`, centered on row `y`: its four strokes.
    static void drawSharp(const AudioScene& a, int32_t x0, int32_t y) MM_NONBLOCKING {
        for (int32_t dy = -2; dy <= 2; dy++) {
            put(a, x0 - 4, y + dy, kMark);
            put(a, x0 - 2, y + dy, kMark);
            if (dy != -1 && dy != 1) continue;
            put(a, x0 - 5, y + dy, kMark); put(a, x0 - 3, y + dy, kMark); put(a, x0 - 1, y + dy, kMark);
        }
    }

    /// The treble and bass clefs at the left over a cleared margin, with the key signature of `fifths` after them, and the line joining the staves, where a step has two rows or more to draw them.
    static void drawClefs(const AudioScene& a, const Staff& st, int8_t fifths) MM_NONBLOCKING {
        const int32_t top = st.rowOf(kTrebleHigh), bottom = st.rowOf(kBassLow);
        for (int32_t y = top; y <= bottom; y++) put(a, 0, y, kStaffLine);
        if (st.rows < 2) return;
        const int32_t clefs = 6 * st.rows, count = fifths < 0 ? -fifths : fifths;
        clearMargin(a, st, clefs + count * kSignatureStep);
        drawTreble(a, st);
        drawBass(a, st);
        drawSignature(a, st, clefs, fifths);
    }

    /// The key signature from column `x`: its sharps or flats in the order and on the steps notation puts them, on both staves.
    static void drawSignature(const AudioScene& a, const Staff& st, int32_t x, int8_t fifths) MM_NONBLOCKING {
        constexpr uint8_t kSharpSteps[7] = {38, 35, 39, 36, 33, 37, 34};   // F5 C5 G5 D5 A4 E5 B4 on the treble staff
        constexpr uint8_t kFlatSteps[7] = {34, 37, 33, 36, 32, 35, 31};    // B4 E5 A4 D5 G4 C5 F4; the bass staff two octaves down
        const int32_t count = fifths < 0 ? -fifths : fifths;
        for (int32_t i = 0; i < count; i++) {
            const int32_t step = fifths < 0 ? kFlatSteps[i] : kSharpSteps[i], x0 = x + i * kSignatureStep + 5;
            for (const int32_t staff : {step, step - 14})
                drawAccidental(a, st, x0, st.rowOf(staff), fifths < 0 ? kFlat : kSharp);
        }
    }

    /// The clefs' and the signature's margin, `width` columns, cleared of notes with the staff lines kept.
    static void clearMargin(const AudioScene& a, const Staff& st, int32_t width) MM_NONBLOCKING {
        for (int32_t y = 0; y < a.h; y++) {
            const int32_t s = kMiddleC + (st.mid - y) / st.rows;
            const bool line = (st.mid - y) % st.rows == 0 && ((s >= kBassLow && s <= 26) || (s >= 30 && s <= kTrebleHigh)) && s % 2 == 0;
            for (int32_t x = 1; x < width; x++) put(a, x, y, line ? kStaffLine : RGB{0, 0, 0});
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

    /// The rows a key takes in the piano roll, top and bottom.
    struct Rows { int32_t top, bottom; };
    /// The rows key `k` takes when `shown` keys from `s0` share the panel's height, the lowest at the bottom; a row shared by several keys shows each.
    static Rows rollRows(const AudioScene& a, uint8_t k, uint8_t s0, uint8_t shown) MM_NONBLOCKING {
        const int32_t i = k - s0, h = a.h;
        const int32_t lowRb = i * h / shown, highRb = ((i + 1) * h + shown - 1) / shown - 1;   // the rows from the bottom whose keys span `i`
        const int32_t hi = highRb > lowRb ? highRb : lowRb;
        return Rows{h - 1 - hi, h - 1 - lowRb};
    }

    /// The piano roll: a faint line on each C, then each note a bar across its key's rows from where it started to where it ended.
    void drawRoll(const AudioScene& a) const MM_NONBLOCKING {
        const uint8_t shown = rollKeys(), s0 = rollFirstKey(shown);
        if (a.h >= shown) drawCLines(a, s0, shown);   // one row a key or more
        for (size_t n = 0; n < count_; n++) {
            const Note& note = notes_[(first_ + n) % notes_.count()];
            if (note.key >= s0 && note.key < s0 + shown) drawBar(a, note, rollRows(a, note.key, s0, shown));
        }
    }

    /// A faint line on the lowest row of each C among the `shown` keys from `s0`.
    static void drawCLines(const AudioScene& a, uint8_t s0, uint8_t shown) MM_NONBLOCKING {
        for (uint8_t i = 0; i < shown; i++) {
            const uint8_t k = static_cast<uint8_t>(s0 + i);
            if ((21 + k) % 12 != 0) continue;
            const int32_t y = rollRows(a, k, s0, shown).bottom;
            for (lengthType x = 0; x < a.w; x++) put(a, x, y, kLine);
        }
    }

    /// Note `note` as a bar over `rows`, from where it started to where it ended or the right edge while it sounds, its first column at full brightness.
    void drawBar(const AudioScene& a, const Note& note, Rows rows) const MM_NONBLOCKING {
        const int32_t from = columnOf(note.at, a.w), to = note.open ? a.w - 1 : columnOf(note.until, a.w);
        const uint8_t hue = static_cast<uint8_t>((21 + note.key) % 12 * 256u / 12u);
        for (int32_t x = from < 0 ? 0 : from; x <= to; x++) {
            const RGB c = colorFromPalette(*a.pal, hue, x == from ? 255 : kBody);
            for (int32_t y = rows.top; y <= rows.bottom; y++) put(a, x, y, c);
        }
    }

    /// How many keys the piano roll shows: `octaves` of them, at most every key.
    uint8_t rollKeys() const { return static_cast<uint8_t>(octaves * 12u < kAudioNoteKeys ? octaves * 12u : kAudioNoteKeys); }
    /// The lowest key the piano roll shows: `octave`'s first, moved down so all `shown` keys exist.
    uint8_t rollFirstKey(uint8_t shown) const { return static_cast<uint8_t>(octave * 12u + shown <= kAudioNoteKeys ? octave * 12u : kAudioNoteKeys - shown); }

};

}  // namespace mm
