/// @module oscillators
/// @also math16

/// The oscillator bank, advanced once per frame and read per pixel: a value stays in its range, two oscillators sharing a rate keep their relationship, a rate change agrees on every device, and one pass a frame.

#include "doctest.h"
#include "core/util/oscillators.h"

using namespace mm;

namespace {

/// Run a bank forward to `untilMs` in `dtMs` steps from `from`, the way a render loop does.
template <uint8_t N>
uint32_t run(OscillatorBank<N>& bank, uint32_t untilMs, uint32_t dtMs, uint32_t from = 0) {
    if (from == 0) bank.advanceTo(0);
    uint32_t t = from;
    while (t + dtMs <= untilMs) { t += dtMs; bank.advanceTo(t); }
    return t;
}

}  // namespace

TEST_CASE("an oscillator stays inside the range the effect asked for") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 120, .low = 10, .high = 200, .phaseOffset = 0, .wave = Wave::Sine});
    int32_t lo = 1 << 30, hi = -(1 << 30);
    uint32_t t = 0;
    for (uint32_t f = 0; f < 2000; f++) {
        bank.advanceTo(t);
        t += 7;                                    // an irregular frame time, as a real loop has
        lo = bank.value(0) < lo ? bank.value(0) : lo;
        hi = bank.value(0) > hi ? bank.value(0) : hi;
    }
    CHECK(lo >= 10);
    CHECK(hi <= 200);
    CHECK(lo < 20);                                // and it actually reaches the ends
    CHECK(hi > 190);
}

TEST_CASE("a range given backwards runs the shape backwards") {
    OscillatorBank<2> bank;
    bank.set(0, {.rate = 60, .low = 0, .high = 100, .phaseOffset = 0, .wave = Wave::Saw});
    bank.set(1, {.rate = 60, .low = 100, .high = 0, .phaseOffset = 0, .wave = Wave::Saw});
    run(bank, 200, 5);
    // Same phase, mirrored ranges: the two are reflections, so they sum to the span.
    CHECK(bank.value(0) + bank.value(1) == 100);
}

TEST_CASE("two oscillators at the same rate hold their phase relationship indefinitely") {
    // The property a composition depends on: layers set a quarter-cycle apart must not drift, or a deliberate arrangement decays into noise over an evening.
    OscillatorBank<2> bank;
    bank.set(0, {.rate = 45, .low = 0, .high = 65535, .phaseOffset = 0,     .wave = Wave::Sine});
    bank.set(1, {.rate = 45, .low = 0, .high = 65535, .phaseOffset = 16384, .wave = Wave::Sine});

    run(bank, 1600, 16);
    const int32_t earlyGap = static_cast<int32_t>(bank.phase(1)) - static_cast<int32_t>(bank.phase(0));
    uint32_t t = 1600;
    for (uint32_t f = 0; f < 200000; f++) { t += 16; bank.advanceTo(t); }   // ~an hour of frames
    const int32_t lateGap = static_cast<int32_t>(bank.phase(1)) - static_cast<int32_t>(bank.phase(0));
    CHECK(earlyGap == lateGap);
}

TEST_CASE("a rate of zero holds the picture still") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 0, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Sine});
    run(bank, 8000, 16);
    CHECK(bank.phase(0) == 0);
    CHECK(bank.value(0) == 32768);                  // a sine at phase zero sits at its midpoint
}

