/// @module AudioService

/// The audio's dynamics: the floor learns the room, the gate keeps silence dark, and the window's top follows the music so a quiet song fills the display as a loud one does, never lifting the room's noise.

#include "doctest.h"
#include "core/services/AudioDynamics.h"

namespace {

constexpr float kBlockMs = 21.0f;   // a 512-sample block at 24 kHz

// `seconds` of blocks at this level, the loudest band 6 dB under it as music's often is.
void feed(mm::AudioDynamics& d, float levelDb, float seconds, float bandBelowDb = 6.0f) {
    for (float t = 0; t < seconds * 1000.0f; t += kBlockMs) d.update(levelDb, levelDb - bandBelowDb, kBlockMs);
}

}  // namespace

TEST_CASE("silence keeps the gate shut and every band dark") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 5.0f);
    CHECK_FALSE(d.gateOpen);
    CHECK(d.floorDb == doctest::Approx(-80.0f));
    CHECK(d.toByte(-80.0f) == 0);
    CHECK(d.toByte(0.0f) == 0);
}

TEST_CASE("music above the squelch opens the gate and its loudest band reaches the top") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);
    feed(d, -30.0f, 1.0f);
    CHECK(d.gateOpen);
    CHECK(d.toByte(-36.0f) >= 250);   // the loudest band, 6 dB under the level
    CHECK(d.toByte(-80.0f) == 0);     // the room's noise stays dark
}

TEST_CASE("the floor is the quietest block of the window, so music shorter than it leaves the floor alone") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);
    feed(d, -30.0f, 8.0f);    // a song with no pause, shorter than the window
    CHECK(d.floorDb == doctest::Approx(-80.0f));
}

// Regression: the floor rose only while the level sat within 3 dB of it, so a room 10 dB noisier, a fan or an air conditioner, held the gate open on the noise for good.
TEST_CASE("the floor follows a room growing noisier by any step, within one to two windows") {
    mm::AudioDynamics d;
    feed(d, -60.0f, 25.0f);
    feed(d, -50.0f, 21.0f);   // the fan comes on
    CHECK(d.floorDb == doctest::Approx(-50.0f));
    CHECK_FALSE(d.gateOpen);
}

// Regression: a near-silent block while a mic starts sank the floor 60 dB under the room, where it stayed, and the room's own noise lit the rig.
TEST_CASE("a dip under the room is forgotten after the window, and the gate shuts on the room again") {
    mm::AudioDynamics d;
    feed(d, -129.0f, 0.1f);   // the mic's first blocks
    feed(d, -65.0f, 21.0f);   // the room
    CHECK(d.floorDb == doctest::Approx(-65.0f));
    CHECK_FALSE(d.gateOpen);
}

TEST_CASE("the gate closes only a hysteresis under where it opened, after its hold") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);
    feed(d, -60.0f, 0.5f);
    REQUIRE(d.gateOpen);
    feed(d, -73.0f, 2.0f);   // 7 dB over the floor: under the 10 dB opening, over the 6 dB close
    CHECK(d.gateOpen);
    feed(d, -76.0f, 0.2f);   // under the close threshold, shorter than the hold
    CHECK(d.gateOpen);
    feed(d, -76.0f, 0.2f);
    CHECK_FALSE(d.gateOpen);
}

TEST_CASE("a quiet song after a loud one reaches the top within the release") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);
    feed(d, -10.0f, 5.0f);
    feed(d, -25.0f, 12.0f);   // 15 dB quieter: four time constants of the 3 s release
    CHECK(d.toByte(-31.0f) >= 245);
}

TEST_CASE("music near the room's noise narrows the window: the noise stays dark and the music still fills it") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);
    feed(d, -68.0f, 5.0f);    // music just over the squelch, its loudest band at -74
    CHECK(d.gateOpen);
    CHECK(d.topDb < -73.0f);                 // the top sits on the music, not 40 dB over the floor
    CHECK(d.toByte(d.bandFloorDb) == 0);     // the room's loudest band
    CHECK(d.toByte(-74.0f) >= 245);
}

TEST_CASE("the top holds still while the gate is shut, so a pause does not reset the window") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);
    feed(d, -20.0f, 5.0f);
    const float top = d.topDb;
    feed(d, -80.0f, 10.0f);   // a pause: the gate shuts
    CHECK_FALSE(d.gateOpen);
    CHECK(d.topDb == doctest::Approx(top).epsilon(0.001));
}

TEST_CASE("the gate sits on the bands' scale where the squelch puts it, open or shut") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f, 0.0f);  // the floor at -80, the gate opening at -70, the loudest band level with the RMS throughout
    feed(d, -40.0f, 2.0f, 0.0f);  // music with its loudest band at -40: the window is -80 to -40
    REQUIRE(d.gateOpen);
    CHECK(d.gateToByte() == d.toByte(-70.0f));
    CHECK(d.gateToByte() > 50);   // a quarter of the way up
    CHECK(d.gateToByte() < 75);
    feed(d, -80.0f, 1.0f, 0.0f);  // the music stops and the gate shuts
    REQUIRE_FALSE(d.gateOpen);
    CHECK(d.gateToByte() > 50);   // the line stays where the music must reach
}

// Regression: the bands' window stopped at the RMS floor, but a band's power sits well under the block's RMS, so quiet music got a window of a dB or two and every bar read empty or full.
TEST_CASE("the bands' window stops at the bands' own floor, so quiet music over the squelch still has a scale") {
    mm::AudioDynamics d;
    feed(d, -60.0f, 2.0f, 14.0f);    // the room, its loudest band 14 dB under its RMS as broadband noise's is
    feed(d, -48.0f, 2.0f, 14.0f);    // quiet music just over the squelch
    REQUIRE(d.gateOpen);
    CHECK(d.bandFloorDb == doctest::Approx(-74.0f));
    const uint8_t mid = d.toByte(-68.0f);   // halfway between the bands' floor and the loudest band
    CHECK(mid > 80);
    CHECK(mid < 175);
}
