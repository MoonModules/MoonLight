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

// `seconds` of music swinging 12 dB, loud and soft a fifth of a second each, its loudest band 6 dB under its level.
void song(mm::AudioDynamics& d, float seconds) {
    for (int b = 0; b < static_cast<int>(seconds * 1000.0f / kBlockMs); b++) {
        const float level = (b / 10) % 2 ? -26.0f : -38.0f;
        d.update(level, level - 6.0f, kBlockMs);
    }
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
    feed(d, -50.0f, 25.0f);   // the fan comes on: the floor learns it within the window, the gate shuts within its quiet hold
    CHECK(d.floorDb == doctest::Approx(-50.0f));
    CHECK_FALSE(d.gateOpen);
}

// Regression: a near-silent block while a mic starts sank the floor 60 dB under the room, where it stayed, and the room's own noise lit the rig.
TEST_CASE("a dip under the room is forgotten after the window, and the gate shuts on the room again") {
    mm::AudioDynamics d;
    feed(d, -129.0f, 0.1f);   // the mic's first blocks
    feed(d, -65.0f, 25.0f);   // the room
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
    feed(d, -76.0f, 2.8f);   // under the close threshold, the room still in the window: shorter than the quiet hold
    CHECK(d.gateOpen);
    feed(d, -76.0f, 0.4f);
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
    CHECK(d.topDb < -73.0f);                 // the top sits on the music, not a window's span over the floor
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
    feed(d, -50.0f, 2.0f, 0.0f);  // music with its loudest band at -50: the window is -80 to -50
    REQUIRE(d.gateOpen);
    CHECK(d.gateToByte() == d.toByte(-70.0f));
    CHECK(d.gateToByte() > 75);   // a third of the way up
    CHECK(d.gateToByte() < 95);
    feed(d, -80.0f, 3.5f, 0.0f);  // the music stops, the room within the window, and the gate shuts after its quiet hold
    REQUIRE_FALSE(d.gateOpen);
    CHECK(d.gateToByte() > 75);   // the line stays where the music must reach
}

// The gate's floor rises into a long song while the window's bottom stays at the room, so the line is placed on the window the bars use.
TEST_CASE("after a long song the gate line still sits on the bands' scale") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);   // the room, its loudest band at -86
    song(d, 120.0f);
    REQUIRE(d.gateOpen);
    REQUIRE(d.bandFloorDb > d.roomBandFloorDb + 10.0f);   // the gate's band floor rose into the song
    CHECK(d.gateToByte() == d.toByte(d.bandFloorDb + d.squelchDb));
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

// Regression (2026-11-06, PO bench): a song outlasting the floor's window taught the window's bottom its quiet moments, and the bars lost energy until a pause. The bottom learns only while the gate is shut.
TEST_CASE("a long song keeps the window wide: the bottom's floor learns only while the gate is shut") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f);          // the room, its loudest band at -86
    song(d, 120.0f);                 // a two-minute song with no pause, far past the 10 s half window
    REQUIRE(d.gateOpen);
    REQUIRE(d.floorDb > -40.0f);     // the gate's floor rose into the song, so soft passages keep the gate open
    CHECK(d.toByte(-86.0f) == 0);    // the room's loudest band stays dark
    CHECK(d.toByte(-47.0f) > 100);   // a band mid-window still reads mid, not compressed to the bottom
}

// Regression: a song with no pause lifted the floor into its quiet moments, and the gate shut on every soft passage while its bars still showed (.221, Satie, 2026-11-04).
TEST_CASE("a soft passage near a floor that rose into the music keeps its bars, the gate open") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f, 6.0f);
    song(d, 25.0f);                 // a song with no pause: the floor rises into its quiet moments
    REQUIRE(d.gateOpen);
    REQUIRE(d.floorDb > -40.0f);
    feed(d, -36.0f, 2.0f, 12.0f);   // a soft passage, its level under the close threshold, its loudest band still in the window
    CHECK(d.gateOpen);
}

// When the music stops, the line's silence sits far under the window: no bar shows, so the gate shuts at once and stays shut.
TEST_CASE("stopped music goes dark within the hold and stays dark on the line's silence") {
    mm::AudioDynamics d;
    feed(d, -80.0f, 2.0f, 14.0f);
    song(d, 25.0f);
    REQUIRE(d.gateOpen);
    feed(d, -76.0f, 0.3f, 14.0f);   // the player stops: the line's own silence
    CHECK_FALSE(d.gateOpen);
    feed(d, -76.0f, 30.0f, 14.0f);
    CHECK_FALSE(d.gateOpen);
}
