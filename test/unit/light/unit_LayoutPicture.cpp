/// @module LayoutPicture
/// @also HdmiDriver, NdiDriver, HlsDriver, RtspDriver

/// The layout as a picture: which light each pixel of the physical front face shows, the one placement every screen and stream driver draws through.

#include "doctest.h"
#include "light/drivers/LayoutPicture.h"
#include "wall_fixture.h"

#include <memory>
#include <string>
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

// Each light's bytes as its own color, so a drawn pixel names the light it shows.
void identity(const uint8_t* s, uint8_t* rgb) MM_NONBLOCKING { std::memcpy(rgb, s, 3); }

}  // namespace

TEST_CASE("LayoutPicture shows light i on pixel i, holding no table, when the layout's order is the raster") {
    auto f = panelsFace(1, 4, 2, false);
    Owner owner;
    mm::LayoutPicture picture(owner);
    REQUIRE(picture.place(f->layer, 4, 2));
    for (size_t p = 0; p < 8; p++) CHECK(picture.lightAt(p) == p);
    CHECK(owner.dynamicBytes() == 0);
}

TEST_CASE("LayoutPicture shows each pixel the light the layout puts there, for chained panels and a snaked panel") {
    Owner owner;
    mm::LayoutPicture picture(owner);
    auto chained = panelsFace(2, 2, 2, false);   // two 2x2 panels side by side
    REQUIRE(picture.place(chained->layer, 4, 2));
    CHECK(picture.lightAt(2) == 4);   // x 2 of the top row is panel 1's first light
    CHECK(owner.dynamicBytes() == 8 * sizeof(mm::nrOfLightsType));
    auto snaked = panelsFace(1, 4, 2, true);
    REQUIRE(picture.place(snaked->layer, 4, 2));
    CHECK(picture.lightAt(7) == 4);   // the second row runs back
}

TEST_CASE("LayoutPicture ignores a light placed outside the face, leaving the pixel its index wraps to with its own light") {
    Owner owner;
    mm::LayoutPicture picture(owner);
    auto left = std::make_unique<LayoutFace<mm::test::LeftOfBox>>();
    left->build();
    REQUIRE(picture.place(left->layer, 4, 2));
    CHECK(picture.lightAt(3) == 4);   // (3, 0) keeps its own light; light 0, left of the box, lands nowhere
    for (size_t p = 0; p < 8; p++) CHECK(picture.lightAt(p) != 0);
}

TEST_CASE("LayoutPicture takes no table for a layout shorter than its box whose lights run in raster order") {
    Owner owner;
    mm::LayoutPicture picture(owner);
    auto inOrder = std::make_unique<LayoutFace<ShortRaster>>();   // 6 lights over a 4x2 box, the last two pixels empty
    inOrder->build();
    REQUIRE(picture.place(inOrder->layer, 4, 2));
    CHECK(owner.dynamicBytes() == 0);
    CHECK(picture.lightAt(5) == 5);
    auto outOfOrder = std::make_unique<LayoutFace<ShortRaster>>();
    outOfOrder->layout.inOrder = false;
    outOfOrder->build();
    REQUIRE(picture.place(outOfOrder->layer, 4, 2));
    CHECK(picture.lightAt(7) == 4);   // light 4 at (3, 1)
    CHECK(picture.lightAt(4) == 5);   // light 5 at (0, 1)
    CHECK(picture.lightAt(5) == mm::Layer::kNoDriver);   // no light at (1, 1)
}

TEST_CASE("LayoutPicture without room for a needed table says so, keeps the raster order, and names it for a status line") {
    if (sizeof(mm::nrOfLightsType) <= 3) {   // a table no heavier than a frame: the refusal cannot tell them apart
        MESSAGE("skipped: a light index of " << sizeof(mm::nrOfLightsType) << " bytes");
        return;
    }
    auto f = panelsFace(1, 4, 2, true);
    Owner owner;
    mm::LayoutPicture picture(owner);
    CHECK(picture.placed());
    CHECK(std::string(picture.note()).empty());
    {
        mm::test::RefuseAbove refuse(24);   // a 4x2 RGB frame
        CHECK_FALSE(picture.place(f->layer, 4, 2));
    }
    for (size_t p = 0; p < 8; p++) CHECK(picture.lightAt(p) == p);
    CHECK_FALSE(picture.placed());
    CHECK(std::string(picture.note()) == ", no memory to place the lights by the layout");
    REQUIRE(picture.place(f->layer, 4, 2));   // room again: placed, and an empty note
    CHECK(picture.placed());
    CHECK(std::string(picture.note()).empty());
}

TEST_CASE("LayoutPicture draws each pixel's light as a scale by scale block, black where no light reaches") {
    auto f = panelsFace(1, 2, 2, true);   // 2x2, its second row running back: lights 2 and 3 swap places
    Owner owner;
    mm::LayoutPicture picture(owner);
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
    Owner owner;
    mm::LayoutPicture picture(owner);
    auto f = std::make_unique<LayoutFace<mm::test::OneOutsideDense>>();
    f->build();
    REQUIRE(picture.place(f->layer, 4, 2));
    CHECK(owner.dynamicBytes() == 8 * sizeof(mm::nrOfLightsType));
    CHECK(picture.lightAt(3) == mm::Layer::kNoDriver);   // (3, 0) has no light: light 3 sits outside
    CHECK(picture.lightAt(4) == 4);
}
