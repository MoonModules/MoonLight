#pragma once

#include "light/effects/EffectBase.h"
#include "light/powerfunctions/audioReadout.h"   // draw::noteNames

namespace mm {

/// The mic's frequency bands spread across X, the sixteen, every third octave it measures or the piano's keys, each column lit bottom-up by its magnitude.
/// @card AudioSpectrumEffect.gif
///
/// The classic equalizer display. The bands scale to any width, so a wide grid gives each several columns and a narrow one samples. On a single row the bars collapse to per-column brightness.
///
/// `readouts` draws the rest of the frame with the bars: the dominant frequency, the gate, the bass, mids and treble, a level meter with a square for each hit, and the twelve pitch classes: @xref{the-readouts}. `noteNames` writes the three strongest classes at the top left, "C E G", and the key with its tuning at the right, "Am +12".
///
/// @attribution Andrew Tuline, troyhacks
///
/// @guide
///
/// ## Comparing audio engines
///
/// The sweep colors with sixteen bands and the readouts off draw what WLED-MM's GEQ PPA draws: the same bars and heights, and its sweep's timing through the palette, so two boards side by side differ only in their audio. Point one at the other's WLED audio sync to see both engines through this one effect.
///
/// @technical
///
/// ## The readouts
///
/// Each mark is a bar wide, rounded up as the rows are: along the bars' top, white over the dominant frequency; at the right edge, the height where the gate opens, white while open and red while the room is gated.
///
/// The bass, mids and treble each draw as a grey line across their own bars at their level, a silent one on the bottom row, so all three always show; a line flashes white on its range's own hit. On the third octaves and the keys each bar's top row flashes white on its own hit, so a tom fill sweeps across the bars and a key flashes as it is struck.
///
/// On a grid at least three rows tall a level meter sits under the bars, as tall as a bar is wide rounded up, with a square that flashes red on each hit. With `harmony` on a row of the twelve pitch classes sits over them, C at the left, each in its palette color as bright as it sounds, going dark for an instant as the chord changes.
///
/// ## The instrument
///
/// This effect is the audio test instrument, so it shows the raw level rather than a smoothed one, which lags by tens of milliseconds. A frame from audio sync carries the sixteen alone, so the third octaves and the keys fall back to them; with `harmony` off the keys fall back to the third octaves.
///
/// Of `n` bars, bar `i` spans columns `i*w/n` up to `(i+1)*w/n` and rises `band*h/255` rows, as GEQ PPA computes them for sixteen, so the two compare pixel for pixel.
///
/// The floor needs no line of its own: the window's bottom never sinks under it, so the bars stand on the floor.
class AudioSpectrumEffect : public EffectBase {
public:
    /// Catalog tags for the visual catalog.
    const char* tags() const override { return "💫🎶"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// The coloring choices, in option order.
    enum ColorMode : uint8_t { kColorHeight, kColorPerBand, kColorSweep, kColorModeCount };

    /// The palette by height (a VU), per band, or as GEQ PPA's sweep.
    uint8_t colorMode = kColorPerBand;
    /// The level meter, hits, gate and peak, drawn with the bars.
    bool readouts = true;
    /// The bands to draw, in option order.
    enum Bands : uint8_t { kBands16, kBandsThirds, kBandsNotes, kBandsCount };
    /// WLED-MM's sixteen, every third octave, or the piano's keys.
    uint8_t bands = kBandsThirds;
    /// The sweep's swings through the palette a minute.
    uint8_t speed = 60;
    /// The strongest pitch classes top left, the key top right.
    bool noteNames = false;

    /// Publish the coloring choice, the readouts, the note names and the band count.
    void defineControls() override {
        static constexpr const char* kColorOptions[] = {"height", "per-band", "sweep"};
        controls_.addSelect("colorMode", colorMode, kColorOptions, kColorModeCount);
        // defineControls reruns on every control change, so picking sweep shows its speed.
        controls_.addControl("speed", speed, 1, 255);
        controls_.setHidden(controls_.count() - 1, colorMode != kColorSweep);
        controls_.addControl("readouts", readouts);
        controls_.addControl("noteNames", noteNames);
        static constexpr const char* kBandOptions[] = {"16", "thirds", "notes"};
        controls_.addSelect("bands", bands, kBandOptions, kBandsCount);
    }

    /// Draw the level meter and the pitch classes, one bar per column from its band's magnitude, then the gate and the peak over them.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width();
        const lengthType h = height();

        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* f = AudioService::latestFrame();
        // The scale asked for, falling back to the next coarser one a frame does not carry: @xref{the-instrument}.
        const Bands scale = bands == kBandsNotes && f->notesCount > 0 ? kBandsNotes
                          : bands != kBands16 && f->thirdsCount > 0 ? kBandsThirds : kBands16;
        const uint8_t n = scale == kBandsNotes ? f->notesCount : scale == kBandsThirds ? f->thirdsCount : 16;   // 79 keys, or 28 thirds at the default 24 kHz

        // A bar's width rounded up, so a bar under two columns wide still gets two rows of meter and a mark two wide: the width every readout takes.
        const lengthType barW = static_cast<lengthType>((w + n - 1) / n);
        const lengthType levelRows = readouts ? std::min<lengthType>(barW, static_cast<lengthType>((h - 1) / 2)) : 0;
        const lengthType specH = static_cast<lengthType>(h - levelRows);
        // The pitch classes take as many rows from the top, where the frame measured them.
        const lengthType pitchRows = readouts && f->notesCount > 0 ? std::min<lengthType>(barW, static_cast<lengthType>((specH - 1) / 2)) : 0;
        const lengthType barsH = static_cast<lengthType>(specH - pitchRows);   // the rows the bars rise in

        const uint32_t ms = elapsed();
        if (f->onset) onsetUntilMs_ = ms + kOnsetFlashMs;
        if (f->harmonyChange) changeUntilMs_ = ms + kChangeFlashMs;
        const uint8_t rangeHits[3] = {f->onsetLow, f->onsetMid, f->onsetHigh};
        for (uint8_t r = 0; r < 3; r++) if (rangeHits[r]) rangeUntilMs_[r] = ms + kOnsetFlashMs;
        // Each bar's own hit, where the bars are the bands that carry them.
        const uint8_t* hits = !readouts || h == 1 ? nullptr : scale == kBandsNotes ? f->notesHits : scale == kBandsThirds ? f->thirdsHits : nullptr;
        if (hits) markBarHits(hits, n, ms);
        if (levelRows) drawLevelMeter(cv, f->level, specH, ms < onsetUntilMs_);
        if (pitchRows) drawPitchClasses(cv, f->chroma, pitchRows, ms < changeUntilMs_);

        // The bars sit on the level meter, or on the floor without one.
        const lengthType specBottom = static_cast<lengthType>(specH - 1);
        for (uint8_t band = 0; band < n; band++) {
            const uint8_t mag = scale == kBandsNotes ? f->notes[band] : scale == kBandsThirds ? f->thirds[band] : f->bands[band];
            // The bar's height over the spectrum area, where a single row lights whole.
            const lengthType lit = (h == 1) ? 1 : static_cast<lengthType>(static_cast<uint32_t>(mag) * barsH / 255u);
            // The bar's one color, once per bar rather than per pixel; height varies it per row instead.
            const RGB whole = colorMode == kColorSweep ? sweepColor(band, ms, speed)
                            : colorMode == kColorPerBand ? colorFromPalette(*Palettes::active(), static_cast<uint8_t>(band * 256u / n), 255)
                            : RGB{0, 0, 0};
            // This band's columns, which a narrow grid rounds to none for some bands.
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(band) * w / n);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(band + 1) * w / n);
            // A hit shows on the bar's top row, the bottom one for a bar too low to have rows.
            const bool hit = hits && hitAt(barsLit_, band);
            const lengthType rows = hit && lit == 0 ? 1 : lit;
            for (lengthType x = x0; x < x1; x++)
                draw::bar(cv, x, specBottom, rows, draw::Grow::Up, [&](lengthType row) {
                    return hit && row + 1 == rows ? RGB{255, 255, 255} : barColor(mag, row, barsH, h == 1, whole);
                });
        }
        if (readouts && h > 1) {   // a single row has no room over its bars
            drawRanges(cv, *f, scale, n, specH, barsH, ms);   // first, so the marks sit on top of the lines
            if (f->gate) drawGate(cv, f->gate, f->gateOpen, specH, barsH, barW);
            // The dominant frequency, while there is sound to have one.
            if (f->peakHz && (f->gate == 0 || f->gateOpen)) drawPeak(cv, f->peakHz, scale, n, barW, pitchRows);
        }
        if (noteNames) draw::noteNames(cv, *f, w, pitchRows);
    }

    /// Where `hz` sits across `n` bars, in bars: the key from A0, the third-octave index, or that mapped between the sixteen when only those show.
    static float peakBar(uint16_t hz, Bands scale, uint8_t n) MM_NONBLOCKING {
        if (scale == kBandsNotes) return std::fmin(std::fmax(12.0f * std::log2(static_cast<float>(hz) / 27.5f), 0.0f), static_cast<float>(n - 1));
        const float third = 17.0f + 3.0f * std::log2(static_cast<float>(hz) / 1000.0f);   // band 17 is 1 kHz
        if (scale == kBandsThirds) return std::fmin(std::fmax(third, 0.0f), static_cast<float>(n - 1));
        if (third <= kSixteenOfThirds[0]) return 0.0f;
        for (uint8_t i = 0; i + 1 < 16; i++)
            if (third < kSixteenOfThirds[i + 1])
                return i + (third - kSixteenOfThirds[i]) / static_cast<float>(kSixteenOfThirds[i + 1] - kSixteenOfThirds[i]);
        return 15.0f;
    }

