#pragma once

/// One include writes a modifier: the base plus the maths a coordinate fold reaches for.

#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType, Dim
#include "core/util/math8.h"        // sin8/cos8, integer trig for a rotate modifier

#include <cmath>              // std::sqrt, sin, cos: float trig for circle folds
#include <cstdint>           // fixed-width ints
#include <cstdlib>          // std::abs
#include <algorithm>       // std::max / std::min / std::clamp

namespace mm {

/// A modifier sits between an [effect](https://moonmodules.org/MoonLight/moonmodules/light/effects.html) and the output: it reshapes where the pixels land, or masks them, without changing what the effect draws.
///
/// Modifiers compose: a @ref Layer applies its whole stack in child order, each reshaping the result of the one below. A static modifier is baked into the layer's mapping at rebuild. A dynamic one also runs a pass every frame, and an animated one rebuilds its static fold on its own clock.
///
/// A modifier folds coordinates rather than drawing, so it uses little of the shared [power functions](https://moonmodules.org/MoonLight/moonmodules/light/power-functions.html). Each card's emoji are its tags, as the [tag emoji legend](https://moonmodules.org/MoonLight/explanation/architecture/index.html#tag-emoji-legend) explains.
///
/// @technical
///
/// A layer builds its mapping by folding every physical light through each enabled modifier, and the whole chain collapses into one table, so the per-frame render stays a single lookup.
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

    /// Whether this modifier's mapping changed and wants a rebuild, clearing the flag; polled once per frame across every enabled modifier, so several coalesce into one rebuild.
    virtual bool consumeNeedsRebuild() { return false; }
};

} // namespace mm
