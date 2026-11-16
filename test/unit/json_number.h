#pragma once

/// A double as a JSON number, for the scenario runner, which keeps a fixture's numbers as doubles to hold their precision. Firmware writes its numbers through the typed serializers, since an ESP32 runs a double in software.

#include "core/util/JsonSink.h"

#include <cmath>

namespace mm {

/// Write `v` as one syntactically correct value: a whole value as an integer, a fraction compactly, and NaN or infinity, which JSON lacks, as null.
inline void writeJsonNumber(JsonSink& sink, double v) {
    if (!std::isfinite(v)) { sink.append("null"); return; }   // the cast below is undefined for them
    // The range test guards the cast the whole-value test itself uses.
    if (v >= -9.2e18 && v <= 9.2e18 && v == static_cast<double>(static_cast<long long>(v))) {
        sink.appendf("%lld", static_cast<long long>(v));
    } else {
        sink.appendf("%g", v);
    }
}

}  // namespace mm
