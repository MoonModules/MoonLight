/// @module LayoutPicture
/// @also HdmiDriver, NdiDriver, HlsDriver, RtspDriver

/// The layout as a picture: which light each pixel of the physical front face shows, the one placement every screen and stream driver draws through.

#include "doctest.h"
#include "light/drivers/LayoutPicture.h"
#include "light/layouts/GridBlacksLayout.h"
#include "wall_fixture.h"

#include <memory>
#include <string_view>
#include <vector>

using mm::test::LayoutFace;
using mm::test::panelsFace;

namespace {

// The module whose memory the picture's table counts against.
struct Owner : mm::MoonModule {};

// The first 5 lights of a 4x2 raster then one at the far corner, so the box is 4x2: in raster order when `inOrder`, else lights 4 and 5 swapped across row 1.
struct ShortRaster : mm::LayoutBase {
    bool inOrder = true;
    mm::nrOfLightsType lightCount() const override { return 6; }
    void placeLights(const mm::CoordSink& sink) const override {
        for (mm::nrOfLightsType i = 0; i < 4; i++) sink.pixel(i, static_cast<mm::lengthType>(i), 0, 0);
        sink.pixel(4, inOrder ? 0 : 3, 1, 0);
        sink.pixel(5, inOrder ? 1 : 0, 1, 0);
    }
};

// A 4x1 GridBlacks with its first column dark, then a 4x1 Grid over the same row: lights 0 to 3, light 0 a gap, under lights 4 to 7.
struct BlacksUnderGrid {
    mm::GridBlacksLayout blacks;   ///< the first layout, its column 0 dark
    mm::GridLayout grid;           ///< the second, over the same place
    mm::Layouts layouts;           ///< both, in that order
    mm::Layer layer;               ///< the layer the picture reads
    BlacksUnderGrid() {
        blacks.width = 4; blacks.height = 1; blacks.depth = 1;
        blacks.blackStart = 0; blacks.blackCount = 1;
        grid.width = 4; grid.height = 1; grid.depth = 1;
        layouts.addChild(&blacks);
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

// A picture and the module that owns its table, on the heap: both beside a case's checks outgrow a 512-byte frame.
struct Pictured {
    Owner owner;                            ///< whose memory the table counts against
    mm::LayoutPicture picture{owner};       ///< the picture under test
};

// Whether a placement table outweighs an RGB frame, which a refusal needs to tell them apart; a case that needs it says why it skips.
bool tableOutweighsFrame() {
    if (sizeof(mm::nrOfLightsType) > 3) return true;
    MESSAGE("skipped: a light index of " << sizeof(mm::nrOfLightsType) << " bytes");
    return false;
}

// A snaked 4x2 panel and a picture placed on it while every allocation above a 4x2 RGB frame is refused: a picture without room for its table.
struct RefusedPicture {
    std::unique_ptr<LayoutFace<mm::PanelsLayout>> face = panelsFace(1, 4, 2, true);   ///< the panel, built with room
    std::unique_ptr<Pictured> pictured = std::make_unique<Pictured>();                ///< the picture, refused its table
    bool placedFirst = true;                                                          ///< what the refused place() answered
    RefusedPicture() {
        mm::test::RefuseAbove refuse(24);
        placedFirst = pictured->picture.place(face->layer, 4, 2);
    }
};

// Each light's bytes as its own color, so a drawn pixel names the light it shows.
void identity(const uint8_t* s, uint8_t* rgb) MM_NONBLOCKING { std::memcpy(rgb, s, 3); }

}  // namespace

TEST_CASE("LayoutPicture shows light i on pixel i, holding no table, when the layout's order is the raster") {
    auto f = panelsFace(1, 4, 2, false);
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    REQUIRE(picture.place(f->layer, 4, 2));
    for (size_t p = 0; p < 8; p++) CHECK(picture.lightAt(p) == p);
    CHECK(pictured->owner.dynamicBytes() == 0);
}

TEST_CASE("LayoutPicture shows each pixel the light the layout puts there, for panels chained side by side") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto chained = panelsFace(2, 2, 2, false);   // two 2x2 panels side by side
    REQUIRE(picture.place(chained->layer, 4, 2));
    CHECK(picture.lightAt(2) == 4);   // x 2 of the top row is panel 1's first light
    CHECK(pictured->owner.dynamicBytes() == 8 * sizeof(mm::nrOfLightsType));
}

TEST_CASE("LayoutPicture shows each pixel the light the layout puts there, for a snaked panel") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto snaked = panelsFace(1, 4, 2, true);
    REQUIRE(picture.place(snaked->layer, 4, 2));
    CHECK(picture.lightAt(7) == 4);   // the second row runs back
}

TEST_CASE("LayoutPicture ignores a light placed outside the face, leaving the pixel its index wraps to with its own light") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto left = std::make_unique<LayoutFace<mm::test::LeftOfBox>>();
    left->build();
    REQUIRE(picture.place(left->layer, 4, 2));
    CHECK(picture.lightAt(3) == 4);   // (3, 0) keeps its own light; light 0, left of the box, lands nowhere
    for (size_t p = 0; p < 8; p++) CHECK(picture.lightAt(p) != 0);
}

