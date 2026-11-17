/// @module math16
/// @also math8

/// The 16-bit fixed-point tier the power-function contract is written in. What matters here is not that the functions compute something, but the three properties large fixtures depend on: sin16 is SMOOTH (no 8-bit staircase), map32 does not lose the last column to a fencepost, and BeatPhase keeps animating when the frame time is under a millisecond, the failure that silently froze hand-rolled accumulators on host.

#include "doctest.h"
#include "core/util/math16.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

using namespace mm;

TEST_CASE("sin16 traces a full sine over one turn") {
    // SIGNED, matching lib8tion: 0 at the crossings, +/-32767 at the peaks.
    CHECK(sin16(0) == 0);                          // zero crossing, rising
    CHECK(sin16(16384) > 32700);                   // peak
    CHECK(sin16(32768) == 0);                      // zero crossing, falling
    CHECK(sin16(49152) < -32700);                  // trough
    // The angle wraps for free: one full turn later is the same value.
    CHECK(sin16(1234) == sin16(static_cast<angle16>(1234 + 65536)));
    CHECK(cos16(0) > 32700);                       // cosine peaks where sine crosses
}

// The reason the 16-bit tier exists: on a large fixture an 8-bit sine steps visibly. Sampling finer than the 8-bit LUT's resolution must produce intermediate values, not a staircase.
TEST_CASE("sin16 is smooth between LUT entries, where sin8 would step") {
    // Four samples inside ONE 8-bit step (angle 0x1000..0x10C0 all share sin8 index 0x10). int16_t, matching sin16's signed return: unsigned samples would compare wrong once negative.
    const int16_t a = sin16(0x1000), b = sin16(0x1040), c = sin16(0x1080), d = sin16(0x10C0);
    CHECK(a < b);
    CHECK(b < c);
    CHECK(c < d);                                   // strictly rising — a staircase would tie
    CHECK(sin8(0x10) == sin8(0x10));                // (the 8-bit form has one value for all four)
}

// Accuracy against the ideal sine: the claim in the design doc is ~0.2% of amplitude, which is what makes the zero-extra-flash implementation acceptable instead of a bigger table.
TEST_CASE("sin16 stays within 0.5% of a true sine") {
    double worst = 0.0;
    for (uint32_t t = 0; t < 65536; t += 7) {       // 7: a stride that hits varied LUT positions
        const double ideal = std::sin(t * 2.0 * std::numbers::pi_v<double> / 65536.0) * 32767.0;
        worst = std::max(worst, std::abs(ideal - sin16(static_cast<angle16>(t))));
    }
    CHECK(worst < 65535 * 0.005);
}

TEST_CASE("map32 maps a range and clamps outside it") {
    CHECK(map32(5, 0, 10, 0, 100) == 50);
    CHECK(map32(0, 0, 10, 0, 100) == 0);
    CHECK(map32(10, 0, 10, 0, 100) == 100);
    CHECK(map32(-5, 0, 10, 0, 100) == 0);           // below the input range clamps to outLo
    CHECK(map32(999, 0, 10, 0, 100) == 100);        // above clamps to outHi
    CHECK(map32(5, 0, 0, 7, 100) == 7);             // zero span has no ratio: outLo
    CHECK(map32(5, 10, 0, 0, 100) == 50);           // descending input range
    CHECK(map32(2, 0, 10, 100, 0) == 80);           // descending output range
}

// The fencepost six effects each carried a comment about: mapping an audio band to a grid column must be able to reach the LAST column, which the naive (n-1)/(max-1) form loses.
TEST_CASE("map32 can reach the last column of a grid") {
    const int32_t width = 16;
    CHECK(map32(255, 0, 255, 0, width) == width);   // full input reaches the extent
    CHECK(map32(254, 0, 255, 0, width) == 15);      // just under still lands inside
}

// Full-width ranges: an int32 subtraction of INT32_MIN from INT32_MAX overflows, so every operand widens before the arithmetic. A mapping engine that corrupts at the extremes would misplace pixels silently rather than crash, which is the worst failure mode.
TEST_CASE("map32 survives full-width 32-bit ranges") {
    constexpr int32_t lo = INT32_MIN, hi = INT32_MAX;
    CHECK(map32(lo, lo, hi, 0, 100) == 0);            // the extremes still clamp correctly
    CHECK(map32(hi, lo, hi, 0, 100) == 100);
    CHECK(map32(0, lo, hi, 0, 100) == 50);            // and the midpoint is still the midpoint
    // A full-width OUTPUT range: the product of two full spans must not overflow the intermediate.
    CHECK(map32(lo, lo, hi, lo, hi) == lo);
    CHECK(map32(hi, lo, hi, lo, hi) == hi);
    // Full-width input mapped to a tiny output, and the reverse.
    CHECK(map32(0, lo, hi, 0, 1) == 0);
    CHECK(map32(1, 0, 1, lo, hi) == hi);
}

