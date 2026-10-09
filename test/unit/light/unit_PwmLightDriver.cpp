/// @module PwmLightDriver
/// @also Correction

#include "doctest.h"
#include "light/drivers/PwmLightDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "platform/platform.h"

#include <cstring>

namespace {

using R = mm::ChannelRole;

// A driver wired the way the Drivers container wires one: a source buffer, a correction, then the build.
struct Rig {
    mm::PwmLightDriver d;
    mm::Buffer src;
    Rig(const char* pins, mm::nrOfLightsType lights, const R* roles, uint8_t nRoles, uint16_t frequency = 19531) {
        REQUIRE(src.allocate(lights, 3));
        std::strcpy(d.pins, pins);
        d.frequency = frequency;
        d.defineControls();
        d.setSourceBuffer(&src);
        d.correctionForTest().rebuild(255, roles, nRoles);
        d.rebuildCorrection(255);   // the brightness rebuild every driver gets, which fills the 16-bit table
        d.applyState();
    }
    ~Rig() { d.release(); }
    void light(mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = src.data() + static_cast<size_t>(i) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
    uint32_t duty(uint8_t output) { return mm::platform::pwmDutyForTest(d.channelForTest(output)); }
};

const R kRGB[] = {R::Red, R::Green, R::Blue};
const R kRGBCCT[] = {R::Red, R::Green, R::Blue, R::White, R::WarmWhite};

}  // namespace

// The light count is the pins divided by the profile's channels, which is what lets one driver serve a bulb and a fifteen-channel controller.
TEST_CASE("the pins make as many lights as the profile's channels divide them into") {
    Rig bulb("6,7,5,3,4", 1, kRGBCCT, 5);
    CHECK(bulb.d.lightsForTest() == 1);
    bulb.d.release();
    Rig deca("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15", 3, kRGBCCT, 5);
    CHECK(deca.d.lightsForTest() == 3);
}

// Each channel drives its own pin, in the order the pins are listed.
TEST_CASE("each light's corrected channels become the duties of its pins") {
    Rig rig("10,11,12,13,14,15", 2, kRGB, 3);
    rig.light(0, 255, 0, 128);
    rig.light(1, 0, 255, 0);
    rig.d.tick();
    const uint32_t full = rig.d.maxDutyForTest();
    CHECK(rig.duty(0) == full);
    CHECK(rig.duty(1) == 0);
    uint16_t corrected[3];
    rig.d.correctionForTest().applyWide(rig.src.data(), corrected, 3);   // the curve shapes a mid value
    CHECK(rig.duty(2) == (uint64_t{corrected[2]} * full + 32767) / 65535);
    CHECK(rig.duty(3) == 0);
    CHECK(rig.duty(4) == full);
    CHECK(mm::platform::pwmPinForTest(rig.d.channelForTest(5)) == 15);
}

// The curve reaches the timer in 16 bits, so the bottom of a fade keeps a step per level where 8 bits round several levels together.
TEST_CASE("a dim fade under the CIE curve keeps a distinct duty per level") {
    Rig rig("10,11,12", 1, kRGB, 3, 9765);
    uint32_t last = 0;
    for (uint8_t v = 1; v <= 32; v++) {
        rig.light(0, v, 0, 0);
        rig.d.tick();
        CHECK(rig.duty(0) > last);
        last = rig.duty(0);
    }
}

// Each channel's pulse starts an even share of the period after the one before, so full white does not switch every channel at once.
TEST_CASE("the channels' pulses start at evenly spaced phases") {
    Rig rig("10,11,12,13", 1, kRGB, 3, 9765);
    const uint32_t share = (rig.d.maxDutyForTest() + 1) / 4;
    for (uint8_t i = 0; i < 4; i++) CHECK(mm::platform::pwmPhaseForTest(rig.d.channelForTest(i)) == share * i);
}

// The resolution is the most duty steps one period holds at the clock, so a lower frequency gets more of them.
TEST_CASE("the resolution follows from the frequency") {
    Rig fast("10,11,12", 1, kRGB, 3, 19531);
    CHECK(fast.d.maxDutyForTest() == (1u << 12) - 1);
    fast.d.release();
    Rig slow("10,11,12", 1, kRGB, 3, 9765);
    CHECK(slow.d.maxDutyForTest() == (1u << 13) - 1);
}

// Pins that do not complete a light are reported, not guessed at.
TEST_CASE("pins past the last whole light idle and the status says so") {
    Rig rig("10,11,12,13", 1, kRGB, 3);
    CHECK(rig.d.lightsForTest() == 1);
    REQUIRE(rig.d.status() != nullptr);
    CHECK(std::strstr(rig.d.status(), "idle") != nullptr);
}

// The chip has a fixed number of channels, and asking for more is an error rather than a silent truncation.
TEST_CASE("more pins than the chip has channels is an error") {
    Rig rig("0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16", 5, kRGB, 3);
    CHECK(rig.d.lightsForTest() == 0);
    CHECK(rig.d.severity() == mm::MoonModule::Severity::Error);
}

// An unchanged frame writes nothing, so a still light costs no register writes.
TEST_CASE("a duty is written only when it changes") {
    Rig rig("10,11,12", 1, kRGB, 3);
    rig.light(0, 200, 100, 50);
    rig.d.tick();
    const uint32_t before = rig.duty(0);
    mm::platform::pwmWrite(rig.d.channelForTest(0), 7);   // a write behind the driver's back
    rig.d.tick();                                         // the same frame: the driver writes nothing
    CHECK(rig.duty(0) == 7);
    CHECK(before != 7);
}
