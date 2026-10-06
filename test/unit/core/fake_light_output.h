#pragma once

#include "core/util/LightOutput.h"
#include "core/util/JsonSink.h"

#include <cstdint>

namespace mm::test {

/// A light output with twelve palettes thirty degrees of hue apart, so a core test's color mapping is exact.
/// A fake Drivers derives from it beside MoonModule and claims the seat, as the real one does from prepare.
struct FakeLightOutput : public LightOutput {
    /// The summary a test sets directly.
    const LightSummary& summary() const override { return summary_; }
    /// The palette whose hue is nearest, ignoring saturation.
    uint8_t nearestPalette(uint16_t hue, uint8_t) const override { return static_cast<uint8_t>(((hue + 15) / 30) % 12); }
    /// Palette `index` sits at `index` times thirty degrees, fully saturated.
    void paletteHueSat(uint8_t index, uint16_t& hue, uint16_t& sat) const override { hue = index * 30u; sat = 255; }
    /// Twelve palettes.
    uint8_t paletteCount() const override { return 12; }
    /// One name, which is all a shape test reads.
    void writePaletteNames(JsonSink& sink) const override { sink.append("\"P0\""); }
    using LightOutput::nearestPalette;

    /// What summary() answers.
    LightSummary summary_;
    /// The seat a test claims, as the real Drivers does from prepare.
    ActiveInstance<LightOutput> outputSeat_{*this};

protected:
    ~FakeLightOutput() = default;
};

}  // namespace mm::test
