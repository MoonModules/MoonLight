/// @module RegionModifier

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "light/modifiers/RegionModifier.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/drivers/Drivers.h"
#include "light/effects/EffectBase.h"

// A show moving a Region 50 times a second re-prepared the whole tree each time, 0.5 s a frame on an ESP32-S3.

namespace {

// Counts its prepares.
class CountingEffect : public mm::EffectBase {
public:
    void tick() MM_NONBLOCKING override {}
    void prepare() override { prepares++; }
    int prepares = 0;
};

// Counts the structural rebuilds the output peripheral would take.
class CountingDriver : public mm::DriverBase {
public:
    void setSourceBuffer(mm::Buffer*) override {}
    void tick() MM_NONBLOCKING override {}
    void defineDriverControls() override { defineCorrectionControls(); }
    void prepare() override { prepares++; }
    int prepares = 0;
};

// Two layers over one grid, a Region in the first, and a driver.
struct Rig {
    mm::Scheduler s;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Effects effects;
    mm::Layer layerA, layerB;
    mm::RegionModifier region;
    CountingEffect fxA, fxB;
    mm::Drivers drivers;
    CountingDriver drv;
    Rig() {
        grid.width = 16; grid.height = 16; grid.depth = 1;
        layouts.addChild(&grid);
        region.setName("Region");
        region.endX = 50;
        layerA.addChild(&region);
        layerA.addChild(&fxA);
        layerB.addChild(&fxB);
        effects.addChild(&layerA);
        effects.addChild(&layerB);
        effects.setLayouts(&layouts);
        drivers.setEffects(&effects);
        drivers.addChild(&drv);
        drivers.on = true;
        s.addModule(&layouts);
        s.addModule(&effects);
        s.addModule(&drivers);
        for (mm::MoonModule* m : {static_cast<mm::MoonModule*>(&layouts), static_cast<mm::MoonModule*>(&effects), static_cast<mm::MoonModule*>(&drivers)})
            m->defineControls();
        s.setup();
    }
    void zero() { fxA.prepares = fxB.prepares = drv.prepares = 0; }
};

}  // namespace

TEST_CASE("a Region change prepares its own layer, not the other layer or the drivers") {
    Rig r;
    r.zero();
    REQUIRE(r.s.setControl("Region", "endX", "{\"value\":60}") == mm::Scheduler::SetControlResult::Ok);
    r.s.tick();
    CHECK(r.fxA.prepares == 1);
    CHECK(r.fxB.prepares == 0);
    CHECK(r.drv.prepares == 0);   // two layers composite into Drivers' own buffer, whose shape a Region does not change
    CHECK(r.layerA.width() == 10);   // the change did apply: 60 percent of 16, the window's end ceiling to an exclusive one
}

TEST_CASE("after a layer's rebuild, Drivers prepares again only when the layer's shape changed") {
    Rig r;
    r.zero();
    r.s.prepareSubtree(&r.layerA);   // the shape it sized from is unchanged
    CHECK(r.drv.prepares == 0);
    r.layerA.setChannelsPerLight(4);   // as a white channel being added would
    r.s.prepareSubtree(&r.layerA);
    CHECK(r.drv.prepares == 1);
}
