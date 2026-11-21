/// @module modmatrix

#include "doctest.h"
#include "light/powerfunctions/modmatrix.h"

#include <cstdint>

namespace {

// A frame with a distinct value in every field the sources read, so a source reading the wrong field shows.
mm::AudioFrame distinctFrame() {
    mm::AudioFrame f;
    f.level = 11;
    f.levelSmoothed = 22;
    f.bass = 33;
    f.mid = 44;
    f.treble = 55;
    f.onset = 66;
    f.onsetLow = 77;
    f.onsetMid = 88;
    f.onsetHigh = 99;
    f.flux = 111;
    f.peakMag = 122;
    f.harmonyChange = true;
    for (uint8_t b = 0; b < 16; b++) f.bands[b] = static_cast<uint8_t>(130 + b);
    return f;
}

}  // namespace

// Each named source reads its own field and no other.
TEST_CASE("modmatrix reads each source from its own field") {
    const mm::AudioFrame f = distinctFrame();
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::read(S::kNone, f) == 0);
    CHECK(mm::modmatrix::read(S::kLevel, f) == 11);
    CHECK(mm::modmatrix::read(S::kSmooth, f) == 22);
    CHECK(mm::modmatrix::read(S::kBass, f) == 33);
    CHECK(mm::modmatrix::read(S::kMid, f) == 44);
    CHECK(mm::modmatrix::read(S::kTreble, f) == 55);
    CHECK(mm::modmatrix::read(S::kOnset, f) == 66);
    CHECK(mm::modmatrix::read(S::kKick, f) == 77);
    CHECK(mm::modmatrix::read(S::kSnare, f) == 88);
    CHECK(mm::modmatrix::read(S::kHats, f) == 99);
    CHECK(mm::modmatrix::read(S::kFlux, f) == 111);
    CHECK(mm::modmatrix::read(S::kPeak, f) == 122);
    CHECK(mm::modmatrix::read(S::kChord, f) == 255);
}

// The bands read their own slot, first and last.
TEST_CASE("modmatrix reads each band from its own slot") {
    const mm::AudioFrame f = distinctFrame();
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::read(S::kBand0, f) == 130);
    CHECK(mm::modmatrix::read(S::kBand7, f) == 137);
    CHECK(mm::modmatrix::read(S::kBand15, f) == 145);
}

// A source outside the list, as a stored index from an older list would be, reads silence.
TEST_CASE("modmatrix reads an unknown source as silence") {
    const mm::AudioFrame f = distinctFrame();
    CHECK(mm::modmatrix::read(mm::modmatrix::source(250), f) == 0);
    CHECK(mm::modmatrix::read(mm::modmatrix::source(static_cast<uint8_t>(mm::modmatrix::Source::kCount)), f) == 0);
}

// The wide fields clamp at 255, so a loud frame cannot push a source past full.
TEST_CASE("modmatrix clamps the wide fields at full") {
    mm::AudioFrame f;
    f.level = 300;
    f.levelSmoothed = 1000;
    f.peakMag = 4096;
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::read(S::kLevel, f) == 255);
    CHECK(mm::modmatrix::read(S::kSmooth, f) == 255);
    CHECK(mm::modmatrix::read(S::kPeak, f) == 255);
}

// The chord source is a pulse: full on the one block the harmony moved, none otherwise.
TEST_CASE("modmatrix reads the chord change as a pulse") {
    mm::AudioFrame f;
    CHECK(mm::modmatrix::read(mm::modmatrix::Source::kChord, f) == 0);
    f.harmonyChange = true;
    CHECK(mm::modmatrix::read(mm::modmatrix::Source::kChord, f) == 255);
}

// The option list and the enum agree, so the select shows every source once.
TEST_CASE("modmatrix offers one option per source") {
    CHECK(sizeof(mm::modmatrix::kSourceOptions) / sizeof(mm::modmatrix::kSourceOptions[0]) == mm::modmatrix::kSourceCount);
    CHECK(mm::modmatrix::kSourceCount == 31);   // none, twelve signals, sixteen bands, two LFOs
}

