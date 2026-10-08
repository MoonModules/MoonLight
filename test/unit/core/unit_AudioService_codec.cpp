/// @module AudioService
/// @also I2cBusModule

/// The codec in front of a microphone is runtime configuration: AudioService names it and its address, on the board's I2C bus.

#include "doctest.h"
#include "core/services/AudioService.h"

#include <string>

// A direct digital microphone is the default, so a board without a codec never touches a bus.
TEST_CASE("a microphone asks for no codec until one is named") {
    mm::AudioService audio;
    CHECK(audio.codecType() == mm::platform::CodecType::None);
    audio.codec = mm::AudioService::kCodecEs8311;
    CHECK(audio.codecType() == mm::platform::CodecType::Es8311);
}

// Every codec setting re-initializes the microphone, as a pin change does, so an edit applies live.
TEST_CASE("changing the codec or its address re-initializes the microphone") {
    mm::AudioService audio;
    for (const char* name : {"codec", "codecAddr"}) CHECK(audio.affectsPrepare(name));
}

// The bus scan reads 0x18 as this codec only while the wired microphone actually uses one.
TEST_CASE("the codec is reported on the bus only while the local microphone uses one") {
    mm::AudioService audio;
    mm::MoonModule::I2cDevice dev[2];
    audio.mode = mm::AudioService::kLocalMode;
    CHECK(audio.i2cDevices(dev, 2) == 0);   // no codec named
    audio.codec = mm::AudioService::kCodecEs8311;
    REQUIRE(audio.i2cDevices(dev, 2) == 1);
    CHECK(dev[0].addr == 0x18);
    CHECK(std::string(dev[0].role) == "ES8311");
    audio.mode = mm::AudioService::kSimMode;
    CHECK(audio.i2cDevices(dev, 2) == 0);   // a synthesized signal drives no chip
}
