#pragma once

/// One include writes a layout: the base plus the maths a coordinate placement reaches for.

#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType, Coord3D
#include "core/util/math8.h"        // sin8/cos8/atan2_8, integer trig for circular layouts

#include <cmath>              // sinf/cosf/fmodf, float trig where a layout needs it
#include <cstdint>          // fixed-width ints
#include <limits>          // std::numeric_limits, the lightCount clamp GridLayout uses
#include <numbers>       // std::numbers::pi_v, portable pi for ring layouts

namespace mm {

/// The per-light function a `CoordSink` carries, taking a physical index and a position.
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

// The builder/visitor-sink shape, one named method per kind, avoiding a boolean parameter.
/// The sink a layout emits its positions into, with one method per kind of pixel.
struct CoordSink {
    /// The lit-pixel handler, which every layout reaches.
    CoordCallback cb;
    /// The gap handler, or null to let a gap fall back to `cb`.
    CoordCallback blackCb;
    /// The consumer's own state, handed back to whichever handler runs.
    void* ctx;

    /// A normal light at physical index `idx`, position (x,y,z). What every layout calls.
    void pixel(nrOfLightsType idx, lengthType x, lengthType y, lengthType z) const {
        cb(ctx, idx, x, y, z);
    }
    /// A gap light: a physical slot that stays black, for a layout with dark regions.
    void blackPixel(nrOfLightsType idx, lengthType x, lengthType y, lengthType z) const {
        (blackCb ? blackCb : cb)(ctx, idx, x, y, z);
    }
};

class LayoutBase;

/// The running index a layout emits through, so a layout built from other layouts appends each part after the last. It stops at the light count, and with no sink it only counts.
struct LightEmitter {
    /// Where the lights go, or null to count them.
    const CoordSink* sink = nullptr;
    /// The light count, past which nothing is emitted.
    uint32_t limit = std::numeric_limits<uint32_t>::max();
    /// The next physical index, and so the count so far.
    uint32_t idx = 0;
    /// Emit the next light, false once the bound is reached.
    bool pixel(lengthType x, lengthType y, lengthType z) {
        if (idx >= limit) return false;
        if (sink) sink->pixel(static_cast<nrOfLightsType>(idx), x, y, z);
        idx++;
        return true;
    }
    /// Emit the next light as a gap that stays black, false once the bound is reached.
    bool blackPixel(lengthType x, lengthType y, lengthType z) {
        if (idx >= limit) return false;
        if (sink) sink->blackPixel(static_cast<nrOfLightsType>(idx), x, y, z);
        idx++;
        return true;
    }
    /// Place a whole layout as the next part, shifted by (dx, dy, dz), its numbering continuing this one's and its gaps staying gaps.
    void place(const LayoutBase& part, lengthType dx = 0, lengthType dy = 0, lengthType dz = 0);
};

/// A layout says where the lights physically are: it maps each light's index to an (x, y, z) position, the shape an [effect](https://moonmodules.org/MoonLight/moonmodules/light/effects.html) draws onto and a [driver](https://moonmodules.org/MoonLight/moonmodules/light/drivers.html) sends out.
/// @video 05-layouts.webm Layouts: a grid, a ring, a spiral, a cube, tubes, and two written in MoonLive.
///
/// The @ref Layouts container holds one or more layouts and composes them into one coordinate space, which a @ref Layer renders over. The container itself takes no controls.
///
/// Each card's emoji are its tags, as the [tag emoji legend](https://moonmodules.org/MoonLight/explanation/architecture/index.html#tag-emoji-legend) explains.
///
/// @moreinfo
///
/// A concrete layout implements `lightCount` and `placeLights` directly. Every layout control changes the light count, so any change rebuilds the pipeline.
///
/// A wiring control, reversed, serpentine or an axis order, changes only the order the lights are emitted in, never a light's coordinate: the order is what the driver's index means. A layout counts by running its own walk without a sink, so the count and the lights cannot disagree. Float trig is fine here, since placement runs at rebuild, never in the render loop.
///
/// A layout can be built from others: it makes one, sets its public settings and places it through `LightEmitter::place` as the next part, its numbering continuing. The Layouts container places its children the same way. CarLights and Rings241 are made of rings, Tubes of columns, Panels of panels and GridBlacks of a grid.
class LayoutBase : public MoonModule {
public:
    /// A layout, so the Layouts container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Layout; }
    /// How many physical lights this layout places.
    virtual nrOfLightsType lightCount() const = 0;
    /// Emit every light's position into the sink, in physical index order.
    virtual void placeLights(const CoordSink& sink) const = 0;

    /// The shape this layout lays out, a line, a picture or a volume: advisory only, since nothing in the render path reads it.
    virtual Dim dimensions() const { return Dim::D2; }

    /// Whether this layout holds any physical slots dark, which turns the dense-identity fast path off, since an identity map would light a gap.
    virtual bool hasBlackPixels() const { return false; }

    /// Whether a control change rebuilds the pipeline, which every layout control does.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }
    /// A light count clamped to what the index type holds, so an oversized layout clamps rather than wrapping.
    static nrOfLightsType clampedCount(uint64_t n) {
        constexpr uint64_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }
};

inline void LightEmitter::place(const LayoutBase& part, lengthType dx, lengthType dy, lengthType dz) {
    if (sink) {
        struct Relay { const CoordSink* sink; uint32_t offset, limit; lengthType dx, dy, dz; };
        Relay relay{sink, idx, limit, dx, dy, dz};
        part.placeLights(CoordSink{
            [](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType z) {
                auto* r = static_cast<Relay*>(c);
                if (uint32_t(i) + r->offset < r->limit)
                    r->sink->pixel(static_cast<nrOfLightsType>(i + r->offset), static_cast<lengthType>(x + r->dx), static_cast<lengthType>(y + r->dy), static_cast<lengthType>(z + r->dz));
            },
            [](void* c, nrOfLightsType i, lengthType x, lengthType y, lengthType z) {
                auto* r = static_cast<Relay*>(c);
                if (uint32_t(i) + r->offset < r->limit)
                    r->sink->blackPixel(static_cast<nrOfLightsType>(i + r->offset), static_cast<lengthType>(x + r->dx), static_cast<lengthType>(y + r->dy), static_cast<lengthType>(z + r->dz));
            },
            &relay});
    }
    idx += part.lightCount();
}

} // namespace mm