// Depth 0 leaves the base untouched, whatever the source.
TEST_CASE("modmatrix at depth 0 leaves the base alone") {
    const mm::AudioFrame f = distinctFrame();
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::apply(60, S::kLevel, 0, f, 0, 255) == 60);
    CHECK(mm::modmatrix::apply(60, S::kBass, 0, f, 0, 255) == 60);
}

// A full source at full depth adds the whole depth.
TEST_CASE("modmatrix at full source and full depth adds the whole depth") {
    mm::AudioFrame f;
    f.bass = 255;
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::apply(60, S::kBass, 255, f, 0, 255) == 255);   // 60 + 255 clamps at 255
    CHECK(mm::modmatrix::apply(60, S::kBass, 100, f, 0, 255) == 160);
}

// A half source adds half the depth.
TEST_CASE("modmatrix at half source adds half the depth") {
    mm::AudioFrame f;
    f.bass = 128;
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::apply(60, S::kBass, 100, f, 0, 255) == 110);   // 60 + 128 * 100 / 255
}

// The value clamps to the parameter's own range at both ends.
TEST_CASE("modmatrix clamps at the parameter's own range") {
    mm::AudioFrame f;
    f.bass = 255;
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::apply(200, S::kBass, 100, f, 150, 210) == 210);
    CHECK(mm::modmatrix::apply(150, S::kBass, 100, f, 150, 210) == 210);
    CHECK(mm::modmatrix::apply(160, S::kNone, 255, f, 150, 210) == 160);   // no source, no lift, base kept
    CHECK(mm::modmatrix::apply(150, S::kBass, 0, f, 150, 210) == 150);
}

// The frame carries no LFO, so an LFO read against the frame alone is silence; the inputs carry its value.
TEST_CASE("modmatrix reads an LFO from the inputs, not the frame") {
    const mm::AudioFrame f = distinctFrame();
    using S = mm::modmatrix::Source;
    CHECK(mm::modmatrix::read(S::kLfo1, f) == 0);
    mm::modmatrix::Inputs in{f};
    in.lfo[0] = 70;
    in.lfo[1] = 200;
    CHECK(mm::modmatrix::read(S::kLfo1, in) == 70);
    CHECK(mm::modmatrix::read(S::kLfo2, in) == 200);
    CHECK(mm::modmatrix::read(S::kBass, in) == 33);   // the audio sources still read the frame
}

// Rate r runs r/32 cycles a second: rate 32 is one cycle a second, so a quarter second is a quarter turn of the sine.
TEST_CASE("modmatrix LFO runs its rate by elapsed time") {
    mm::modmatrix::Lfo lfo;
    CHECK(lfo.value() == mm::sin8(0));
    lfo.advance(32, 250);
    CHECK(lfo.phase == 16384);   // a quarter of 65536
    CHECK(lfo.value() == mm::sin8(64));
    lfo.advance(32, 750);
    CHECK(lfo.phase == 0);       // a full cycle in a second
}

// Rate 0 holds the LFO where it is.
TEST_CASE("modmatrix LFO at rate 0 holds") {
    mm::modmatrix::Lfo lfo;
    lfo.advance(32, 100);
    const uint16_t held = lfo.phase;
    lfo.advance(0, 10000);
    CHECK(lfo.phase == held);
}

// The fraction of a phase unit a tick leaves carries to the next, so a slow LFO keeps its rate at any frame time: rate 1 for 16 s in 16 ms ticks is exactly half a cycle.
TEST_CASE("modmatrix LFO carries its fraction across ticks") {
    mm::modmatrix::Lfo lfo;
    for (int t = 0; t < 1000; t++) lfo.advance(1, 16);
    CHECK(lfo.phase == 32768);
}
