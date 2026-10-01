#pragma once

/// @defgroup ControlSurface A transport that mirrors the surface state
/// @{
/// A control surface is a view of the state ControlModule owns, kept in step in both directions.
///
/// @moreinfo
///
/// ## Why a surface mirrors rather than syncs
///
/// ControlModule owns the state: `switch1..8`, `encoder1..8`, `fader1..8`, and what each drives.
/// A surface holds no copy, which is what lets two attach at once and stay in step, a phone running Open Stage Control and a desk on the rack.
/// It is also why inbound is not a method here.
/// A surface writes through the same control path the HTTP API and the UI use, so it gains no privilege and there is no second copy to reconcile.
///
/// ## Feedback is not an echo
///
/// On a motorized desk the motors move because the host sends positions back, and on an Akai APC the pads have no meaning until the host lights them.
/// The surface is often the output device, and this interface is how it gets driven.
///
/// ## Why four verbs
///
/// The hardware has four kinds of feedback, and folding them into one would force every transport to pretend:
///
/// | Verb | What it carries |
/// |------|-----------------|
/// | `sendValue` | a position: a motor, an Open Stage Control widget, an LED bar |
/// | `sendRing` | a position and the style the ring draws, which the APC40 mk2 (CC 0x18/0x38) and the X-Touch MINI (CC 1-8) both carry on a separate CC |
/// | `sendColor` | a color, for an RGB grid, in per-vendor SysEx with no shared structure |
/// | `sendLabel` | text, where the transport has somewhere to put it, such as a scribble strip |
///
/// ## Why the unimplemented three stay
///
/// Everything past `sendValue` has a default, so a transport implements only what its hardware can do.
/// OSC overrides one method, a GPIO surface lights an LED, and an APC transport overrides the color verbs.
/// OSC and MIDI are the transports today, and both override the value verb only, so the other three have no implementer yet.
/// They are kept because their shapes come from the hardware rather than a guess, and a default each means a transport that cannot do one writes no code for it.

#include <cstdint>

namespace mm {

/// Which bank a surface control belongs to, so a kind plus an index names exactly one control.
enum class SurfaceControl : uint8_t { Switch, Encoder, Fader, Pad };

/// What an LED ring draws as well as where it points, a pan ring filling from the center and a volume ring from one end.
enum class RingStyle : uint8_t { Single, Volume, Pan, Fan, Off };

/// One attached surface, implemented by a transport and called when the state it mirrors changes.
class ControlSurface {
public:
    /// Destroyed through this interface, since ControlModule holds surfaces by base pointer.
    virtual ~ControlSurface() = default;

    /// A control's position changed, `value` being 0..255, the unit every surface control uses.
    virtual void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) = 0;

    /// An encoder's ring: where it points and what it draws, defaulting to the position alone.
    virtual void sendRing(uint8_t index, uint8_t value, RingStyle style) {
        (void)style;
        sendValue(SurfaceControl::Encoder, index, value);
    }

    /// A pad's color. Does nothing by default: most transports have no color at all.
    virtual void sendColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
        (void)index; (void)r; (void)g; (void)b;
    }

    /// Several colors at once, `rgb` being `count` RGB triples, defaulting to a `sendColor` loop.
    virtual void sendColors(const uint8_t* rgb, uint8_t count) {
        if (!rgb) return;
        for (uint8_t i = 0; i < count; i++)
            sendColor(i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }

    /// A name for a control, where the transport can show one. Does nothing by default.
    virtual void sendLabel(uint8_t index, const char* text) { (void)index; (void)text; }
};

/// @}
}  // namespace mm
