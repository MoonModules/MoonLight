/// @module SolidEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/SolidEffect.h"
#include "light/layouts/GridLayout.h"

// Mode 0 (RGB(W)) fills the whole buffer with one uniform color: every light equals red/green/blue.
TEST_CASE("SolidEffect mode 0 fills the buffer with one uniform color") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::SolidEffect solid;
    solid.colorMode = mm::SolidEffect::kColorFlat;
    solid.red = 100;
    solid.green = 50;
    solid.blue = 25;
    solid.brightness = 255;  // brightness 255 leaves the color unscaled
    layer.addChild(&solid);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 16);
    // Every light carries exactly the configured RGB — the whole grid is one flat color.
    for (size_t i = 0; i < buf.count(); i++) {
        CHECK(buf.data()[i * 3 + 0] == 100);
        CHECK(buf.data()[i * 3 + 1] == 50);
        CHECK(buf.data()[i * 3 + 2] == 25);
    }
}

// Brightness scales the flat color down per channel (channel * brightness / 255).
TEST_CASE("SolidEffect mode 0 scales the flat color by brightness") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 2;
    grid.height = 2;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::SolidEffect solid;
    solid.colorMode = mm::SolidEffect::kColorFlat;
    solid.red = 200;
    solid.green = 100;
    solid.blue = 0;
    solid.brightness = 128;  // half brightness roughly halves each channel
    layer.addChild(&solid);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    // 200*128/255 = 100, 100*128/255 = 50, 0 stays 0 — uniform across the grid.
    for (size_t i = 0; i < buf.count(); i++) {
        CHECK(buf.data()[i * 3 + 0] == static_cast<uint8_t>(200 * 128 / 255));
        CHECK(buf.data()[i * 3 + 1] == static_cast<uint8_t>(100 * 128 / 255));
        CHECK(buf.data()[i * 3 + 2] == 0);
    }
}

// On an RGBW layer mode 0 writes the white channel too (white scaled by brightness).
TEST_CASE("SolidEffect mode 0 writes the white channel on an RGBW layer") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 3;
    grid.height = 1;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(4);  // RGBW

    mm::SolidEffect solid;
    solid.colorMode = mm::SolidEffect::kColorFlat;
    solid.red = 10;
    solid.green = 20;
    solid.blue = 30;
    solid.white = 200;
    solid.brightness = 255;
    layer.addChild(&solid);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.count(); i++) {
        CHECK(buf.data()[i * 4 + 0] == 10);
        CHECK(buf.data()[i * 4 + 1] == 20);
        CHECK(buf.data()[i * 4 + 2] == 30);
        CHECK(buf.data()[i * 4 + 3] == 200);  // white channel carries the configured white
    }
}

// The effect runs at a degenerate 0×0×0 grid and at every color mode without crashing.
TEST_CASE("SolidEffect survives a 0x0x0 grid across all color modes") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::SolidEffect solid;
    layer.addChild(&solid);

    for (uint8_t mode = 0; mode < mm::SolidEffect::kColorModeCount; mode++) {
        solid.colorMode = mode;
        layer.applyState();
        layer.tick();  // must not crash on an empty grid
    }
    CHECK(true);
}
