/// @module NetworkSendDriver
/// @also Drivers, Correction

/// Pins the no-allocation-in-loop contract for NetworkSendDriver. The framework fires prepare (topology) and onCorrectionChanged (preset toggle) off the hot path; tick() must read the pre-sized buffer and never allocate.
///
/// A malloc cannot be seen without an interceptor, so the contract is pinned indirectly: after a build-state the corrected_ buffer's pointer and shape are already right, before any tick(), and stay so.

#include "doctest.h"
#include "light/drivers/NetworkSendDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/drivers/Drivers.h"
#include "light/layers/Buffer.h"
#include "platform/platform_clock.h"   // setTestNowMs
#include <chrono>
#include <thread>

// prepare sizes the correction-applied buffer to source-count × out-channels. The size matches what tick() needs on its first send. Calling tick() after prepare must not reallocate, pin the data pointer + shape.
TEST_CASE("NetworkSendDriver sizes corrected_ in prepare, not in tick") {
    mm::Buffer source;
    REQUIRE(source.allocate(64, 3));

    mm::Correction correction;
    mm::rebuildFromPreset(correction, 255, mm::PresetOrder::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();

    // After prepare the resized buffer is already in place.
    REQUIRE(driver.correctedBuffer().data() != nullptr);
    CHECK(driver.correctedBuffer().count() == 64);
    CHECK(driver.correctedBuffer().channelsPerLight() == 3);

    // tick() keeps the backing pointer and shape on every protocol path, all sharing the pre-sized buffer and the member packet; virtual time passes the fps limiter between protocols so each path sends.
    const uint8_t* dataBefore = driver.correctedBuffer().data();
    for (uint8_t p = 0; p < mm::NetworkSendDriver::kProtocolCount; p++) {
        mm::platform::setTestNowMs(1000u + 100u * p);
        driver.protocol = p;
        driver.tick();
        CHECK(driver.correctedBuffer().data() == dataBefore);
        CHECK(driver.correctedBuffer().count() == 64);
        CHECK(driver.correctedBuffer().channelsPerLight() == 3);
    }
    mm::platform::setTestNowMs(0);   // restore real-clock behavior for later cases
}

// A preset toggle from RGB to RGBW grows outChannels from 3 to 4. The grow runs in onCorrectionChanged, off the hot path.
TEST_CASE("NetworkSendDriver grows corrected_ in onCorrectionChanged on RGB → RGBW") {
    mm::Buffer source;
    REQUIRE(source.allocate(32, 3));

    mm::Correction correction;
    mm::rebuildFromPreset(correction, 255, mm::PresetOrder::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();

    REQUIRE(driver.correctedBuffer().channelsPerLight() == 3);

    // Simulate a preset change. Drivers normally drives this; we call directly. The driver owns its Correction, so mutate that copy (not the external one).
    mm::rebuildFromPreset(driver.correctionForTest(), 255, mm::PresetOrder::RGBW);
    driver.onCorrectionChanged();

    CHECK(driver.correctedBuffer().count() == 32);
    CHECK(driver.correctedBuffer().channelsPerLight() == 4);
}

// A brightness-only change keeps outChannels at 3, onCorrectionChanged is still called, but the resize short-circuits (existing buffer already fits).
TEST_CASE("NetworkSendDriver onCorrectionChanged is a no-op when outChannels unchanged") {
    mm::Buffer source;
    REQUIRE(source.allocate(48, 3));

    mm::Correction correction;
    mm::rebuildFromPreset(correction, 255, mm::PresetOrder::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();

    const uint8_t* dataBefore = driver.correctedBuffer().data();
    REQUIRE(dataBefore != nullptr);

    // Brightness change: outChannels stays 3, so the existing allocation fits. The driver owns its Correction, so mutate that copy (not the external one).
    mm::rebuildFromPreset(driver.correctionForTest(), 128, mm::PresetOrder::RGB);
    driver.onCorrectionChanged();

    // Same backing allocation, the resize short-circuited.
    CHECK(driver.correctedBuffer().data() == dataBefore);
    CHECK(driver.correctedBuffer().count() == 48);
    CHECK(driver.correctedBuffer().channelsPerLight() == 3);
}

namespace {
// A driver sending 64 lights to the loopback address, paced from its own task, at `fps`.
struct PacedSend {
    mm::Buffer source;
    mm::NetworkSendDriver driver;
    explicit PacedSend(uint8_t fps = 50) {
        REQUIRE(source.allocate(64, 3));
        driver.setSourceBuffer(&source);
        std::snprintf(driver.receivers, sizeof(driver.receivers), "%s", "127.0.0.1");
        driver.packetGap = 500;   // paced from its own task
        driver.fps = fps;
        driver.applyState();
        REQUIRE(driver.taskStartsForTest() == 1);
    }
};
}  // namespace

// Every prepare restarted the send task, so a show re-preparing its layers kept an FPP from receiving a whole frame.
TEST_CASE("NetworkSendDriver keeps its send task running through a prepare that changed nothing it sends") {
    PacedSend rig;
    mm::NetworkSendDriver& driver = rig.driver;
    driver.applyState();
    CHECK(driver.taskStartsForTest() == 1);   // the stream runs on
    std::snprintf(driver.receivers, sizeof(driver.receivers), "%s", "127.0.0.2");
    driver.applyState();
    CHECK(driver.taskStartsForTest() == 2);   // a new destination restarts it
    std::snprintf(driver.receivers, sizeof(driver.receivers), "%s", "fpp.local");   // a name, whose address the resolver fills in
    driver.applyState();
    REQUIRE(driver.taskStartsForTest() == 3);
    driver.applyState();
    CHECK(driver.taskStartsForTest() == 3);
    driver.release();
}

// A render faster than `fps` ticks several times inside one due frame; a send task held past whole intervals misses a frame per interval, each counted once.
TEST_CASE("NetworkSendDriver counts a frame missed per whole interval the send task holds the last one, however often the render ticks inside it") {
    PacedSend rig(50);   // a frame is due every 20 ms
    mm::NetworkSendDriver& driver = rig.driver;
    driver.holdFrameForTest();
    for (uint32_t ms = 1100; ms < 1200; ms += 5) {   // twenty render ticks across five due frames
        mm::platform::setTestNowMs(ms);
        driver.tick();
    }
    CHECK(driver.framesDropped().value_or(0) == 5);
    driver.release();
    mm::platform::setTestNowMs(0);
}

// A render tick off the interval grid kept every later count late, so a long hold read short.
TEST_CASE("NetworkSendDriver counts missed frames on the send's interval grid when the render ticks off it") {
    PacedSend rig(50);   // a frame is due every 20 ms
    mm::NetworkSendDriver& driver = rig.driver;
    driver.holdFrameForTest();
    for (uint32_t ms = 1100; ms <= 1240; ms += 7) {   // 7 ms ticks across the boundaries 1100, 1120 ... 1240
        mm::platform::setTestNowMs(ms);
        driver.tick();
    }
    CHECK(driver.framesDropped().value_or(0) == 8);
    driver.release();
    mm::platform::setTestNowMs(0);
}

// A frame the driver keeps is corrected into, not rebuilt: a channel the new profile gives no role would keep the old profile's byte.
TEST_CASE("NetworkSendDriver sends 0 on a channel the profile gives no role, whatever the profile before it sent there") {
    mm::Buffer source;
    REQUIRE(source.allocate(4, 3));
    std::memset(source.data(), 255, 4 * 3);
    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    mm::rebuildFromPreset(driver.correctionForTest(), 255, mm::PresetOrder::RGB);
    std::snprintf(driver.receivers, sizeof(driver.receivers), "%s", "127.0.0.1");   // a destination, since a driver with none idles
    driver.applyState();
    mm::platform::setTestNowMs(1000);
    driver.tick();
    REQUIRE(driver.correctedBuffer().data()[2] == 255);   // blue, lit
    const mm::ChannelRole redGreen[3] = {mm::ChannelRole::Red, mm::ChannelRole::Green, mm::ChannelRole::None};
    driver.correctionForTest().rebuild(255, redGreen, 3);
    driver.onCorrectionChanged();
    bool dark = false;
    for (uint32_t i = 1; i <= 200 && !dark; i++) {   // a tick is skipped while the send task still holds the last frame
        mm::platform::setTestNowMs(1000 + 100 * i);
        driver.tick();
        dark = driver.correctedBuffer().data()[2] == 0;
        if (!dark) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(dark);                                          // no role, so dark
    CHECK(driver.correctedBuffer().data()[0] == 255);
    mm::platform::setTestNowMs(0);
}

// Found on MoonShine's memory sheet: a driver that was off held a frame of its own, and no driver reported the frame it held.
TEST_CASE("NetworkSendDriver holds its staging frame only while it is on, and counts it in its memory") {
    mm::Buffer source;
    REQUIRE(source.allocate(64, 3));
    mm::NetworkSendDriver driver;
    driver.setEnabled(false);
    mm::rebuildFromPreset(driver.correctionForTest(), 255, mm::PresetOrder::RGB);
    driver.setSourceBuffer(&source);
    driver.applyState();
    CHECK(driver.correctedBuffer().data() == nullptr);   // off: no frame
    const size_t off = driver.dynamicBytes();
    driver.setEnabled(true);
    driver.applyState();
    REQUIRE(driver.correctedBuffer().data() != nullptr);
    CHECK(driver.dynamicBytes() >= off + 64 * 3);        // on: the frame, counted
    driver.setEnabled(false);
    driver.applyState();
    CHECK(driver.correctedBuffer().data() == nullptr);   // off again: given back
    CHECK(driver.dynamicBytes() == off);
}