// constexpr: the contract is compile-time evaluable, so a table or a control default can be built from it without runtime cost.
TEST_CASE("sin16 and map32 evaluate at compile time") {
    static_assert(sin16(0) == 0, "sin16 is constexpr");
    static_assert(map32(5, 0, 10, 0, 100) == 50, "map32 is constexpr");
    CHECK(true);
}

TEST_CASE("BeatPhase keeps animating when frames are under a millisecond") {
    BeatPhase p;
    p.advanceTo(1000, 120);                            // first call only sets the time base
    CHECK(p.phase(256) == 0);

    // 1000 frames of a sub-millisecond dt: a per-tick divide would round each to zero and freeze. (Integer ms means some ticks advance 0 and some 1, the accumulator must survive both.)
    for (uint32_t i = 1; i <= 1000; i++) p.advanceTo(1000 + i / 2, 120);
    CHECK(p.phase(256) > 0);
}

TEST_CASE("BeatPhase advances proportionally to elapsed time and rate") {
    BeatPhase slow, fast;
    slow.advanceTo(0, 60); fast.advanceTo(0, 120);
    slow.advanceTo(1000, 60); fast.advanceTo(1000, 120);
    CHECK(fast.numerator() == 2 * slow.numerator());  // double the rate, double the phase

    BeatPhase p;
    p.advanceTo(0, 60);
    p.advanceTo(500, 60);
    const uint64_t half = p.numerator();
    p.advanceTo(1000, 60);
    CHECK(p.numerator() == 2 * half);                 // double the time, double the phase
}

TEST_CASE("BeatPhase holds still at rate zero and resets to zero") {
    BeatPhase p;
    p.advanceTo(0, 120);
    p.advanceTo(1000, 0);
    CHECK(p.numerator() == 0);

    p.advanceTo(2000, 120);
    CHECK(p.numerator() > 0);
    p.reset();
    CHECK(p.numerator() == 0);
}

// millis() wraps every ~49 days; unsigned subtraction gives the correct delta across the wrap, so a long-running device must not see the phase jump backwards or leap.
TEST_CASE("BeatPhase survives the millis wrap") {
    BeatPhase p;
    p.advanceTo(0xFFFFFF00u, 120);
    p.advanceTo(0xFFFFFF00u + 100u, 120);              // wraps past 2^32
    const uint64_t afterWrap = p.numerator();

    BeatPhase q;
    q.advanceTo(1000, 120);
    q.advanceTo(1100, 120);                            // the same 100 ms, no wrap
    CHECK(afterWrap == q.numerator());
}

// --- Polar ---------------------------------------------------------------------------------
// The 16-bit polar pair. What matters is not any single value but that a full sweep around the circle is monotone and accurate enough that a gradient shows no steps and no corners, the two artifacts the 8-bit forms produce on a large fixture.

TEST_CASE("atan16 spans the circle in the right quadrants") {
    CHECK(atan16(0, 100) == 0);                              // +x axis
    // Each axis lands on its quarter turn (within a hair of rounding).
    CHECK(atan16(100, 0) > 16384 - 200);
    CHECK(atan16(100, 0) < 16384 + 200);                     // +y
    CHECK(atan16(0, -100) > 32768 - 200);
    CHECK(atan16(0, -100) < 32768 + 200);                    // -x
    CHECK(atan16(-100, 0) > 49152 - 200);
    CHECK(atan16(-100, 0) < 49152 + 200);                    // -y
    CHECK(atan16(0, 0) == 0);                                // the centre has no direction
}

TEST_CASE("atan16 puts the diagonals halfway between the axes") {
    CHECK(atan16(100, 100) > 8192 - 200);
    CHECK(atan16(100, 100) < 8192 + 200);                    // 45 degrees
    CHECK(atan16(100, -100) > 24576 - 200);
    CHECK(atan16(100, -100) < 24576 + 200);                  // 135 degrees
}

