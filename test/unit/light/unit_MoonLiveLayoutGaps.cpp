/// @module MoonLiveLayout
/// @also LayoutBase, Layer

/// A scripted layout's gaps: `addGap` places a dark slot the driver clocks in wiring order, as a light behind a round frame stays dark under every layer.

#include "doctest.h"
#include "MoonLiveScriptFixture.h"
#include "light/moonlive/MoonLiveLayout.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"

#include <cstring>
#include <set>
#include <vector>

using namespace mm;

// Every case compiles a script and runs its native code, so it needs a JIT backend for the host ISA, as unit_MoonLiveLayout does.
#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
struct GapEntry { nrOfLightsType idx; lengthType x, y; bool gap; };

const char* const kCornerGaps =
    "class CornerGapsLayout {\n"
    "  byte gaps = 1;\n"
    "  void defineControls() { addControl(\"gaps\", gaps, 0, 1); }\n"
    "  void placeLights() {\n"
    "    for (int y = 0; y < 4; y = y + 1) {\n"
    "      for (int x = 0; x < 4; x = x + 1) {\n"
    "        int edgeX = 0; int edgeY = 0;\n"
    "        if (x == 0) { edgeX = 1; } if (x == 3) { edgeX = 1; }\n"
    "        if (y == 0) { edgeY = 1; } if (y == 3) { edgeY = 1; }\n"
    "        if (gaps == 1 && edgeX == 1 && edgeY == 1) { addGap(x, y, 0); } else { addLight(x, y, 0); }\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

bool isCorner(nrOfLightsType idx) { return idx == 0 || idx == 3 || idx == 12 || idx == 15; }
}  // namespace

TEST_CASE("addGap places a slot the driver clocks and nothing lights") {
    MoonLiveLayout l;
    l.defineControls();
    l.setScript(mmWriteScript(kCornerGaps));
    l.prepare();

    CHECK(l.lightCount() == 16);   // a gap is a wire slot, so it counts
    CHECK(l.hasGaps());

    std::vector<GapEntry> v;
    CoordSink sink{
        [](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType) {
            static_cast<std::vector<GapEntry>*>(c)->push_back({i, x, y, false});
        },
        [](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType) {
            static_cast<std::vector<GapEntry>*>(c)->push_back({i, x, y, true});
        },
        &v};
    l.placeLights(sink);
    REQUIRE(v.size() == 16);
    for (nrOfLightsType i = 0; i < 16; i++) {
        INFO("slot " << (int)i);
        CHECK(v[i].idx == i);     // indices run on through the gaps, in call order
        CHECK(v[i].x == i % 4);   // every slot keeps its true position
        CHECK(v[i].y == i / 4);
        CHECK(v[i].gap == isCorner(i));
    }
}

TEST_CASE("a scripted layout with gaps maps no light to them, and switching gaps off and on never leaves a stale map") {
    MoonLiveLayout layout;
    layout.defineControls();
    layout.setScript(mmWriteScript(kCornerGaps));
    layout.prepare();
    layout.rebuildControls();   // the script's `gaps` control exists once it has compiled

    Layouts group;
    group.addChild(&layout);
    Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    layer.defineControls();

    auto setGaps = [&](uint8_t v) {
        const auto& cs = layout.controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (cs[i].name && std::strcmp(cs[i].name, "gaps") == 0) *static_cast<uint8_t*>(cs[i].ptr) = v;
    };
    // The physical slots any light reaches: a gap must be reached by none.
    auto litSlots = [&]() {
        std::set<nrOfLightsType> lit;
        const MappingLUT& lut = layer.lut();
        for (nrOfLightsType li = 0; li < lut.logicalCount(); li++)
            lut.forEachDestination(li, [&](nrOfLightsType d) { lit.insert(d); });
        return lit;
    };

    // Off, on, off, on: the flag the Layer reads comes from the count it took last.
    for (uint8_t gaps : {0, 1, 0, 1}) {
        INFO("gaps " << (int)gaps);
        setGaps(gaps);
        group.applyState();
        layer.applyState();
        CHECK(layer.physicalLightCount() == 16);
        CHECK(layout.hasGaps() == (gaps == 1));
        if (gaps == 0) {
            CHECK_FALSE(layer.lut().hasLUT());   // no gap: the dense identity fast path
        } else {
            REQUIRE(layer.lut().hasLUT());   // a gap needs the folded map, or identity would light it
            const auto lit = litSlots();
            CHECK(lit.size() == 12);
            for (nrOfLightsType c : {0, 3, 12, 15}) CHECK(lit.count(c) == 0);
        }
    }
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
