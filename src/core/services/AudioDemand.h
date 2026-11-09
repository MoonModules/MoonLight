#pragma once

#include <cstdint>

#include "core/module/Demand.h"

namespace mm {

/// The audio features a reader can need, a bit each, computed only while something needs one: `Bands` the analysis, `Notes` the semitone bank on top of it.
enum class AudioFeature : uint8_t { Notes, Bands };

/// A module's need for an audio feature: a Demand on AudioService, with the feature named rather than numbered.
class AudioDemand : public Demand {
public:
    /// The provider's type name, the one the readers address.
    static constexpr const char* kProvider = "AudioService";
    /// The bit `f` takes.
    static constexpr uint8_t bitOf(AudioFeature f) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(f)); }
    /// `owner`'s need for `feature`, off until the owner turns it on.
    AudioDemand(MoonModule& owner, AudioFeature feature) : Demand(owner, kProvider, bitOf(feature)) {}
};

} // namespace mm
