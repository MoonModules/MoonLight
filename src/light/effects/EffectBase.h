#pragma once

#include "core/module/MoonModule.h"
#include "light/util/light_types.h" // lengthType, nrOfLightsType, Dim

#include <cstdint>

namespace mm::draw { struct Canvas; }   // draw.h comes in at the bottom, so the accessor's return type is declared here

namespace mm {

class Layer; // forward declaration, defined in light/layers/Layer.h and included at the bottom

/// An effect draws: each frame it writes a color per light into its @ref Layer's buffer, which modifiers reshape and a driver sends out.
///
/// Effects are built from the shared [power functions](https://moonmodules.org/MoonLight/moonmodules/light/power-functions.html), the drawing, field and motion routines every effect composes. One that names an index color reads the global palette, the `palette` control on @ref Drivers.
///
/// Each card's emoji are its tags, as the [tag emoji legend](https://moonmodules.org/MoonLight/explanation/architecture/index.html#tag-emoji-legend) explains, and its dimension is its native axes: a Layer extrudes a lower-dimensional effect onto a bigger grid.
///
/// @moreinfo
///
/// ## Writing an effect
///
/// Derive from EffectBase and include this one file, the whole surface an effect may use. A scripted MoonLive effect gets the same one, and an effect needing more adds one include. EffectBase holds no state of its own: its accessors forward to the parent Layer, so an effect never caches a `Layer*` or the extents. `DriverBase` does the same for drivers.
///
/// ## Animation
///
/// Speed controls use BPM, so 60 BPM is one beat a second, and animation follows `elapsed()` rather than a frame count, so speed holds at any frame rate. Multiply a time offset by the panel dimension, or a large display looks sluggish.
///
/// ## Why the helper includes sit at the bottom
///
/// Layer.h includes this file, since a Layer holds effect children, so this file forward-declares Layer and pulls Layer.h in once the class is complete. Layer.h re-enters harmlessly through its include guard and defines the accessor bodies.
class EffectBase : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Effect; }

    /// Which axes the effect iterates, read at frame time: a hardcoded bound is a buffer overrun.
    virtual Dim dimensions() const { return Dim::D3; }

    /// Whether the frame arrives in the order the lights are wired, so a layer showing it alone hands it to the output unmapped.
    virtual bool deliversWiringOrder() const MM_NONBLOCKING { return false; }
    /// Copy that wiring-order frame into the output, all of it when `force` says the output holds something else.
    virtual void copyWiringFrame(uint8_t* /*out*/, size_t /*bytes*/, bool /*force*/) {}

    /// The parent, always a Layer, defined in Layer.h once Layer is complete.
    Layer* layer() const;

    /// The layer's pixel buffer, sized to width x height x depth x channels.
    uint8_t* buffer();
    /// Lights along x.
    lengthType width() const;
    /// Lights along y.
    lengthType height() const;
    /// Lights along z, which is 1 on a panel or a strip.
    lengthType depth() const;
    /// Bytes each light occupies in the buffer.
    uint8_t channelsPerLight() const;
    /// Lights in the whole layer.
    nrOfLightsType nrOfLights() const;
    /// Milliseconds since render start. Drive animation off this rather than a frame count.
    uint32_t elapsed() const;

    /// The layer's surface as one value, read once a frame since nothing may cache extents across ticks.
    mm::draw::Canvas canvas();

    /// Aim a fixture, a no-op on an absent channel, and never scaled by brightness.
    void setPan(nrOfLightsType index, uint8_t value);
    /// Tilt the fixture, under the same contract as `setPan`.
    void setTilt(nrOfLightsType index, uint8_t value);
    /// Zoom the beam, under the same contract as `setPan`.
    void setZoom(nrOfLightsType index, uint8_t value);
    /// Spin the gobo or prism, under the same contract as `setPan`.
    void setRotate(nrOfLightsType index, uint8_t value);
    /// Select the gobo pattern, whose byte is a range per slot on most heads rather than an index.
    void setGobo(nrOfLightsType index, uint8_t value);

    /// True when the lights carry pan or tilt, so an effect can skip motion math on a strip.
    bool movable() const;

    /// True when the lights carry a gobo or rotate channel, which plenty of moving heads lack.
    bool hasBeam() const;
};

} // namespace mm

// The effect author's helper surface, pulled in after the class so Layer.h re-enters harmlessly.
#include "light/layers/Layer.h"   // EffectBase's out-of-line accessors
#include "light/powerfunctions/draw.h"           // draw::pixel / fill / line / fade / blur: write pixels by coordinate
#include "light/util/Palette.h"        // colorFromPalette, Palettes::active: the palette system
#include "core/util/math8.h"           // beat8 / beatsin8 / sin8 / random8: the integer animation helpers
#include "core/util/noise.h"           // inoise8: the shared gradient-noise field
#include "core/util/color.h"           // RGB
#include "core/util/crc.h"             // crc16: grid and state fingerprints for stasis detection
#include "core/util/ScratchBuffer.h"   // ScratchBuffer<T>: self-sizing scratch memory for stateful effects
#include "core/util/oscillators.h"     // OscillatorBank / Wave: the motion kernel five effects share
#include "core/util/math16.h"          // angle16, kaleido, halfLifeKeep: the fixed-point trig and decay
#include "light/powerfunctions/polar.h"          // PolarLut: the per-pixel angle and radius four radial effects read
#include "core/services/AudioService.h"    // AudioService::latestFrame(): the shared audio source
#include "core/util/AudioFrame.h"      // AudioFrame: level and 16-band spectrum for an audio-reactive effect

#include <cstring>                // memset / memcpy / strcmp: buffer and control-name handling
#include <cmath>                  // sqrtf / sinf / log10f: per-frame float math, never per-light
#include <array>                  // std::array: fixed-size effect state tables
