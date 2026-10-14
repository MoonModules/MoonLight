/// @module FseqPlayerEffect
/// @also ChannelDataSink

/// The FSEQ player as it stands: its settings, its frames landing where a sender-mapped stream's do, and a status saying it reads no file yet.

#include "doctest.h"
#include "light/effects/FseqPlayerEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"

#include <string>

namespace {
struct FseqRig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::FseqPlayerEffect fx;
    FseqRig() {
        grid.width = 4;
        grid.height = 4;
        grid.depth = 1;
        grid.serpentine = true;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
        fx.defineControls();
        layer.applyState();
    }
};
}  // namespace

// An FSEQ frame is in wiring order, so alone on its layer it goes to the output as it is, on a serpentine grid too.
TEST_CASE("an FSEQ player alone on its layer hands its frame to the output") {
    FseqRig r;
    CHECK(r.fx.deliversWiringOrder());
    CHECK(r.layer.wiringSource() == &r.fx);
}

// Until playback lands the player says so, rather than showing black with no reason.
TEST_CASE("an FSEQ player says it reads no file yet") {
    FseqRig r;
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::string(r.fx.status()) == "reads no FSEQ file yet: playback is the next step");
}
