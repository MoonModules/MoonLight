#pragma once
/// A layout's lights in strip order, as its placeLights emits them, for a test that compares geometry.

#include "light/layouts/LayoutBase.h"

#include <vector>

namespace mm::test {

/// One placed light.
struct Light { lengthType x, y, z; bool operator==(const Light&) const = default; };

/// The sink callback that appends each light.
inline void collectLight(void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
    static_cast<std::vector<Light>*>(ctx)->push_back({x, y, z});
}

/// Every light `layout` places, in the order it places them.
template <typename L>
std::vector<Light> lightsOf(const L& layout) {
    std::vector<Light> v;
    layout.placeLights(CoordSink{collectLight, nullptr, &v});
    return v;
}

}  // namespace mm::test
