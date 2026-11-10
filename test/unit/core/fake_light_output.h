#pragma once

#include "core/util/LightOutput.h"
#include "core/util/JsonSink.h"

#include <cstdint>

namespace mm::test {

/// A light output with twelve palettes thirty degrees of hue apart, so a core test's color mapping is exact; a fake Drivers derives from it and claims the seat.
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
    /// The outputs a test sets, as the JSON the real Drivers writes.
    void writeOutputs(JsonSink& sink) const override { sink.append(outputs_); }
    /// The effects a test sets, as the JSON the real Drivers writes.
    void writeEffects(JsonSink& sink) const override { sink.append(effects_); }
    using LightOutput::nearestPalette;

    /// What summary() answers.
    LightSummary summary_;
    /// What writeOutputs() writes.
    const char* outputs_ = "";
    /// What writeEffects() writes.
    const char* effects_ = "";
    /// The seat a test claims, as the real Drivers does from prepare.
    ActiveInstance<LightOutput> outputSeat_{*this};

protected:
    ~FakeLightOutput() = default;
};

}  // namespace mm::test
