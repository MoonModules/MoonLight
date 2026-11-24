#pragma once

/// One include writes a modifier: the base plus the maths a coordinate fold reaches for.

#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType, Dim
#include "core/util/math8.h"        // sin8/cos8, integer trig for a rotate modifier
#include "core/util/math16.h"       // sin16/cos16, the finer trig a live rotation steps with

#include <cmath>              // std::sqrt, sin, cos: float trig for circle folds
#include <cstdint>           // fixed-width ints
#include <cstdlib>          // std::abs
#include <algorithm>       // std::max / std::min / std::clamp

namespace mm {

/// A live map from a destination light to the source it samples, as an affine transform in 16.16 fixed point: `src = (a·x + b·y + c, d·x + e·y + f)`.
///
/// A rotation, a zoom, a shear and a pan are each one, and so is any chain of them, so the Layer warps a frame by stepping the source along each row, two additions a light, the textbook incremental affine warp.
struct LiveAffine {
    int32_t a = 65536, b = 0, c = 0;   ///< the source x: per destination x, per destination y, and the offset
    int32_t d = 0, e = 65536, f = 0;   ///< the source y, likewise
    /// This map applied after `first`: a chain of live modifiers composes into one.
    LiveAffine after(const LiveAffine& first) const {
        const auto mul = [](int32_t p, int32_t q) { return static_cast<int32_t>((static_cast<int64_t>(p) * q) >> 16); };
        return {mul(a, first.a) + mul(b, first.d), mul(a, first.b) + mul(b, first.e), mul(a, first.c) + mul(b, first.f) + c,
                mul(d, first.a) + mul(e, first.d), mul(d, first.b) + mul(e, first.e), mul(d, first.c) + mul(e, first.f) + f};
    }
    /// Where light (x, y) samples, rounded to the nearest source light.
    void apply(lengthType& x, lengthType& y) const {
        const int64_t sx = static_cast<int64_t>(a) * x + static_cast<int64_t>(b) * y + c;
        const int64_t sy = static_cast<int64_t>(d) * x + static_cast<int64_t>(e) * y + f;
        x = static_cast<lengthType>((sx + 0x8000) >> 16);
        y = static_cast<lengthType>((sy + 0x8000) >> 16);
    }
};

/// A modifier sits between an [effect](https://moonmodules.org/MoonLight/moonmodules/light/effects.html) and the output: it reshapes where the pixels land, or masks them, without changing what the effect draws.
///
/// Modifiers compose: a @ref Layer folds its static modifiers in child order, each reshaping the result of the one below, into its mapping at rebuild. A dynamic modifier instead runs a pass every frame after all effects, on the logical buffer the static mapping then carries to the lights, whatever its place, and an animated one rebuilds its static fold on its own clock.
///
/// A modifier folds coordinates rather than drawing, so it uses little of the shared [power functions](https://moonmodules.org/MoonLight/moonmodules/light/power-functions.html). Each card's emoji are its tags, as the [tag emoji legend](https://moonmodules.org/MoonLight/explanation/architecture/index.html#tag-emoji-legend) explains.
///
/// @technical
///
/// A layer builds its mapping by folding every physical light through each enabled static modifier, and the whole chain collapses into one table, so the per-frame render stays a single lookup.
///
/// Three hooks do it, each a no-op by default, so a modifier implements only what it needs. The build walks physical lights, so several folding onto one logical cell is the fan-out, which costs nothing, and the mapping cannot overflow. Float math is fine in a static fold, since it runs while the mapping builds, never per frame.
///
/// Most modifiers are not affine, a mask being a predicate and a tile a modulo; rotation is the exception, written as an integer rotation matrix. It is the textbook image-warping pattern: a transform baked into a spatial table, built backward.
class ModifierBase : public MoonModule {
public:
    /// A modifier, so a layer accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Modifier; }

    /// Whether a control change rebuilds the mapping, which every modifier control does.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }
    /// The layer holding it: the mapping it shapes is that layer's alone, and Drivers re-prepares only if the layer's shape changed.
    PrepareScope prepareScope() const override { return PrepareScope::Parent; }

    /// Which axes this modifier can transform, advisory only: unlike an effect's dimensions, the render path never reads it.
    virtual Dim dimensions() const { return Dim::D3; }

    /// Reshape the running logical box, once per rebuild in child order; a modifier whose per-light fold needs the box stashes it here, reading its own stage.
    virtual void modifyLogicalSize(Coord3D& /*size*/) {}

    /// Fold one coordinate into this stage's logical space, returning false to reject it: a bool, since a later modulo could alias a sentinel coordinate back in range.
    virtual bool modifyLogical(Coord3D& /*pos*/) const { return true; }

    /// Remap a coordinate per frame without rebuilding the mapping, run only when some enabled modifier overrides it, so a static chain pays nothing.
    virtual void modifyLive(Coord3D& /*pos*/, const Coord3D& /*logical*/) const {}

    /// Whether this modifier does per-frame work, which gates the live pass.
    virtual bool hasModifyLive() const { return false; }

    /// This frame's live map as one affine transform over a `logical` box, false for a map that is none; when every live modifier answers, the Layer warps the frame incrementally rather than per light through modifyLive.
    virtual bool liveAffine(LiveAffine& /*out*/, const Coord3D& /*logical*/) const { return false; }

    /// Whether this modifier's mapping changed and wants a rebuild, clearing the flag; polled once per frame across every enabled modifier, so several coalesce into one rebuild.
    virtual bool consumeNeedsRebuild() { return false; }
};

} // namespace mm
