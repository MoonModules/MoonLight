#pragma once

#include <cstdint>
#include <type_traits>
#include "platform_config.h"

/// @defgroup light_types Light-domain foundational types
/// @{
/// The coordinate, count and dimension types every effect, modifier, layout and driver shares.
///
/// @moreinfo
///
/// ## Why they live together
///
/// Roughly twenty files name them and none owns them, so a shared header beats scattering them into an arbitrary one.
/// A symbol that does have a single owner lives with that owner instead.
///
/// ## The boundary stays one-directional
///
/// This header includes the platform layer and never core, and core names none of these types: a module's dimensionality is captured through a probe instead.

namespace mm {

/// A grid coordinate, sixteen bits everywhere, a byte being too narrow for a 128-edge grid.
using lengthType = int16_t;

/// A 3D grid position or size, with per-component operators so a fold reads like the geometry.
///
/// @moreinfo Each modifier is a coordinate transform, and the operators let it read as one line rather than three.
/// Plain data, and the per-pixel inner loop stays on flat indices rather than carrying a struct through it.
struct Coord3D {
    lengthType x = 0, y = 0, z = 0;   ///< the three axes

    Coord3D operator+(const Coord3D& o) const { return {static_cast<lengthType>(x + o.x), static_cast<lengthType>(y + o.y), static_cast<lengthType>(z + o.z)}; }
    Coord3D operator-(const Coord3D& o) const { return {static_cast<lengthType>(x - o.x), static_cast<lengthType>(y - o.y), static_cast<lengthType>(z - o.z)}; }
    Coord3D operator*(const Coord3D& o) const { return {static_cast<lengthType>(x * o.x), static_cast<lengthType>(y * o.y), static_cast<lengthType>(z * o.z)}; }
    // The per-axis forms guard a zero extent, so a modifier folds without pre-checking each axis.
    Coord3D operator%(const Coord3D& o) const { return {modAxis(x, o.x), modAxis(y, o.y), modAxis(z, o.z)}; }
    Coord3D operator/(const Coord3D& o) const { return {divAxis(x, o.x), divAxis(y, o.y), divAxis(z, o.z)}; }
    bool operator==(const Coord3D& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Coord3D& o) const { return !(*this == o); }

private:
    static lengthType modAxis(lengthType a, lengthType m) { return m > 0 ? static_cast<lengthType>(a % m) : a; }
    static lengthType divAxis(lengthType a, lengthType d) { return d > 0 ? static_cast<lengthType>(a / d) : a; }
};

/// A count of lights, and the index the mapping stores: wider with PSRAM, half the size without.
using nrOfLightsType = std::conditional_t<platform::hasPsram, uint32_t, uint16_t>;

/// @}

} // namespace mm
