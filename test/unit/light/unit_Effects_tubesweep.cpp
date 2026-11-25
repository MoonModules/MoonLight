/// @module EffectBase

/// Every registered effect on a 1x60x10 tube rig: its output reaches past the first tube.

#include "effect_grid_sweep.h"
#include "platform/platform_clock.h"   // setTestNowMs

using namespace mm::sweep;

// A tube rig: 1 wide, 60 down y, 10 deep, ten tubes of sixty lights; the depth axis carries the fixtures, and Layer::tick extrudes a D1 or D2 effect across the remaining depth so its output reaches every tube.
TEST_CASE("an effect reaches past the first tube of a 1x60x10 rig") {
    constexpr mm::lengthType kW = 1, kH = 60, kD = 10;
    int audited = 0, painted = 0;

    mm::forEachEffect([&](const char* name, auto make) {
        const std::string effectName(name);
        // Same exemptions as the background audit: DemoReel delegates to a child absent here, and NetworkReceive blocks waiting for a packet.
        if (effectName == "DemoReelEffect" || effectName == "NetworkReceiveEffect") return;
        audited++;

        mm::Layouts layouts;
        auto* grid = new mm::GridLayout();
        grid->width = kW; grid->height = kH; grid->depth = kD;
        layouts.addChild(grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        mm::MoonModule* effect = make();
        effect->defineControls();
        layer.addChild(effect);
        layouts.applyState();
        layer.applyState();

        // Several effects seed on their first tick and draw from the second; a few are beat-driven, so the clock has to move for them to paint anything at all.
        for (int f = 1; f <= 8; f++) {
            mm::platform::setTestNowMs(static_cast<uint32_t>(f) * 40u);
            layer.tick();
        }

        const uint8_t* buf = layer.buffer().data();
        const auto lights = layer.buffer().count();
        REQUIRE(buf != nullptr);
        REQUIRE(lights == static_cast<mm::nrOfLightsType>(kW * kH * kD));

        // How many of the ten tubes have at least one lit light. An effect that paints only the first tube scores 1; one that fills the rig scores 10.
        int litTubes = 0;
        for (mm::lengthType z = 0; z < kD; z++) {
            bool lit = false;
            for (mm::lengthType y = 0; y < kH && !lit; y++) {
                const size_t i = (static_cast<size_t>(z) * kH + y) * 3;
                if (buf[i] || buf[i + 1] || buf[i + 2]) lit = true;
            }
            if (lit) litTubes++;
        }

        CAPTURE(effectName);
        CAPTURE(litTubes);
        // Reaching some tubes suffices: sparse effects (Random, StarSky, SphereMove) fill few in eight frames, and an effect confined to the first tube (ignoring z) leaves nine dark; input-driven effects with a dark frame are skipped.
        if (litTubes > 0) {
            CHECK_MESSAGE(litTubes > 1,
                          effectName << " lit only " << litTubes << " of " << kD
                                     << " tubes: it looks confined to the first slice");
            painted++;
        }
        delete effect;
    });

    CHECK_MESSAGE(audited > 0, "no effects audited: the test would pass without testing anything");
    MESSAGE("audited " << audited << " effects, " << painted << " painted the rig");
}
