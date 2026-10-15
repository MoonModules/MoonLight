#pragma once

#include "core/util/math16.h"          // BeatPhase, sin16, triwave16, angle16

#include <cstdint>

/// Independent low-frequency oscillators, advanced once per frame and read as often as an effect likes.
///
/// @technical
///
/// Every animated quantity in a generative field is an oscillator.
/// Where a shape sits, how far a coordinate is displaced, how fast a layer rotates, how bright it is this instant.
/// Written by hand that is a phase member, a waveform call and a range map per quantity.
/// That is why an effect animating several things grows a row of near-identical members and a block of arithmetic in its tick.
/// The bank is that pattern owned once: declare how many, set each one's rate, waveform and output range, advance the bank, then read.
///
/// ## Why a bank rather than loose oscillators
///
/// Advancing is one pass, so the cost is per frame rather than per pixel.
/// A pixel loop reads values already computed, which is the whole point when a field samples the same oscillator thousands of times.
///
/// Phases are held together, so oscillators keep their relationships.
/// Two layers at the same rate a quarter-cycle apart stay that way for as long as the device runs.
/// That is what makes a composition read as deliberate rather than as drift.
///
/// ## How a bank is used
///
/// Once per frame the bank is advanced to the current time, and a value is then read wherever it is needed, including inside a pixel loop.
/// Configuration happens in prepare, or whenever a control changes.
///
/// A rate change takes effect from that frame without jumping the phase, which is what live reconfiguration requires.
///
/// ## Why the range map divides rather than shifts
///
/// Dividing by the full range rather than shifting is the difference between an oscillator that reaches its stated maximum and one that stops a step short of it forever.
/// An effect sweeping a hue to its top would never arrive.
///
/// The intermediate is wide because a range spanning the full signed width overflows a narrow product.
/// A caller mapping onto pixel coordinates times a scale gets there sooner than it looks.
///
/// ## Standard shapes only
///
/// Sine, triangle, sawtooth and square, all from the 16-bit tier.
/// Anything richer is a combination of oscillators, which is the vocabulary these shaders are written in, rather than a new waveform here.

namespace mm {

/// The shape an oscillator traces over its cycle.
enum class Wave : uint8_t {
    Sine,        ///< the default: smooth, no corners, the shape most motion wants
    Triangle,    ///< linear out and back; sharper turns than a sine at the same rate
    Saw,         ///< ramp up then jump back: rotation, scrolling, anything that only goes one way
    Square,      ///< low half the cycle, high the other: switching, strobing, hard alternation
};

/// One oscillator's settings.
struct Oscillator {
    uint16_t rate = 0;          ///< cycles per minute, zero holding the phase still
    int32_t  low = 0;           ///< the output range's low end, inclusive
    int32_t  high = 65535;      ///< its high end, which may sit below the low one to run backwards
    angle16  phaseOffset = 0;   ///< an angle added at read time, so two can share a rate and sit apart
    Wave     wave = Wave::Sine; ///< the shape it traces
};

/// A fixed-size bank, its count a compile-time one so the member array needs no allocation.
template <uint8_t N>
class OscillatorBank {
public:
    /// How many oscillators this bank holds.
    static constexpr uint8_t kCount = N;

    /// Configure oscillator `i`. Out-of-range indices are ignored rather than trapping: an effect driving the bank from a control must not be able to crash the device with a bad index.
    void set(uint8_t i, const Oscillator& osc) {
        if (i < N) osc_[i] = osc;
    }

    /// Read one oscillator's settings, for a caller that adjusts a single field.
    const Oscillator& get(uint8_t i) const { return osc_[i < N ? i : N - 1]; }

    /// Advance every oscillator to the current time. Call once per frame, before reading. Takes
    void advanceTo(uint32_t nowMs) {
        for (uint8_t i = 0; i < N; i++) phase_[i].advanceTo(nowMs, osc_[i].rate);
    }

    /// Oscillator `i`'s current phase as an angle16, offset included. The raw cycle position, for a caller that wants to drive something other than the configured range.
    angle16 phase(uint8_t i) const {
        if (i >= N) return 0;
        return static_cast<angle16>(phase_[i].phase(65536) + osc_[i].phaseOffset);
    }

    /// Oscillator `i`'s current value, mapped into its configured range. Cheap enough to call per pixel, though an effect whose value is constant across the frame should hoist it.
    int32_t value(uint8_t i) const {
        if (i >= N) return 0;
        return map(osc_[i], unit(osc_[i].wave, phase(i)));
    }

    /// Oscillator `i`'s value as a 0..65535 unit position, ignoring the configured range. For a caller doing its own mapping, and the form `value` is built on.
    uint16_t unitValue(uint8_t i) const {
        if (i >= N) return 0;
        return unit(osc_[i].wave, phase(i));
    }

    /// Restart every phase at zero, keeping the configuration. For an effect whose composition must begin from a known state.
    void reset() {
        for (uint8_t i = 0; i < N; i++) phase_[i].reset();
    }

private:
    /// The waveform, as a 0..65535 position through the cycle.
    static uint16_t unit(Wave wave, angle16 theta) {
        switch (wave) {
            // sin16 is signed around zero; shift it into the unsigned range the mapping expects.
            case Wave::Sine:     return static_cast<uint16_t>(sin16(theta) + 32768);
            case Wave::Triangle: return triwave16(theta);
            case Wave::Saw:      return theta;
            case Wave::Square:   return theta < 32768 ? 0 : 65535;
        }
        return 0;
    }

    /// Map a unit position onto the output range, inclusive at both ends.
    static int32_t map(const Oscillator& osc, uint16_t u) {
        const int64_t span = static_cast<int64_t>(osc.high) - osc.low;
        return static_cast<int32_t>(osc.low + (span * u) / 65535);
    }

    Oscillator osc_[N];
    BeatPhase  phase_[N];
};

}  // namespace mm
