/// @module NoiseMeterEffect
/// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/NoiseMeterEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/services/AudioService.h"

// NoiseMeter is an audio-reactive 1D effect: a vertical VU column whose height tracks the overall sound level and whose color is a scrolling 2D noise field. It writes only the x=0 column and Layer::extrude fans each lit row across every x (and z), so a lit row is a complete horizontal band. The column fills bottom-up: row y=0 lights first (the floor is buffer row height-1, since drawY = sizeY-1-y). The frame comes from AudioService::latestFrame() (a process-wide static); on the host with no I2S mic we run a live AudioService with `simulate` set to an "always" mode so synthesizeFrame() fills the level each tick(). Every audio case brackets its own AudioService setup()/release() so it never leaks the active-mic pointer into another test file. Buffer index = (y*width + x)*3.

// Helper: is any byte of the pixel at (x,y) on a width-W grid non-zero.
static bool pixelLit(mm::Layer& layer, int x, int y, int W) {
    auto* d = layer.buffer().data();
    size_t idx = (static_cast<size_t>(y) * W + x) * 3;
    return d[idx] || d[idx + 1] || d[idx + 2];
}

// With no live audio source the level is 0, so no row lights and the fading buffer settles to black.
TEST_CASE("NoiseMeterEffect fades to dark without an audio frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::NoiseMeterEffect meter;
    layer.addChild(&meter);

    layer.applyState();
    // No AudioService is active → latestFrame() is the static all-silence frame (level 0). Each loop fades then reads silence → maxLen 0 → nothing drawn, so the buffer decays to fully dark.
    for (int i = 0; i < 16; i++) layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 64);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// Fed a loud audio frame the meter fills from the floor upward: a lit row above the floor implies every row below it (down to the floor) is also lit, the column never floats. Extrude also means a lit row is complete across x, so column 0 and the last column of a lit row agree.
TEST_CASE("NoiseMeterEffect fills the column from the floor upward") {
    mm::AudioService audio;
    audio.defineControls();
    audio.mode = mm::AudioService::kSimMode;   // simulate mode
    audio.simulate = mm::AudioService::kSimMusic;   // music pattern
    audio.setup();

    const int W = 8, H = 8;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = W;
    grid.height = H;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::Palettes::setActive(0);   // colorful palette so a drawn pixel is non-black (order-independent)

    mm::NoiseMeterEffect meter;
    meter.fadeRate = 254;   // fastest fade so a lit row this frame is this frame's fill, not a stale trail
    layer.addChild(&meter);

    layer.applyState();

    // Run frames until the meter rises, then assert the bottom-up invariant on the tallest lit column.
    bool sawFill = false;
    for (int i = 0; i < 64; i++) {
        audio.tick();
        layer.tick();
        // Floor is buffer row H-1 (drawY = sizeY-1-y, y=0 draws there first).
        if (pixelLit(layer, 0, H - 1, W)) {
            sawFill = true;
            // Walk up from the floor: once a dark row is found, every row above it must also be dark (a contiguous fill from the bottom, no floating segment).
            bool darkSeen = false;
            for (int y = H - 1; y >= 0; y--) {
                bool lit = pixelLit(layer, 0, y, W);
                if (!lit) darkSeen = true;
                if (lit) CHECK_FALSE(darkSeen);
                // Extrude fans the x=0 column across x: a lit row is lit at the far column too.
                CHECK(pixelLit(layer, W - 1, y, W) == lit);
            }
        }
    }
    CHECK(sawFill);   // the meter rose off the floor during the run

    audio.release();
}

// The `width` gain control scales level→length: width=0 zeroes the length (tmpSound2 = level*2*0/255), so even a loud audio frame lights no row and the buffer stays dark.
TEST_CASE("NoiseMeterEffect width 0 keeps the meter dark despite loud audio") {
    mm::AudioService audio;
    audio.defineControls();
    audio.mode = mm::AudioService::kSimMode;   // simulate mode
    audio.simulate = mm::AudioService::kSimMusic;   // music pattern
    audio.setup();

    const int W = 8, H = 8;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = W;
    grid.height = H;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    mm::Palettes::setActive(0);

    mm::NoiseMeterEffect meter;
    meter.width = 0;   // gain 0 → maxLen 0 → no row drawn, regardless of level
    layer.addChild(&meter);

    layer.applyState();
    for (int i = 0; i < 16; i++) { audio.tick(); layer.tick(); }

    bool anyLit = false;
    for (size_t i = 0; i < layer.buffer().bytes(); i++) {
        if (layer.buffer().data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);

    audio.release();
}

// The "runs at every grid size" hard rule: 0×0×0 and 1×1 both render with a live audio frame every tick without crashing (the sizeX/sizeY<=0 early-out and the maxLen constrain cover them).
TEST_CASE("NoiseMeterEffect survives degenerate grid sizes") {
    mm::AudioService audio;
    audio.defineControls();
    audio.mode = mm::AudioService::kSimMode;   // simulate mode
    audio.simulate = mm::AudioService::kSimMusic;   // music pattern
    audio.setup();

    for (auto dims : {mm::Coord3D{0, 0, 0}, mm::Coord3D{1, 1, 1}}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = dims.x;
        grid.height = dims.y;
        grid.depth = dims.z;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::NoiseMeterEffect meter;
        layer.addChild(&meter);

        layer.applyState();
        for (int i = 0; i < 4; i++) { audio.tick(); layer.tick(); }
    }
    CHECK(true);   // no crash at 0×0×0 or 1×1

    audio.release();
}
