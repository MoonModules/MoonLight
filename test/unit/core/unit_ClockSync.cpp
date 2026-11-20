/// The fleet clock's arithmetic: the four-timestamp offset and round trip, the filter trusting the least delay, and a discipline that slews small corrections and keeps the clock running forward.

#include "doctest.h"

#include "core/util/ClockSync.h"

using namespace mm;

TEST_CASE("ClockSync: an exchange gives the reference's offset and the round trip, the reference's turnaround taken out") {
    // The reference runs 5 ms ahead; 2 ms each way; it holds the request 1 ms before replying.
    const ClockSample s = clockSample(/*t1=*/1000000, /*t2=*/1007000, /*t3=*/1008000, /*t4=*/1005000);
    CHECK(s.offsetUs == 5000);
    CHECK(s.delayUs == 4000);
    // An asymmetric path, 3 ms out and 1 ms back: the estimate is off by half the asymmetry, within half the round trip.
    const ClockSample a = clockSample(1000000, 1008000, 1008000, 1004000);
    CHECK(a.offsetUs == 6000);
    CHECK(a.delayUs == 4000);
    CHECK(a.offsetUs - 5000 <= a.delayUs / 2);
}

TEST_CASE("ClockSync: the filter trusts the sample with the least round trip of the last eight, and refuses a negative one") {
    ClockFilter f;
    ClockSample out;
    CHECK_FALSE(f.best(out));
    CHECK(f.add({5300, 9000}));
    CHECK(f.add({5010, 400}));   // the least queued
    CHECK(f.add({4700, 7000}));
    CHECK_FALSE(f.add({1, -5}));
    REQUIRE(f.best(out));
    CHECK(out.offsetUs == 5010);
    CHECK(f.count() == 3);
    for (int i = 0; i < ClockFilter::kSamples; i++) f.add({6000, 1000});   // eight newer samples push the good one out
    REQUIRE(f.best(out));
    CHECK(out.offsetUs == 6000);
    CHECK(f.count() == ClockFilter::kSamples);
    f.clear();
    CHECK_FALSE(f.best(out));
}

TEST_CASE("ClockSync: the first estimate steps, a small correction slews at 500 ppm, a large one steps") {
    ClockDiscipline d;
    CHECK_FALSE(d.synced());
    CHECK(d.fleetAt(1000000) == 1000000);
    d.steer(1000000, 250000);   // first: stepped, whatever its size
    CHECK(d.synced());
    CHECK(d.offsetAt(1000000) == 250000);
    d.steer(2000000, 249000);   // 1 ms behind: slewed, 500 us a second
    CHECK(d.offsetAt(2000000) == 250000);
    CHECK(d.offsetAt(3000000) == 249500);
    CHECK(d.offsetAt(4000000) == 249000);
    CHECK(d.offsetAt(9000000) == 249000);   // and no further
    d.steer(10000000, 549000);   // 300 ms ahead: past the step threshold
    CHECK(d.offsetAt(10000000) == 549000);
    d.reset();
    CHECK_FALSE(d.synced());
}

TEST_CASE("ClockSync: a slewed correction backwards still moves fleet time forward") {
    ClockDiscipline d;
    d.steer(0, 0);
    d.steer(1000000, -100000);   // 100 ms behind: under the threshold, so slewed
    int64_t last = d.fleetAt(1000000);
    for (int64_t t = 1001000; t <= 300000000; t += 1000) {
        const int64_t f = d.fleetAt(t);
        REQUIRE(f > last);
        last = f;
    }
    CHECK(d.offsetAt(300000000) == -100000);   // reached after 200 s at 500 ppm
}