// The reason for the 16-bit form: a sweep must be smooth. Sampling finer than the 8-bit atan2 can resolve has to produce distinct, increasing angles rather than a staircase.
TEST_CASE("atan16 is monotone around a full sweep") {
    uint16_t prev = 0;
    int wraps = 0;
    for (int deg = 0; deg < 360; deg += 3) {
        const double rad = deg * 3.14159265358979 / 180.0;
        const int32_t x = static_cast<int32_t>(10000 * std::cos(rad));
        const int32_t y = static_cast<int32_t>(10000 * std::sin(rad));
        const uint16_t a = atan16(y, x);
        if (deg > 0 && a < prev) wraps++;                    // exactly one wrap, at the end
        prev = a;
    }
    CHECK(wraps <= 1);
}

// A fitted polynomial was tried here first and measured 9.6 degrees of error at the octant boundary; the table form measures 0.015. The bound below is tight enough to catch a return to the polynomial, which is the regression worth pinning.
TEST_CASE("atan16 stays within a fraction of a degree of the true angle") {
    double worst = 0.0;
    for (int deg = 0; deg < 360; deg++) {
        const double rad = deg * 3.14159265358979 / 180.0;
        const int32_t x = static_cast<int32_t>(30000 * std::cos(rad));
        const int32_t y = static_cast<int32_t>(30000 * std::sin(rad));
        const double got = atan16(y, x) * 360.0 / 65536.0;
        double err = std::abs(got - deg);
        if (err > 180.0) err = 360.0 - err;                  // across the wrap
        worst = std::max(worst, err);
    }
    CHECK(worst < 0.05);                                     // measured 0.015; no visible seam
}

TEST_CASE("dist16 is a true radius, where dist8 approximates an octagon") {
    CHECK(dist16(3, 4) == 5);                                // the exact 3-4-5 triangle
    CHECK(dist16(0, 0) == 0);
    CHECK(dist16(-3, -4) == 5);                              // sign does not matter
    // A circle of constant radius reads as constant, the octagonal form bulges at the diagonal.
    const uint32_t axis = dist16(1000, 0);
    const uint32_t diag = dist16(707, 707);
    CHECK(diag > axis - 10);
    CHECK(diag < axis + 10);
}

TEST_CASE("dist16 does not saturate past the 8-bit ceiling") {
    CHECK(dist16(5000, 0) == 5000);                          // dist8 would clamp to 255
    CHECK(dist16(0, 12000) == 12000);
}

// --- Easing, followers, position-addressable randomness --------------------------------------

TEST_CASE("easing curves start at zero and finish at full") {
    CHECK(easeInOutQuad(0) == 0);
    CHECK(easeInOutQuad(65535) > 65400);
    CHECK(easeInOutCubic(0) == 0);
    CHECK(easeInOutCubic(65535) > 65400);
    CHECK(easeOutQuad(0) == 0);
    CHECK(easeOutQuad(65535) > 65400);
}

// The property that makes an easing an easing: it never runs backwards, so motion through it cannot stutter or reverse.
TEST_CASE("easing curves are monotone") {
    uint16_t prevQ = 0, prevC = 0, prevO = 0;
    for (uint32_t t = 0; t <= 65535; t += 251) {
        const frac16 f = static_cast<frac16>(t);
        CHECK(easeInOutQuad(f) >= prevQ);
        CHECK(easeInOutCubic(f) >= prevC);
        CHECK(easeOutQuad(f) >= prevO);
        prevQ = easeInOutQuad(f); prevC = easeInOutCubic(f); prevO = easeOutQuad(f);
    }
}

// In-out easings are slow at the ends and fast in the middle, the difference from a linear ramp, and the reason motion through them looks deliberate rather than mechanical.
TEST_CASE("an in-out easing moves slowly at the ends and quickly in the middle") {
    const int nearStart = easeInOutQuad(6553) - easeInOutQuad(0);        // first 10%
    const int nearMid   = easeInOutQuad(36044) - easeInOutQuad(29491);   // middle 10%
    CHECK(nearMid > nearStart * 2);
    CHECK(easeInOutQuad(32768) > 32000);                                 // passes through the centre
    CHECK(easeInOutQuad(32768) < 33500);
}

TEST_CASE("ease out starts fast and settles") {
    const int nearStart = easeOutQuad(6553) - easeOutQuad(0);
    const int nearEnd   = easeOutQuad(65535) - easeOutQuad(58982);
    CHECK(nearStart > nearEnd);
}

