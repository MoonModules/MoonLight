/// @module AudioService

/// The desktop OS-capture seam (platform_desktop_audio.cpp, miniaudio-backed): what shared code may rely on regardless of host. Capture HARDWARE is not assumed: on a locked-down or headless host, init failing cleanly is correct behavior (miniaudio's null backend makes success the CI norm, delivering silence), so the lifecycle cases assert no-crash coherence around whichever outcome the host gives. First local run on macOS triggers the OS microphone permission prompt for the invoking terminal, expected, once.

#include "doctest.h"
#include "core/services/AudioService.h"
#include "core/util/JsonSink.h"   // the device-Select persistence case writes a control value out
#include "platform/platform.h"

#include <cstring>
#include <string>

#if defined(MM_PLATFORM_DESKTOP) || !defined(ESP_PLATFORM)

// Device enumeration always offers at least "default", at entry 0, from stable platform storage.
TEST_CASE("audio capture enumeration offers 'default' first, from platform-owned storage") {
    const char* const* options = nullptr;
    const size_t n = mm::platform::audioCaptureDevices(&options);
    REQUIRE(n >= 1);
    REQUIRE(options != nullptr);
    CHECK(std::strcmp(options[0], "default") == 0);
    // A second enumeration returns the same stable pointer array (the Select borrows it).
    const char* const* again = nullptr;
    mm::platform::audioCaptureDevices(&again);
    CHECK(again == options);
}

// The capture device someone picks survives the device list changing under it. A loopback like BlackHole is deliberate routing, set up once, and the OS list it lives in is LIVE: unplug a webcam or let a Continuity Camera drop off and everything below it shifts up a slot, so a config that stored the INDEX silently starts naming a different device. Reported from the desktop bench 2026-08-29 ("I entered blackhole 2ch a few times but later I saw it was changed").
TEST_CASE("a picked audio device is remembered by name, not by its position in the list") {
    // The list as it was when the user picked: BlackHole sits at index 3.
    static constexpr const char* kBefore[] = {"default", "HD Pro Webcam C920", "NDI Audio",
                                              "BlackHole 2ch", "MacBook Air Microphone"};
    uint8_t device = 3;
    mm::ControlList controls;
    controls.addSelect("device", device, kBefore, 5);
    controls.setPersistLabel(controls.count() - 1);

    mm::JsonSink sink;
    sink.append("{\"device\":");
    mm::writeControlValue(sink, controls[0]);
    sink.append("}");
    const std::string saved(sink.data(), sink.size());
    CHECK(saved.find("\"BlackHole 2ch\"") != std::string::npos);   // the name, not "3"

    // The webcam is unplugged: BlackHole is now index 2, and index 3 would be the built-in mic.
    static constexpr const char* kAfter[] = {"default", "NDI Audio", "BlackHole 2ch",
                                             "MacBook Air Microphone"};
    uint8_t reloaded = 0;
    mm::ControlList after;
    after.addSelect("device", reloaded, kAfter, 4);
    after.setPersistLabel(after.count() - 1);
    CHECK(mm::applyControlValue(after[0], saved.c_str(), "device",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(reloaded == 2);                                  // followed BlackHole to its new slot
    CHECK(std::strcmp(kAfter[reloaded], "BlackHole 2ch") == 0);

    // And when the device is genuinely gone (BlackHole uninstalled), it falls back rather than silently grabbing whatever now sits at that index.
    static constexpr const char* kGone[] = {"default", "NDI Audio", "MacBook Air Microphone"};
    // Start at a NON-default row, so "unchanged" is distinguishable from "reset to 0": with `missing` at 0 this case would pass even if apply did nothing at all.
    uint8_t missing = 2;
    mm::ControlList gone;
    gone.addSelect("device", missing, kGone, 3);
    gone.setPersistLabel(gone.count() - 1);
    // A label naming no current option LEAVES THE CONTROL ALONE (Control.cpp: "the driver keeps its default"), which is the contract that matters here: the saved pick must never be silently re-pointed at whatever now occupies its old index. Not an error under Clamp.
    CHECK(mm::applyControlValue(gone[0], saved.c_str(), "device",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(missing == 2);                                        // untouched, not re-pointed
    CHECK(std::strcmp(kGone[missing], "BlackHole 2ch") != 0);   // and certainly not the gone device
}

// The full lifecycle neither crashes nor wedges, whatever the host's audio situation: a successful init delivers a readable (possibly silent) stream; a failed init degrades to false with reads returning nothing.
TEST_CASE("audio capture lifecycle is clean on any host: init, read, deinit, repeat") {
    mm::platform::AudioMicHandle h;
    const bool up = mm::platform::audioCaptureInit(h, 0, 22050);
    int32_t buf[256];
    const size_t got = mm::platform::audioMicRead(h, buf, 256);
    if (!up) CHECK(got == 0);          // failed init must not fabricate samples
    mm::platform::audioMicDeinit(h);
    CHECK(h.impl == nullptr);
    mm::platform::audioMicDeinit(h);   // double deinit is safe
    // The cycle repeats cleanly (the reinit path AudioService uses on every device change).
    const bool up2 = mm::platform::audioCaptureInit(h, 0, 22050);
    CHECK(up2 == up);                  // same host, same answer
    mm::platform::audioMicDeinit(h);
}

// An index past the device list fails loudly rather than opening some other device (the stale-persisted-index case: OS device order changed since the pick).
TEST_CASE("audio capture refuses an out-of-range device index") {
    mm::platform::AudioMicHandle h;
    CHECK_FALSE(mm::platform::audioCaptureInit(h, 200, 22050));
    mm::platform::audioMicDeinit(h);
}

// A capture device delivering silence must not raise the I2S wire diagnosis: on desktop there are no pins to check, and silence is a quiet room or an idle loopback (the flagship BlackHole case between songs). The verdict is compile-time-gated to hasI2sMic.
TEST_CASE("a silent capture device raises no I2S wire status") {
    static_assert(!mm::platform::hasI2sMic,
                  "this desktop-only case pins the desktop side of the gate");
    mm::AudioService a;
    a.mode = mm::AudioService::kLocalMode;
    a.applyState();
    for (int i = 0; i < 3; i++) { a.tick(); a.tick1s(); }   // three diagnosis windows
    const char* st = a.status();
    if (st != nullptr) {
        CHECK(std::strstr(st, "sdPin") == nullptr);
        CHECK(std::strstr(st, "sckPin") == nullptr);
    }
    a.release();
}

#endif