private:
    /// How long the square stays red after a hit: an onset lasts one audio block, 21 ms at 24 kHz, too short for the eye.
    static constexpr uint32_t kOnsetFlashMs = 100;
    uint32_t onsetUntilMs_ = 0;   ///< until when the square shows the last hit
    /// How long the pitch classes go dark after a chord change, one block too short for the eye as an onset is.
    static constexpr uint32_t kChangeFlashMs = 100;
    uint32_t changeUntilMs_ = 0;  ///< until when the row shows the last change
    uint32_t rangeUntilMs_[3] = {};   ///< until when each range's line shows its last hit
    uint8_t barHitTick_[kAudioNoteKeys] = {};     ///< when each bar's last hit came, in 4 ms ticks: a byte a bar, checked every frame so it never wraps unseen
    uint8_t barsLit_[10] = {};        ///< a bit per bar while it shows its hit

    /// Light each bar of `n` whose bit is set in `hits`, and darken each whose hit is older than the flash.
    void markBarHits(const uint8_t* hits, uint8_t n, uint32_t ms) MM_NONBLOCKING {
        const uint8_t tick = static_cast<uint8_t>(ms >> 2);
        for (uint8_t b = 0; b < n && b < kAudioNoteKeys; b++) {
            const uint8_t bit = static_cast<uint8_t>(1u << (b & 7));
            if (hitAt(hits, b)) { barHitTick_[b] = tick; barsLit_[b >> 3] |= bit; }
            else if (static_cast<uint8_t>(tick - barHitTick_[b]) >= kOnsetFlashMs / 4) barsLit_[b >> 3] &= static_cast<uint8_t>(~bit);
        }
    }

    /// The level meter on the rows from `top` down: the raw level across the palette, since a lag is wrong for judging response, and at its right end a square, red while a hit shows.
    void drawLevelMeter(const draw::Canvas& cv, uint16_t level, lengthType top, bool hit) MM_NONBLOCKING {
        const lengthType side = static_cast<lengthType>(height() - top);
        const lengthType w = width() > side ? static_cast<lengthType>(width() - side) : 0;   // the meter stops short of the square
        const lengthType litW = static_cast<lengthType>(static_cast<uint32_t>(level > 255 ? 255 : level) * w / 255u);
        for (lengthType y = top; y < height(); y++) {
            draw::bar(cv, 0, y, litW, draw::Grow::Right, [w](lengthType x) {
                return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(static_cast<uint32_t>(x) * 255u / (w > 1 ? w : 1)), 255);
            });
            if (hit) draw::bar(cv, w, y, static_cast<lengthType>(width() - w), draw::Grow::Right, [](lengthType) { return RGB{255, 0, 0}; });
        }
    }

    /// The dominant frequency as a white mark `barW` wide along the bars' top row `y`, centered where it sits on the bars' axis.
    void drawPeak(const draw::Canvas& cv, uint16_t hz, Bands scale, uint8_t n, lengthType barW, lengthType y) MM_NONBLOCKING {
        const float center = (peakBar(hz, scale, n) + 0.5f) * width() / n;
        const lengthType x = static_cast<lengthType>(std::fmin(std::fmax(center - barW / 2.0f, 0.0f), static_cast<float>(width() - barW)));
        draw::bar(cv, x, y, barW, draw::Grow::Right, [](lengthType) { return RGB{255, 255, 255}; });
    }

    /// The bass, mids and treble, each a grey line across its own bars at its height, a silent range on the bottom row and white while its hit shows: the range's level read where its bands are.
    void drawRanges(const draw::Canvas& cv, const AudioFrame& f, Bands scale, uint8_t n, lengthType specH, lengthType barsH, uint32_t ms) MM_NONBLOCKING {
        const lengthType w = width();
        const float edgesHz[4] = {0.0f, 140.0f, 2240.0f, 0.0f};   // between 125 and 160 Hz, and 2 and 2.5 kHz
        const uint8_t levels[3] = {f.bass, f.mid, f.treble};
        for (uint8_t r = 0; r < 3; r++) {
            const float fromBar = r == 0 ? 0.0f : peakBar(static_cast<uint16_t>(edgesHz[r]), scale, n) + 0.5f;
            const float toBar = r == 2 ? static_cast<float>(n) : peakBar(static_cast<uint16_t>(edgesHz[r + 1]), scale, n) + 0.5f;
            // A column's gap at each inner edge, so two ranges at the same height never read as one line.
            const lengthType x0 = static_cast<lengthType>(fromBar * w / n + (r > 0 ? 1.0f : 0.0f));
            const lengthType x1 = static_cast<lengthType>(std::fmin(toBar * w / n - (r < 2 ? 1.0f : 0.0f), static_cast<float>(w)));
            const lengthType rise = static_cast<lengthType>(static_cast<uint32_t>(levels[r]) * barsH / 255u);
            const lengthType y = static_cast<lengthType>(specH - (rise ? rise : 1));
            const RGB color = ms < rangeUntilMs_[r] ? RGB{255, 255, 255} : RGB{160, 160, 160};   // white while the range's hit shows
            if (x1 > x0) draw::bar(cv, x0, y, static_cast<lengthType>(x1 - x0), draw::Grow::Right, [color](lengthType) { return color; });
        }
    }

    /// The twelve pitch classes across the top `rows`, C at the left, each in its palette color as bright as it sounds; dark while a chord change shows.
    void drawPitchClasses(const draw::Canvas& cv, const uint8_t (&chroma)[12], lengthType rows, bool change) MM_NONBLOCKING {
        const lengthType w = width();
        for (uint8_t c = 0; c < 12; c++) {
            const lengthType x0 = static_cast<lengthType>(static_cast<uint32_t>(c) * w / 12u);
            const lengthType x1 = static_cast<lengthType>(static_cast<uint32_t>(c + 1) * w / 12u);
            const RGB color = change ? RGB{0, 0, 0} : colorFromPalette(*Palettes::active(), static_cast<uint8_t>(c * 256u / 12u), chroma[c]);
            for (lengthType y = 0; y < rows; y++) draw::bar(cv, x0, y, static_cast<lengthType>(x1 - x0), draw::Grow::Right, [color](lengthType) { return color; });
        }
    }

    /// The row a bar of the gate's height reaches over the `barsH` rows ending at `specH`, as a mark `barW` wide at the right edge: white while open, red while shut.
    void drawGate(const draw::Canvas& cv, uint8_t gate, bool open, lengthType specH, lengthType barsH, lengthType barW) MM_NONBLOCKING {
        const lengthType rise = static_cast<lengthType>(static_cast<uint32_t>(gate) * barsH / 255u);
        const lengthType y = static_cast<lengthType>(specH - (rise ? rise : 1));
        const RGB color = open ? RGB{255, 255, 255} : RGB{255, 0, 0};
        draw::bar(cv, static_cast<lengthType>(width() - barW), y, barW, draw::Grow::Right, [color](lengthType) { return color; });
    }

    /// GEQ PPA's sweep through the palette, `bpm` swings a minute, each bar 32 ms further along; GEQ PPA's own is 60.
    static RGB sweepColor(uint8_t band, uint32_t ms, uint8_t bpm) MM_NONBLOCKING {
        return colorFromPalette(*Palettes::active(), beatsin8(bpm, ms, 0, 255, band * 32u, 0), 255);
    }

    /// One pixel of a bar by the coloring choice, each from the palette; on a single row the bar's brightness carries its magnitude.
    RGB barColor(uint8_t mag, lengthType row, lengthType specH, bool oneRow, RGB whole) const MM_NONBLOCKING {
        if (colorMode != kColorHeight)   // sweep and per-band: one color per bar
            return oneRow ? RGB{scale8(whole.r, mag), scale8(whole.g, mag), scale8(whole.b, mag)} : whole;
        // Height: the palette up the spectrum's own rows, so the full height reaches the palette's end; a single row reads it at the magnitude.
        if (specH <= 1) return colorFromPalette(*Palettes::active(), mag, mag);
        return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(static_cast<uint32_t>(row) * 255u / (specH - 1)), 255);
    }
};

} // namespace mm