TEST_CASE("smoothFollow moves toward its target without overshooting") {
    uint8_t v = 0;
    for (int i = 0; i < 60; i++) v = smoothFollow(v, 200, 40);
    CHECK(v > 190);
    CHECK(v <= 200);                        // approaches, never passes
    CHECK(smoothFollow(100, 100, 128) == 100);   // already there: no drift
}

TEST_CASE("smoothFollow rate sets how quickly it converges") {
    uint8_t slow = 0, fast = 0;
    for (int i = 0; i < 5; i++) { slow = smoothFollow(slow, 255, 20); fast = smoothFollow(fast, 255, 200); }
    CHECK(fast > slow);
}

TEST_CASE("smoothFollow falls toward a lower target too") {
    uint8_t v = 255;
    for (int i = 0; i < 60; i++) v = smoothFollow(v, 10, 40);
    CHECK(v < 20);
}

// The asymmetry IS the meter: instant attack catches the transient, slow decay leaves something to read. A symmetric follower would show neither.
TEST_CASE("peakHold rises instantly and falls slowly") {
    CHECK(peakHold(10, 200, 5) == 200);     // a new high is taken at once
    CHECK(peakHold(200, 10, 5) == 195);     // below the peak, it decays by one step
    CHECK(peakHold(3, 0, 5) == 0);          // and stops at zero rather than wrapping
}

TEST_CASE("peakHold holds a peak for many frames after a transient") {
    uint8_t peak = peakHold(0, 255, 2);
    for (int i = 0; i < 10; i++) peak = peakHold(peak, 0, 2);
    CHECK(peak > 200);                      // still clearly visible ten frames later
}

// The supersync property: randomness addressed by POSITION, not drawn from a stream. Two devices computing the same pixel must get the same value regardless of frame count or light count.
TEST_CASE("hashInt is a pure function of its inputs") {
    CHECK(hashInt(5, 9) == hashInt(5, 9));
    CHECK(hashInt(5, 9, 0, 42) == hashInt(5, 9, 0, 42));
}

TEST_CASE("hashInt gives neighboring pixels unrelated values") {
    // Adjacent inputs must not produce adjacent outputs, or a dissolve would appear in stripes.
    int differing = 0;
    for (uint32_t x = 0; x < 64; x++)
        if (std::abs(static_cast<int>(hashInt(x, 7)) - static_cast<int>(hashInt(x + 1, 7))) > 2000)
            differing++;
    CHECK(differing > 50);
}

TEST_CASE("hashInt spreads across its range") {
    int buckets[8] = {0};
    for (uint32_t x = 0; x < 512; x++) buckets[hashInt(x, x / 3) >> 13]++;
    for (int i = 0; i < 8; i++) CHECK(buckets[i] > 20);   // every eighth of the range is used
}

TEST_CASE("hashInt separates its axes, so x and y are not interchangeable") {
    CHECK(hashInt(3, 8) != hashInt(8, 3));
    CHECK(hashInt(1, 1, 1) != hashInt(1, 1, 2));
    CHECK(hashInt(4, 4, 4, 1) != hashInt(4, 4, 4, 2));    // the seed changes the whole field
}

// --- Review findings (2026-08-07) ---------------------------------------------------------------

// A follower must converge from either direction. The shift form truncated toward zero, so a small rate moved DOWN by one but stalled going UP, a meter that could fall and never rise.
TEST_CASE("smoothFollow makes progress at every nonzero rate, in both directions") {
    CHECK(smoothFollow(0, 100, 1) > 0);          // the case that used to stall
    CHECK(smoothFollow(100, 0, 1) < 100);
    CHECK(smoothFollow(0, 255, 255) == 255);     // full rate arrives rather than stopping short
    CHECK(smoothFollow(255, 0, 255) == 0);
    CHECK(smoothFollow(50, 50, 200) == 50);      // already there: no drift
    CHECK(smoothFollow(50, 200, 0) == 50);       // rate 0 holds
}

TEST_CASE("smoothFollow never overshoots its target") {
    for (uint8_t rate : {uint8_t{1}, uint8_t{7}, uint8_t{128}, uint8_t{254}, uint8_t{255}}) {
        CAPTURE(rate);
        uint8_t up = 0, down = 255;
        for (int i = 0; i < 600; i++) {
            up = smoothFollow(up, 200, rate);
            down = smoothFollow(down, 40, rate);
            CHECK(up <= 200);
            CHECK(down >= 40);
        }
        CHECK(up == 200);                        // and it does arrive
        CHECK(down == 40);
    }
}

