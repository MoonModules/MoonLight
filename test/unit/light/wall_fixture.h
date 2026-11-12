/// The wall a driver test drives, as production wires it: a Layout gives the Layer its physical size and the driver reads that, having no geometry of its own.
#pragma once
#include "doctest.h"
#include "correction_presets.h"
#include "light/drivers/Correction.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/PanelsLayout.h"
#include "platform/platform.h"

#include <memory>

namespace mm::test {

/// A wall of `width` x `height`.
struct Wall {
    mm::Layouts layouts;    ///< the layouts the layer reads its size from
    mm::GridLayout grid;    ///< the one layout, a grid of the wall's size
    mm::Layer layer;        ///< the layer the driver is given
    /// Build the grid and apply it, so the layer carries the wall's size.
    Wall(uint16_t width, uint16_t height) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

/// A layer over any layout, built once the layout's fields are set, so a case can shape the layout, or refuse memory, before a driver first prepares.
template <class LayoutT>
struct LayoutFace {
    LayoutT layout;          ///< the one layout, shaped by the case
    mm::Layouts layouts;     ///< the layouts the layer reads its size from
    mm::Layer layer;         ///< the layer a driver is given
    /// Apply the layout, so the layer carries its size.
    void build() {
        layouts.addChild(&layout);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

/// Shape `p` as `across` panels of `w` by `h` in a row, snaked inside each when `snake`.
inline void shapePanels(mm::PanelsLayout& p, uint16_t across, uint16_t w, uint16_t h, bool snake) {
    p.horizontalPanels = across;
    p.verticalPanels = 1;
    p.panelWidth = w;
    p.panelHeight = h;
    p.snake = snake;
}

/// Those panels as a built face; on the heap, since a layer outgrows a test's stack frame.
inline std::unique_ptr<LayoutFace<mm::PanelsLayout>> panelsFace(uint16_t across, uint16_t w, uint16_t h, bool snake) {
    auto face = std::make_unique<LayoutFace<mm::PanelsLayout>>();
    shapePanels(face->layout, across, w, h, snake);
    face->build();
    return face;
}

/// A 4x2 raster after one light at (-1, 1), left of the box, as a script's out-of-range coordinate arrives once cast to a signed position.
struct LeftOfBox : mm::LayoutBase {
    /// The raster's 8 lights and the one outside it.
    mm::nrOfLightsType lightCount() const override { return 9; }
    /// Light 0 left of the box, then the raster.
    void placeLights(const mm::CoordSink& sink) const override {
        sink.pixel(0, -1, 1, 0);
        for (mm::nrOfLightsType i = 1; i < 9; i++)
            sink.pixel(i, static_cast<mm::lengthType>((i - 1) % 4), static_cast<mm::lengthType>((i - 1) / 4), 0);
    }
};

/// A dense 4x2 box of 8 lights in raster order, except light 3 at (-1, 1), left of the box: its row-major index would wrap to cell 3, so only a check per axis sees it is outside.
struct OneOutsideDense : mm::LayoutBase {
    /// Eight lights, as many as the box has cells.
    mm::nrOfLightsType lightCount() const override { return 8; }
    /// The raster, with light 3 moved left of the box.
    void placeLights(const mm::CoordSink& sink) const override {
        for (mm::nrOfLightsType i = 0; i < 8; i++) {
            if (i == 3) sink.pixel(3, -1, 1, 0);
            else sink.pixel(i, static_cast<mm::lengthType>(i % 4), static_cast<mm::lengthType>(i / 4), 0);
        }
    }
};

/// Every allocation above `bytes` is refused while this lives, so a test meets a full heap on a buffer of its choosing.
struct RefuseAbove {
    /// Refuse every allocation above `bytes` from here on.
    explicit RefuseAbove(size_t bytes) { mm::platform::setTestAllocRefusedFrom(bytes + 1); }
    /// Allow every allocation again, however the test ends.
    ~RefuseAbove() { mm::platform::setTestAllocRefusedFrom(0); }
};

/// Wire `driver` onto `layer` over a `count`-light source with a plain RGB correction, ready to tick.
template <class Driver>
void wire(Driver& driver, mm::Buffer& source, mm::Layer& layer, mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    auto correction = std::make_unique<mm::Correction>();   // on the heap: its tables outgrow a stack frame
    rebuildFromPreset(*correction, 255, PresetOrder::RGB);
    driver.setLayer(&layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = *correction;
    driver.applyState();
}

/// Wire `driver` onto `wall`, as onto its layer.
template <class Driver>
void wire(Driver& driver, mm::Buffer& source, Wall& wall, mm::nrOfLightsType count) {
    wire(driver, source, wall.layer, count);
}

/// Light `i` of `source` painted (r, g, b).
inline void paintLight(mm::Buffer& source, mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t* p = source.data() + static_cast<size_t>(i) * 3;
    p[0] = r; p[1] = g; p[2] = b;
}

/// Light `light` painted (10, 20, 30) and one frame run: the frame `sent` reports, or null when none went out.
template <class Driver, class Sent>
const uint8_t* frameAfterPainting(Driver& driver, mm::Buffer& source, mm::nrOfLightsType light, Sent sent) {
    paintLight(source, light, 10, 20, 30);
    mm::platform::setTestNowMs(1000);
    driver.tick();
    return sent();
}

/// A 4x2 snaked panel's frame with light 4 painted (10, 20, 30): the light at the second row's right end, pixel 7, and pixel 4 dark.
inline void checkSnakedFrame(const uint8_t* f) {
    REQUIRE(f != nullptr);
    CHECK(f[7 * 3] == 10);
    CHECK(f[7 * 3 + 1] == 20);
    CHECK(f[7 * 3 + 2] == 30);
    CHECK(f[4 * 3] == 0);
}

/// A driver wired onto `across` panels of `w` by `h` in a row, snaked inside each when `snake`; each part on the heap, the face first so it outlives the driver.
template <class Driver>
struct DriverOnPanels {
    std::unique_ptr<LayoutFace<mm::PanelsLayout>> face;              ///< the layout and its layer
    std::unique_ptr<mm::Buffer> source = std::make_unique<mm::Buffer>();   ///< the lights the driver reads
    std::unique_ptr<Driver> driver = std::make_unique<Driver>();    ///< the driver under test
    /// Build the face and wire the driver onto it.
    DriverOnPanels(uint16_t across, uint16_t w, uint16_t h, bool snake) : face(panelsFace(across, w, h, snake)) {
        wire(*driver, *source, face->layer, face->layout.lightCount());
    }
};

}  // namespace mm::test