TEST_CASE("LayoutPicture takes no table for a layout shorter than its box whose lights run in raster order") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto inOrder = std::make_unique<LayoutFace<ShortRaster>>();   // 6 lights over a 4x2 box, the last two pixels empty
    inOrder->build();
    REQUIRE(picture.place(inOrder->layer, 4, 2));
    CHECK(pictured->owner.dynamicBytes() == 0);
    CHECK(picture.lightAt(5) == 5);
}

TEST_CASE("LayoutPicture leaves a pixel no light reaches empty, for a layout shorter than its box out of raster order") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto outOfOrder = std::make_unique<LayoutFace<ShortRaster>>();   // 6 lights over a 4x2 box, lights 4 and 5 swapped across row 1
    outOfOrder->layout.inOrder = false;
    outOfOrder->build();
    REQUIRE(picture.place(outOfOrder->layer, 4, 2));
    CHECK(picture.lightAt(7) == 4);   // light 4 at (3, 1)
    CHECK(picture.lightAt(4) == 5);   // light 5 at (0, 1)
    CHECK(picture.lightAt(5) == mm::Layer::kNoDriver);   // no light at (1, 1)
}

TEST_CASE("LayoutPicture starts placed, with an empty note for a status line") {
    auto pictured = std::make_unique<Pictured>();
    CHECK(pictured->picture.placed());
    CHECK(std::string_view(pictured->picture.note()).empty());
}

TEST_CASE("LayoutPicture without room for a needed table says so, keeps the raster order, and names it for a status line") {
    if (!tableOutweighsFrame()) return;
    RefusedPicture r;
    CHECK_FALSE(r.placedFirst);
    for (size_t p = 0; p < 8; p++) CHECK(r.pictured->picture.lightAt(p) == p);
    CHECK_FALSE(r.pictured->picture.placed());
    CHECK(std::string_view(r.pictured->picture.note()) == ", no memory to place the lights by the layout");
}

TEST_CASE("LayoutPicture places by the layout again, with an empty note, once room returns") {
    if (!tableOutweighsFrame()) return;
    RefusedPicture r;
    REQUIRE(r.pictured->picture.place(r.face->layer, 4, 2));
    CHECK(r.pictured->picture.placed());
    CHECK(std::string_view(r.pictured->picture.note()).empty());
    CHECK(r.pictured->picture.lightAt(7) == 4);   // the snaked second row, placed
}

TEST_CASE("LayoutPicture draws each pixel's light as a scale by scale block, black where no light reaches") {
    auto f = panelsFace(1, 2, 2, true);   // 2x2, its second row running back: lights 2 and 3 swap places
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    REQUIRE(picture.place(f->layer, 2, 2));
    const uint8_t src[4 * 3] = {10, 10, 10, 20, 20, 20, 30, 30, 30, 40, 40, 40};
    std::vector<uint8_t> dst(4 * 4 * 3, 0xFF);
    picture.draw(src, 3, 3, 2, dst.data(), identity);   // light 3 is past `have`, so it draws black
    const auto at = [&](int x, int y) { return dst[(static_cast<size_t>(y) * 4 + x) * 3]; };
    CHECK(at(0, 0) == 10); CHECK(at(1, 1) == 10);   // light 0's block
    CHECK(at(2, 0) == 20); CHECK(at(3, 1) == 20);   // light 1's block
    CHECK(at(0, 2) == 0);  CHECK(at(1, 3) == 0);    // pixel (0, 1) shows light 3, which the buffer lacks
    CHECK(at(2, 2) == 30); CHECK(at(3, 3) == 30);   // pixel (1, 1) shows light 2
}

TEST_CASE("LayoutPicture takes a table for a dense layout with a light outside the box, and leaves that light's pixel dark") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto f = std::make_unique<LayoutFace<mm::test::OneOutsideDense>>();
    f->build();
    REQUIRE(picture.place(f->layer, 4, 2));
    CHECK(pictured->owner.dynamicBytes() == 8 * sizeof(mm::nrOfLightsType));
    CHECK(picture.lightAt(3) == mm::Layer::kNoDriver);   // (3, 0) has no light: light 3 sits outside
    CHECK(picture.lightAt(4) == 4);
}

TEST_CASE("LayoutPicture shows the lit light where a GridBlacks dark slot shares its pixel with an overlapping layout, since a gap claims no pixel") {
    auto pictured = std::make_unique<Pictured>();
    mm::LayoutPicture& picture = pictured->picture;
    auto f = std::make_unique<BlacksUnderGrid>();
    REQUIRE(picture.place(f->layer, 4, 1));
    CHECK(picture.lightAt(0) == 4);   // GridBlacks' dark slot came first; the Grid's light at (0, 0) is what shows
    CHECK(picture.lightAt(1) == 1);   // elsewhere the first layout's light shows, as on the wall
}