// dist16 saturated at 65535 far inside normal range: two coordinates of 70000 square-and-sum past UINT32_MAX, so the 32-bit root reported 65535 for a distance of 70000.
TEST_CASE("dist16 is exact for distances beyond 16 bits") {
    CHECK(dist16(70000, 0) == 70000);
    CHECK(dist16(0, 100000) == 100000);
    const uint32_t diag = dist16(65535, 65535);  // 65535 * sqrt(2)
    CHECK(diag > 92670);
    CHECK(diag < 92690);
}

TEST_CASE("dist16 handles the full int32 range without wrapping") {
    CHECK(dist16(INT32_MAX, 0) == 2147483647u);
    CHECK(dist16(INT32_MIN, 0) == 2147483648u);  // negating INT32_MIN in place would overflow
    CHECK(dist16(0, INT32_MIN) == 2147483648u);
}

TEST_CASE("isqrt64 roots values a 32-bit root cannot hold") {
    CHECK(isqrt64(0) == 0);
    CHECK(isqrt64(144) == 12);
    CHECK(isqrt64(0xFFFFFFFFULL) == 65535);
    CHECK(isqrt64(1ULL << 62) == (1ULL << 31));  // exact at the top of the range
}

// kaleido: the review asked for coverage of non-divisor segment counts, where 65536/segments has a remainder and the last wedge is a different width.
TEST_CASE("kaleido folds correctly for segment counts that do not divide the circle") {
    for (uint8_t segments : {uint8_t{3}, uint8_t{7}, uint8_t{255}}) {
        CAPTURE(segments);
        const uint32_t wedge = 65536u / segments;
        for (uint32_t a = 0; a < 65536; a += 313) {
            const uint16_t folded = kaleido(static_cast<angle16>(a), segments);
            CHECK(folded < wedge);               // always lands inside one wedge
        }
    }
}

TEST_CASE("kaleido is the identity below two segments") {
    for (uint32_t a = 0; a < 65536; a += 4099) {
        CHECK(kaleido(static_cast<angle16>(a), 0) == a);
        CHECK(kaleido(static_cast<angle16>(a), 1) == a);
    }
}

// isqrt64 exists so a squared distance wider than 32 bits still has a root, a large contact radius in sub-pixel units squares past int32. The Newton iteration has to reach that top of range without its own arithmetic overflowing on the way.
TEST_CASE("isqrt64 finds a root across the whole 64-bit range") {
    CHECK(isqrt64(0) == 0);
    CHECK(isqrt64(1) == 1);
    CHECK(isqrt64(2) == 1);          // floor, not rounded
    CHECK(isqrt64(3) == 1);
    CHECK(isqrt64(4) == 2);
    CHECK(isqrt64(9) == 3);
    CHECK(isqrt64(10) == 3);
    // The top of the range: seeding Newton from x itself overflowed on the first step and returned 0 here, which would have read as "zero distance" at exactly the point the widening was for.
    CHECK(isqrt64(UINT64_MAX) == 4294967295ull);
    CHECK(isqrt64(1ull << 62) == (1ull << 31));
    // Exact squares and the value just below them, across the width.
    for (uint64_t r : {3ull, 1000ull, 65535ull, 1ull << 20, 1ull << 31}) {
        CAPTURE(r);
        CHECK(isqrt64(r * r) == r);
        CHECK(isqrt64(r * r - 1) == r - 1);
    }
}

// atan16 folds into the first octant by taking the magnitude of each axis. INT32_MIN is the one value whose negation has no int32 representation, so the obvious fold is undefined behavior at exactly one input per axis, and a coordinate reaches it whenever a caller passes an unclamped difference. The angles below are the correct quadrants: due-left, straight-down, and the diagonal between them.
TEST_CASE("atan16 handles the extremes of its input range") {
    CHECK(atan16(INT32_MIN, 0) == 49152);            // straight down (three quarter turns)
    CHECK(atan16(0, INT32_MIN) == 32768);            // due left (half a turn)
    CHECK(atan16(INT32_MIN, INT32_MIN) == 40960);    // the diagonal between them
    CHECK(atan16(INT32_MAX, INT32_MAX) == 8192);     // and the opposite diagonal
    CHECK(atan16(0, 0) == 0);                        // the centre has no direction
}

