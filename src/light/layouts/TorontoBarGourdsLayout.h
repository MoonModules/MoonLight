#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// A bar's installation of 24 decorative gourds, each lit at one of three granularities: one light per gourd, per side, or per LED.
/// @card TorontoBarGourdsLayout.gif
///
/// @attribution troyhacks
///
/// @guide
///
/// ## granularity
///
/// One gourd one light stacks a gourd's lights on its own position. One side one light gives each of a gourd's five sides one point, and one LED one light places every LED on its side's ring of 12. Both hold 61 lights a gourd, the middle one included.
///
/// @moreinfo
///
/// A gourd is a five-sided body, front, back, left, right and bottom, plus a middle light, at a fixed position in a 3D grid.
class TorontoBarGourdsLayout : public LayoutBase {
public:
    /// Lights per gourd when one gourd is one light.
    uint8_t nrOfLightsPerGourd = 61;
    /// How coarsely the gourds are lit, in option order.
    enum Granularity : uint8_t { kGourdPerLight, kSidePerLight, kLedPerLight, kGranularityCount };
    /// How finely each gourd is lit.
    uint8_t granularity = kLedPerLight;

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addSelect("granularity", granularity, kGranularityOptions, kGranularityCount);
        // It matters only when one gourd is one light.
        controls_.addControl("nrOfLightsPerGourd", nrOfLightsPerGourd, 1, 128);
    }

    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        nrOfLightsType n = 0;
        walk([](void*, nrOfLightsType, lengthType, lengthType, lengthType) {}, nullptr, &n);
        return n;
    }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        walk(sink.cb, sink.ctx, nullptr);
    }

private:
    static constexpr const char* kGranularityOptions[] = {
        "One Gourd One Light", "One Side One Light", "One LED One Light"};

    /// Either emits or tallies, so every mode below writes one line per light.
    struct Emit {
        CoordCallback cb;
        void* ctx;
        nrOfLightsType idx = 0;
        void add(lengthType x, lengthType y, lengthType z) {
            if (cb) cb(ctx, idx, x, y, z);
            idx++;
        }
    };

    /// One Gourd One Light: every one of the gourd's lights on the same position.
    void addGourd(Emit& e, Coord3D pos) const {
        for (int i = 0; i < nrOfLightsPerGourd; i++)
            e.add(pos.x, pos.y, pos.z);  // all gourd lights on the same position
    }

    /// The coarsest granularity: each side's twelve lights share one virtual pixel.
    void addGourdSides(Emit& e, Coord3D pos) const {
        const int gourdLength = 3;
        const Coord3D sides[] = {
            Coord3D{1, 1, 0}, Coord3D{2, 1, 1}, Coord3D{1, 1, 2},
            Coord3D{0, 1, 1}, Coord3D{1, 2, 1}};

        for (Coord3D side : sides) {  // 5 sides
            for (int i = 0; i < 12; i++)  // 12 LEDs each, all on the same virtual pixel
                e.add(static_cast<lengthType>(pos.x * gourdLength + side.x),
                      static_cast<lengthType>(pos.y * gourdLength + side.y),
                      static_cast<lengthType>(pos.z * gourdLength + side.z));
        }

        // + middleLED
        const Coord3D side{1, 1, 1};
        e.add(static_cast<lengthType>(pos.x * gourdLength + side.x),
              static_cast<lengthType>(pos.y * gourdLength + side.y),
              static_cast<lengthType>(pos.z * gourdLength + side.z));
    }

    /// The finest granularity: a cube shell whose five faces are each a perimeter ring.
    void addGourdPixels(Emit& e, Coord3D pos) const {
        const int gourdLength = 7;
        const Coord3D pixels[] = {  // 12 pixels each
            Coord3D{0, 0, 0}, Coord3D{1, 0, 0}, Coord3D{2, 0, 0}, Coord3D{3, 0, 0},
            Coord3D{3, 1, 0}, Coord3D{3, 2, 0}, Coord3D{3, 3, 0}, Coord3D{2, 3, 0},
            Coord3D{1, 3, 0}, Coord3D{0, 3, 0}, Coord3D{0, 2, 0}, Coord3D{0, 1, 0}};

        // back and front: z constant, increasing x and y
        for (Coord3D pixel : pixels)  // front
            e.add(static_cast<lengthType>(pos.x * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.y),
                  static_cast<lengthType>(pos.z * gourdLength));
        for (Coord3D pixel : pixels)  // back
            e.add(static_cast<lengthType>(pos.x * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.y),
                  static_cast<lengthType>(pos.z * gourdLength + gourdLength - 1));

        // left and right: x constant, increasing y and z
        for (Coord3D pixel : pixels)  // left
            e.add(static_cast<lengthType>(pos.x * gourdLength),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.z * gourdLength + pixel.y));
        for (Coord3D pixel : pixels)  // right
            e.add(static_cast<lengthType>(pos.x * gourdLength + gourdLength - 1),
                  static_cast<lengthType>(pos.y * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.z * gourdLength + pixel.y));

        // bottom : y constant, increasing x and z
        for (Coord3D pixel : pixels)  // bottom
            e.add(static_cast<lengthType>(pos.x * gourdLength + pixel.x),
                  static_cast<lengthType>(pos.y * gourdLength + gourdLength - 1),
                  static_cast<lengthType>(pos.z * gourdLength + pixel.y));

        // + middleLED
        const Coord3D middle{3, 3, 3};
        e.add(static_cast<lengthType>(pos.x * gourdLength + middle.x),
              static_cast<lengthType>(pos.y * gourdLength + middle.y),
              static_cast<lengthType>(pos.z * gourdLength + middle.z));
    }

    /// The one home for the layout: every gourd in grid order, at the chosen granularity.
    void walk(CoordCallback cb, void* ctx, nrOfLightsType* count) const {
        // The gourd order and positions in a 3D grid space.
        const Coord3D gourds[] = {
            Coord3D{0, 0, 0}, Coord3D{1, 0, 0}, Coord3D{2, 0, 0}, Coord3D{3, 0, 0},
            Coord3D{0, 1, 0}, Coord3D{1, 1, 0}, Coord3D{2, 1, 0}, Coord3D{3, 1, 0},
            Coord3D{0, 2, 0}, Coord3D{1, 2, 0}, Coord3D{2, 2, 0}, Coord3D{3, 2, 0},
            Coord3D{0, 0, 1}, Coord3D{1, 0, 1}, Coord3D{2, 0, 1}, Coord3D{3, 0, 1},
            Coord3D{0, 1, 1}, Coord3D{1, 1, 1}, Coord3D{2, 1, 1}, Coord3D{3, 1, 1},
            Coord3D{0, 2, 1}, Coord3D{1, 2, 1}, Coord3D{2, 2, 1}, Coord3D{3, 2, 1}};

        Emit e{cb, ctx};
        for (Coord3D gourd : gourds) {
            if (granularity == kGourdPerLight) {  // one gourd one light
                addGourd(e, gourd);
            } else if (granularity == kSidePerLight) {  // one side one light
                addGourdSides(e, gourd);
            } else if (granularity == kLedPerLight) {  // one LED one light
                addGourdPixels(e, gourd);
            }
        }
        if (count) *count = e.idx;
    }
};

} // namespace mm
