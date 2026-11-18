#pragma once

/// A control surface is a view of the state ControlModule owns, kept in step in both directions.
///
/// @technical
///
/// ## Why a surface mirrors rather than syncs
///
/// ControlModule owns the state: `switch1..8`, `encoder1..8`, `fader1..8`, and what each drives; a surface holds no copy, so two attach at once and stay in step, a phone running Open Stage Control and a desk on the rack.
///
/// Inbound is no method here: a desk writes through ControlModule's `setValue`, the control path the HTTP API and the UI use, so it gains no privilege and there is no second copy to reconcile.
///
/// ## Feedback is not an echo
///
/// On a motorized desk the motors move because the host sends positions back, and on an Akai APC the pads have no meaning until the host lights them: the surface is often the output device, and this interface drives it.
///
/// ## Why a value, not a color
///
/// The surface sends what a control holds, and the transport turns it into what its hardware shows: a fader's value a motor position, an encoder's a ring, a switch's a button light, a pad's state a color the APC40 picks from its palette.
///
/// So one verb covers every desk, and no hardware detail such as a color or a ring style crosses into ControlModule; `sendDisplay` is the Control card's display line, and `lookApplied` names a look applied here.

#include <cstdint>

namespace mm {

/// Which bank a surface control belongs to, so a kind plus an index names exactly one control.
enum class SurfaceControl : uint8_t { Switch, Encoder, Fader, Pad };

/// One attached surface, implemented by a transport and called when the state it mirrors changes (@xref{why-a-surface-mirrors-rather-than-syncs}), which is how a desk's motors and lights are driven (@xref{feedback-is-not-an-echo}).
class ControlSurface {
public:
    /// Destroyed through this interface, since ControlModule holds surfaces by base pointer.
    virtual ~ControlSurface() = default;

    /// A control's value changed, `value` being 0..255, or a ControlModule::PadState for a preset pad: @xref{why-a-value-not-a-color}.
    virtual void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) = 0;

    /// The Control card's display changed to `text`, for a transport whose desk has a display. Does nothing by default.
    virtual void sendDisplay(const char* text) { (void)text; }

    /// A look, a preset of effects alone, was applied here by name, for a transport that passes it on. Ignored by default.
    virtual void lookApplied(const char* name) { (void)name; }
};

}  // namespace mm