TEST_CASE("changing the rate puts the phase where the fleet time and the new rate place it, as on a device starting after the change") {
    // Live reconfiguration across a wall: a board that restarts after a speed change draws what the others draw.
    OscillatorBank<1> running, late;
    running.set(0, {.rate = 30, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
    uint32_t t = 0;
    for (uint32_t f = 0; f < 50; f++) { running.advanceTo(t); t += 16; }

    Oscillator faster = running.get(0);
    faster.rate = 240;
    running.set(0, faster);
    running.advanceTo(t);
    late.set(0, faster);
    late.advanceTo(t);   // its first frame, after the change
    CHECK(running.unitValue(0) == late.unitValue(0));
}

TEST_CASE("the four waveforms have the shapes their names promise") {
    OscillatorBank<4> bank;
    for (uint8_t i = 0; i < 4; i++)
        bank.set(i, {.rate = 60, .low = 0, .high = 65535, .phaseOffset = 0, .wave = static_cast<Wave>(i)});

    // A quarter of a cycle in (60 BPM is one cycle a second, so 250 ms): the sine is at its peak, the triangle halfway up its rise, the saw a quarter of the way along, the square still low.
    uint32_t t = run(bank, 250, 10);
    CHECK(bank.unitValue(0) > 64000);                                   // Sine
    CHECK(bank.unitValue(1) == doctest::Approx(32768).epsilon(0.05));   // Triangle
    CHECK(bank.unitValue(2) == doctest::Approx(16384).epsilon(0.05));   // Saw
    CHECK(bank.unitValue(3) == 0);                                      // Square

    // Half a cycle: the sine is back through its midpoint on the way down, the triangle at ITS peak (it turns at the half, where the sine turned at the quarter), the square now high.
    t = run(bank, 500, 10, t);
    CHECK(bank.unitValue(0) == doctest::Approx(32768).epsilon(0.02));
    CHECK(bank.unitValue(1) > 64000);
    CHECK(bank.unitValue(3) == 65535);

    // And a full cycle brings every shape back to where it started.
    run(bank, 1000, 10, t);
    CHECK(bank.unitValue(1) == 0);
    CHECK(bank.unitValue(2) == 0);
}

TEST_CASE("a square wave is only ever fully on or fully off") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 90, .low = 0, .high = 255, .phaseOffset = 0, .wave = Wave::Square});
    uint32_t t = 0;
    bool sawLow = false, sawHigh = false;
    for (uint32_t f = 0; f < 300; f++) {
        bank.advanceTo(t);
        t += 11;                                     // 3.3 s, about five cycles at 90 BPM
        const int32_t v = bank.value(0);
        CHECK((v == 0 || v == 255));                 // never anything in between
        sawLow = sawLow || v == 0;
        sawHigh = sawHigh || v == 255;
    }
    CHECK(sawLow);
    CHECK(sawHigh);                                  // and it does switch
}

TEST_CASE("an out-of-range oscillator index reads as zero rather than crashing the device") {
    // An effect driving the bank from a user control must not be able to fault it.
    OscillatorBank<2> bank;
    bank.set(9, {.rate = 60, .low = 0, .high = 100, .phaseOffset = 0, .wave = Wave::Sine});
    run(bank, 160, 16);
    CHECK(bank.value(9) == 0);
    CHECK(bank.phase(9) == 0);
    CHECK(bank.unitValue(200) == 0);
}

TEST_CASE("the first frame establishes the time base instead of jumping the phase") {
    // An effect enabled after the device has been up for an hour starts at zero, not an hour in.
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 60, .low = 0, .high = 65535, .phaseOffset = 0, .wave = Wave::Saw});
    bank.advanceTo(3600000u);                          // first call, an hour on the clock
    CHECK(bank.phase(0) == 0);
    bank.advanceTo(3600016u);
    // One 16 ms frame in, not an hour: at 60 BPM that is 16/1000 of a turn, about 1048.
    CHECK(bank.phase(0) > 1000);
    CHECK(bank.phase(0) < 1100);
}

TEST_CASE("reset restarts the motion without losing the configuration") {
    OscillatorBank<1> bank;
    bank.set(0, {.rate = 120, .low = 5, .high = 9, .phaseOffset = 0, .wave = Wave::Triangle});
    run(bank, 1600, 16);
    bank.reset();
    CHECK(bank.phase(0) == 0);
    CHECK(bank.get(0).rate == 120);
    CHECK(bank.get(0).high == 9);
}
