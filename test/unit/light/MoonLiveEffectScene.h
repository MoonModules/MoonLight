#pragma once

/// A scripted effect on a real layer, the way the module tree wires one, for tests that run a script and read the lights it wrote.

#include "MoonLiveScriptFixture.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "core/moonlive/moonlive_emit.h"   // MM_MOONLIVE_HAS_HOST_JIT
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layer.h"

namespace mm {

/// A wired effect on a real layer, the way the module tree builds one.
struct ScriptScene {
    Layouts layouts;          ///< the layout container the layer reads
    GridLayout grid;          ///< a w x h x 1 grid
    Layer layer;              ///< the layer the effect draws on
    MoonLiveEffect effect;    ///< the scripted effect under test
    /// Wire a grid of `w` by `h` lights, the effect on its layer.
    ScriptScene(int w = 16, int h = 16) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&effect);
        effect.defineControls();
    }
    /// Write `script` to a file, point the effect at it, and compile it the way a prepare does.
    void run(const char* script) {
        effect.setScript(mmWriteScript(script));
        layouts.applyState();
        layer.applyState();
    }
    /// How many lights carry any color, which is the coverage a stroke width decides.
    int lit() {
        int n = 0;
        const auto* px = layer.buffer().data();
        for (nrOfLightsType i = 0; i < layer.buffer().count(); i++)
            if (px[i * 3] || px[i * 3 + 1] || px[i * 3 + 2]) n++;
        return n;
    }
};

}  // namespace mm
