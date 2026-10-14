/// @module PictureEffect
/// @also PictureService

/// The picture effect: each cell reads its part of the active picture, point or area, fitted or stretched, on a plane or a cube.

#include "doctest.h"
#include "core/services/PictureService.h"
#include "light/effects/PictureEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"

#include <cstring>
#include <string>

namespace {
// Eight bars across the top two thirds (white, yellow, cyan, green, magenta, red, blue, black) over a black-to-white ramp, written as a source writes.
void writeBars(mm::PictureService& pic) {
    static constexpr uint8_t kBars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                             {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0}};
    const uint32_t w = pic.width, h = pic.height, barsBottom = (h * 2 + 2) / 3;
    uint8_t* rgb = pic.beginWrite();
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = rgb + (y * w + x) * 3;
            if (y < barsBottom) std::memcpy(p, kBars[x * 8 / w], 3);
            else p[0] = p[1] = p[2] = static_cast<uint8_t>(w > 1 ? x * 255 / (w - 1) : 255);
        }
    pic.endWrite();
}

// A plain grid with the effect alone on its layer, and `pw` x `ph` bars as the active picture.
struct PictureRig {
    mm::PictureService pic;
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::PictureEffect fx;

    PictureRig(uint16_t pw, uint16_t ph, mm::lengthType w, mm::lengthType h, mm::lengthType d = 1,
               uint8_t fit = mm::PictureEffect::kStretch, uint8_t sampling = mm::PictureEffect::kPoint) {
        pic.defineControls();
        pic.width = pw;
        pic.height = ph;
        pic.prepare();
        writeBars(pic);
        grid.width = w;
        grid.height = h;
        grid.depth = d;
        grid.serpentine = false;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        fx.fit = fit;
        fx.sampling = sampling;
        layer.addChild(&fx);
        fx.defineControls();
        layer.applyState();
    }
    // The cell at (x, y, z) as red, green, blue.
    const uint8_t* cell(size_t x, size_t y, size_t z = 0) const {
        const size_t w = layer.width(), h = layer.height();
        return layer.buffer().data() + ((z * h + y) * w + x) * 3;
    }
};
}  // namespace

// On a grid the picture's size, each cell is its own pixel: the bars on top, the ramp at the bottom.
TEST_CASE("PictureEffect shows a picture the grid's size pixel for pixel") {
    PictureRig r(8, 3, 8, 3);
    r.layer.tick();
    CHECK(r.cell(0, 0)[2] == 255);   // white bar
    CHECK(r.cell(1, 0)[2] == 0);     // yellow bar: no blue
    CHECK(r.cell(7, 0)[0] == 0);     // black bar
    CHECK(r.cell(0, 2)[0] == 0);     // the ramp starts black
    CHECK(r.cell(7, 2)[0] == 255);   // and ends white
}

// Two picture columns per cell: point takes one, area averages both in linear light, so white and yellow give a blue of 180, not the 127 a plain mean would.
TEST_CASE("PictureEffect area sampling averages in linear light where point sampling picks one pixel") {
    PictureRig point(8, 3, 4, 3, 1, mm::PictureEffect::kStretch, mm::PictureEffect::kPoint);
    point.layer.tick();
    CHECK(point.cell(0, 0)[2] == 255);   // the white pixel alone
    PictureRig area(8, 3, 4, 3, 1, mm::PictureEffect::kStretch, mm::PictureEffect::kArea);
    area.layer.tick();
    CHECK(area.cell(0, 0)[0] == 255);
    CHECK(area.cell(0, 0)[2] == 180);    // sqrt((255² + 0²) / 2)
}

// A wide picture on a square grid: fit keeps its aspect with black rows above and below, fill keeps it by cropping the sides, stretch fills every row.
TEST_CASE("PictureEffect fits a wide picture with black bands, fills it cropped, and stretches it") {
    PictureRig fit(8, 3, 4, 4, 1, mm::PictureEffect::kFit);
    fit.layer.tick();
    CHECK(fit.cell(0, 0)[0] == 0);       // above the picture
    CHECK(fit.cell(0, 3)[0] == 0);       // below it
    CHECK(fit.cell(0, 1)[0] == 255);     // the white bar, inside
    PictureRig stretch(8, 3, 4, 4, 1, mm::PictureEffect::kStretch);
    stretch.layer.tick();
    CHECK(stretch.cell(0, 0)[0] == 255); // the top row is the picture's
    CHECK(stretch.cell(0, 0)[2] == 255); // the white bar at the left edge
    PictureRig fill(8, 3, 4, 4, 1, mm::PictureEffect::kFill);
    fill.layer.tick();
    // 8 x 3 at the grid's height spans 11 columns, 3 cropped each side: the top-left cell is the cyan bar, picture column 2.
    CHECK(fill.cell(0, 0)[1] == 255);    // no band: the top row shows the picture
    CHECK(fill.cell(0, 0)[0] == 0);      // cropped past the white and yellow bars
}

// A cube shows the picture on every slice, the layer extruding the plane through the depth.
TEST_CASE("PictureEffect on a 3D layout shows the picture on every slice") {
    PictureRig r(8, 3, 8, 3, 2);
    r.layer.tick();
    for (size_t x = 0; x < 8; x++) CHECK(r.cell(x, 0, 1)[0] == r.cell(x, 0, 0)[0]);
}

// A picture already on the layer costs nothing again: a later writer's bytes survive until the picture changes.
TEST_CASE("PictureEffect does not sample again without a new picture") {
    PictureRig r(8, 3, 8, 3);
    r.layer.tick();
    uint8_t* buf = r.layer.buffer().data();
    buf[0] = 7;                          // stand in for a later writer
    r.layer.tick();
    CHECK(buf[0] == 7);
    writeBars(r.pic);                    // the same picture written again: a new generation
    r.layer.tick();
    CHECK(buf[0] == 255);
}

// Without a Picture service the effect says what to add and shows black rather than its last picture.
TEST_CASE("PictureEffect without a Picture service says so and shows black") {
    PictureRig r(8, 3, 8, 3);
    r.layer.tick();
    REQUIRE(r.cell(0, 0)[0] == 255);   // the white bar, while the service publishes
    r.pic.release();
    r.layer.tick();
    REQUIRE(r.fx.status() != nullptr);
    CHECK(std::string(r.fx.status()) == "no picture: add a Picture service under Services");
    CHECK(r.cell(0, 0)[0] == 0);
}
