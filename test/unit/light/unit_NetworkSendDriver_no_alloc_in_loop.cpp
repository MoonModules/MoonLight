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

// prepare sizes the correction-applied buffer to source-count × out-channels. The size matches what tick() needs on its first send. Calling tick() after prepare must not reallocate, pin the data pointer + shape.
TEST_CASE("NetworkSendDriver sizes corrected_ in prepare, not in tick") {
    mm::Buffer source;
    REQUIRE(source.allocate(64, 3));

    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);

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
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();

    REQUIRE(driver.correctedBuffer().channelsPerLight() == 3);

    // Simulate a preset change. Drivers normally drives this; we call directly. The driver owns its Correction, so mutate that copy (not the external one).
    mm::test::rebuildFromPreset(driver.correctionForTest(), 255, mm::test::PresetOrder::RGBW);
    driver.onCorrectionChanged();

    CHECK(driver.correctedBuffer().count() == 32);
    CHECK(driver.correctedBuffer().channelsPerLight() == 4);
}

// A brightness-only change keeps outChannels at 3, onCorrectionChanged is still called, but the resize short-circuits (existing buffer already fits).
TEST_CASE("NetworkSendDriver onCorrectionChanged is a no-op when outChannels unchanged") {
    mm::Buffer source;
    REQUIRE(source.allocate(48, 3));

    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);

    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();

    const uint8_t* dataBefore = driver.correctedBuffer().data();
    REQUIRE(dataBefore != nullptr);

    // Brightness change: outChannels stays 3, so the existing allocation fits. The driver owns its Correction, so mutate that copy (not the external one).
    mm::test::rebuildFromPreset(driver.correctionForTest(), 128, mm::test::PresetOrder::RGB);
    driver.onCorrectionChanged();

    // Same backing allocation, the resize short-circuited.
    CHECK(driver.correctedBuffer().data() == dataBefore);
    CHECK(driver.correctedBuffer().count() == 48);
    CHECK(driver.correctedBuffer().channelsPerLight() == 3);
}

// Every prepare restarted the send task, so a show re-preparing its layers kept an FPP from receiving a whole frame.
TEST_CASE("NetworkSendDriver keeps its send task running through a prepare that changed nothing it sends") {
    mm::Buffer source;
    REQUIRE(source.allocate(64, 3));
    mm::NetworkSendDriver driver;
    driver.setSourceBuffer(&source);
    std::snprintf(driver.hosts, sizeof(driver.hosts), "%s", "127.0.0.1");
    driver.packetGap = 500;   // paced from its own task
    driver.applyState();
    REQUIRE(driver.taskStartsForTest() == 1);
    driver.applyState();
    CHECK(driver.taskStartsForTest() == 1);   // the stream runs on
    std::snprintf(driver.hosts, sizeof(driver.hosts), "%s", "127.0.0.2");
    driver.applyState();
    CHECK(driver.taskStartsForTest() == 2);   // a new destination restarts it
    std::snprintf(driver.hosts, sizeof(driver.hosts), "%s", "fpp.local");   // a name, whose address the resolver fills in
    driver.applyState();
    REQUIRE(driver.taskStartsForTest() == 3);
    driver.applyState();
    CHECK(driver.taskStartsForTest() == 3);
    driver.release();
}
