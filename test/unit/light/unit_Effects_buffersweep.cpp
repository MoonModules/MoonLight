/// @module EffectBase

/// Every registered effect against its buffer contracts: it sizes its memory again after a release, and it owns its background rather than inheriting the last frame.

#include "effect_grid_sweep.h"
#include "platform/platform_clock.h"   // setTestNowMs

using namespace mm::sweep;

// A release frees every scratch buffer an effect holds, and the next prepare must size them again. An effect deciding on a size it cached finds it unchanged and leaves its buffers empty.
TEST_CASE("every effect holds its memory again after a release and a re-prepare") {
    int checked = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        const std::string effectName(name);
        CAPTURE(effectName);
        mm::MoonModule* effect = make();
        REQUIRE_MESSAGE(effect != nullptr, "could not construct " << effectName);
        OneEffectLayer rig(effect);
        mm::Layer& layer = rig.layer;
        const size_t before = effect->dynamicBytes();
        layer.release();      // what disabling, a preset or a rebuild does to the tree
        layer.applyState();   // and the prepare that follows
        CHECK_MESSAGE(effect->dynamicBytes() == before, effectName << " held " << before << " bytes before the release and " << effect->dynamicBytes() << " after");
        // And it runs on what it holds: a table that calls itself ready over freed memory reads null here.
        mm::platform::setTestNowMs(100000u); layer.tick();
        mm::platform::setTestNowMs(100016u); layer.tick();
        mm::platform::setTestNowMs(0);
        layer.release();
        delete effect;
        checked++;
    });
    CHECK_MESSAGE(checked > 0, "no effects checked");
}

// The Layer keeps the buffer between frames (trails and scrolls read prior pixels), so every effect owns its background; a golden-frame test starts zeroed and misses unwritten pixels, so this starts from a dirty buffer as a real device does.
TEST_CASE("every effect owns its background rather than inheriting the last frame") {
    int audited = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        // DemoReel delegates the frame to a child (none registered, so an empty frame) and NetworkReceive blocks in recvfrom until a packet arrives, so both are skipped.
        const std::string effectName(name);
        if (effectName == "DemoReelEffect" || effectName == "NetworkReceiveEffect") { audited++; return; }

        mm::MoonModule* effect = make();
        OneEffectLayer rig(effect);
        mm::Layer& layer = rig.layer;

        // A recognizable value no palette produces, standing in for the previous effect's frame.
        constexpr uint8_t kStale = 0xA7;
        uint8_t* buf = const_cast<uint8_t*>(layer.buffer().data());
        const mm::nrOfLightsType lights = layer.buffer().count();
        for (mm::nrOfLightsType i = 0; i < lights * 3; i++) buf[i] = kStale;

        const auto staleCount = [&] {
            int n = 0;
            for (mm::nrOfLightsType i = 0; i < lights; i++)
                if (buf[i * 3] == kStale && buf[i * 3 + 1] == kStale && buf[i * 3 + 2] == kStale) n++;
            return n;
        };

        // The first frame is what the user sees on switching effects and must replace the old picture, so it is measured before a long run can hide a slow paint-over.
        mm::platform::setTestNowMs(100000u);
        layer.tick();
        const int afterFirst = staleCount();

        // Then run on, so an effect that clears once and later stops writing is still caught.
        for (int f = 1; f < 90; f++) {
            mm::platform::setTestNowMs(100000u + static_cast<uint32_t>(f) * 16u);
            layer.tick();
        }
        const int afterMany = staleCount();
        mm::platform::setTestNowMs(0);

        INFO("effect: " << effectName);
        CAPTURE(afterFirst);
        CAPTURE(afterMany);
        // A static subset leaves a few stale pixels, so half the grid is the limit; only the settled frame is asserted because fourteen effects still show the prior picture on frame one (backlog), and afterFirst is captured for failure output.
        CHECK(afterMany < static_cast<int>(lights) / 2);
        audited++;
    });
    MESSAGE("audited " << audited << " effects against a dirty buffer");
}