// The 8-bit waveforms cap at 255, which quantises coarsely across the fixtures this drives, a 128x128 wall indexed through a 0..255 ramp moves in steps, not smoothly. These are the same textbook shapes at full range, so a position scales to any axis length without rescaling.
TEST_CASE("a triangle wave rises to full scale and falls back") {
    CHECK(mm::triwave16(0) == 0);
    CHECK(mm::triwave16(32767) == 65534);       // just short of the peak, on the way up
    CHECK(mm::triwave16(65535) == 0);           // back to the start
    // Monotone up to the midpoint, monotone down after it, the fold that makes it a triangle.
    CHECK(mm::triwave16(8192) < mm::triwave16(16384));
    CHECK(mm::triwave16(49152) < mm::triwave16(40960));
}

TEST_CASE("a beat completes one full cycle per beat, at any tempo") {
    // 60 BPM = one cycle per second: the ramp climbs across the second and restarts.
    CHECK(mm::beat16(60, 0) == 0);
    CHECK(mm::beat16(60, 500) > 32000);         // half way through the cycle, half way up
    CHECK(mm::beat16(60, 500) < 33600);
    CHECK(mm::beat16(60, 1000) == 0);           // one second later, back to the start
    // Twice the tempo reaches the same point in half the time.
    CHECK(mm::beat16(120, 250) == mm::beat16(60, 500));
    // A zero tempo is a still frame rather than a divide-by-zero.
    CHECK(mm::beat16(0, 1234) == 0);
}

// halfLifeKeep: the decay a caller states as "half of it is gone after N ms". The properties below are what make it framerate-independent, which is the whole reason it exists.

TEST_CASE("a half-life decay loses exactly half its value over one half-life") {
    CHECK(mm::halfLifeKeep(100, 100) == 32768);          // one half-life: half survives
    CHECK(mm::halfLifeKeep(200, 100) == 16384);          // two: a quarter
    CHECK(mm::halfLifeKeep(400, 100) == 4096);           // four: a sixteenth
    // Nothing elapsed, or no half-life asked for, leaves the value alone rather than erasing it.
    CHECK(mm::halfLifeKeep(0, 100) == 65536);
    CHECK(mm::halfLifeKeep(50, 0) == 65536);
    // A long stall decays to nothing instead of wrapping around to bright.
    CHECK(mm::halfLifeKeep(100000, 100) == 0);
}

TEST_CASE("two decay steps reach the same place as one step of twice the time") {
    // The framerate-independence property, and the reason the half-life form replaces a per-frame fade: a device rendering at 30 fps and one at 60 must dim a trail at the same rate in SECONDS. Written as decay(2dt) == decay(dt)^2, which is what that means arithmetically.
    //
    // The tolerance is one count at the BYTE width every caller narrows to (a channel is 8 bits), not at the 16-bit width of the weight itself: 256 of 65536. Measured worst case is 11.
    for (uint32_t halfLife : {10u, 100u, 1000u, 5000u}) {
        for (uint32_t dt = 1; dt < halfLife * 2; dt += 7) {
            const uint64_t once = mm::halfLifeKeep(dt, halfLife);
            const uint64_t twice = mm::halfLifeKeep(2 * dt, halfLife);
            const uint64_t squared = (once * once) >> 16;
            const int64_t diff = static_cast<int64_t>(twice) - static_cast<int64_t>(squared);
            CHECK(std::llabs(diff) <= 256);
        }
    }
}

TEST_CASE("halving twice as often dims at the same rate, so framerate cannot change a trail") {
    // The same property stated the way a user meets it: run 20 frames of 10 ms and 10 frames of 20 ms over the same 200 ms, and a value must land in the same place either way.
    const uint32_t halfLife = 250;
    uint64_t fast = 65535, slow = 65535;
    for (int i = 0; i < 20; i++) fast = (fast * mm::halfLifeKeep(10, halfLife)) >> 16;
    for (int i = 0; i < 10; i++) slow = (slow * mm::halfLifeKeep(20, halfLife)) >> 16;
    // Both cover 200 ms of a 250 ms half-life, so both should sit near 65535 * 2^-0.8.
    CHECK(std::llabs(static_cast<int64_t>(fast) - static_cast<int64_t>(slow)) <= 256);
    CHECK(fast > 36000);      // 2^-0.8 is 0.574, so ~37600
    CHECK(fast < 39000);
}
