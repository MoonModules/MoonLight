/// @module FreqSawsEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/FreqSawsEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"

// Helper: any non-zero byte in the layer buffer (a lit pixel somewhere).
static bool anyLit(mm::Layer& layer) {
    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.bytes(); i++)
        if (buf.data()[i] != 0) return true;
    return false;
}

// With no audio (silence) and keepOn off, every band decays to rest so the effect draws nothing, the whole buffer stays black. (No mic is active here, so latestFrame() is the static silence.)
TEST_CASE("FreqSawsEffect silence with keepOn off leaves the buffer dark") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FreqSawsEffect saws;
    saws.keepOn = false;          // a decayed band draws nothing
    layer.addChild(&saws);

    layer.applyState();
    layer.tick();

    REQUIRE(layer.buffer().data() != nullptr);
    CHECK_FALSE(anyLit(layer));   // silence in, dark out
}

// keepOn keeps every band drawing even when its speed has fully decayed, so on a rested (silent) panel the columns are still lit rather than fully dark between hits.
TEST_CASE("FreqSawsEffect keepOn draws bands even with no audio") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::Palettes::setActive(0);   // colorful palette so a drawn pixel is non-black (order-independent)

    mm::FreqSawsEffect saws;
    saws.keepOn = true;           // draw a band whose speed has decayed to zero
    saws.fade = 0;                // no fade so a just-drawn pixel survives the frame
    layer.addChild(&saws);

    layer.applyState();
    layer.tick();

    CHECK(anyLit(layer));         // keepOn lights the panel with no audio at all
}

// Fed a live (simulated) audio frame, the effect reacts: loud bands rise and paint their columns, leaving the buffer non-black even with keepOn off (so the light comes from the audio, not keepOn).
TEST_CASE("FreqSawsEffect reacts to a fed audio frame") {
    // Claim the active-mic seat with a module synthesizing loud "music" every tick (desktop has no I2S mic, so simulate is the only source; music-always fills every band). latestFrame() then hands this module's frame to the effect.
    mm::AudioService mic;
    mic.defineControls();
    mic.mode = mm::AudioService::kSimMode;   // simulate mode
    mic.simulate = mm::AudioService::kSimMusic;   // music pattern
    mic.setup();                  // registers as the active mic

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16; grid.height = 16; grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::Palettes::setActive(0);

    mm::FreqSawsEffect saws;
    saws.keepOn = false;          // any light MUST come from the audio, not keepOn
    saws.fade = 0;
    layer.addChild(&saws);
    layer.applyState();

    // A few ticks: each refreshes the synthesized loud frame, then the effect renders it. Loud bands rise instantly (max with target), so a lit column appears within these frames.
    bool lit = false;
    for (int tick = 0; tick < 8 && !lit; tick++) {
        mic.tick();               // (re)synthesize a loud music frame into the active mic
        layer.tick();             // effect reads latestFrame() and draws the active bands
        lit = anyLit(layer);
    }
    CHECK(lit);

    mic.release();               // vacate the seat so later tests read silence again
}

// The "runs at every grid size" hard rule: a degenerate 0×0×0 grid and a 1×1 grid both render without crashing (the imap zero-span guard and the sizeX/sizeY<=0 early-out cover them).
TEST_CASE("FreqSawsEffect runs at degenerate grid sizes without crashing") {
    for (auto dims : {mm::Coord3D{0, 0, 0}, mm::Coord3D{1, 1, 1}}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = dims.x; grid.height = dims.y; grid.depth = dims.z;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::FreqSawsEffect saws;
        saws.keepOn = true;       // exercise the draw path too, not just the early-out
        layer.addChild(&saws);

        layer.applyState();
        layer.tick();
        CHECK(true);              // reaching here without a crash is the assertion
    }
}
