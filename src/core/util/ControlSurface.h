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
/// A desk writes through ControlModule's `setValue`, the same control path the HTTP API and the UI use, so it gains no privilege and there is no second copy to reconcile.
///
/// ## Feedback is not an echo
///
/// On a motorized desk the motors move because the host sends positions back, and on an Akai APC the pads have no meaning until the host lights them.
/// The surface is often the output device, and this interface is how it gets driven.
///
/// ## Why a value, not a color
///
/// The surface sends what a control holds, and the transport turns it into what its hardware shows.
/// A fader's value becomes a motor position, an encoder's a ring and a switch's a button light.
/// A preset pad's state (empty, stored, applied) becomes a color the APC40 picks from its own palette.
/// So one verb covers every desk, and no hardware detail such as a color or a ring style crosses into ControlModule.
/// `sendDisplay` is the Control card's display line, for a desk with a display of its own.

#include <cstdint>

namespace mm {

/// Which bank a surface control belongs to, so a kind plus an index names exactly one control.
enum class SurfaceControl : uint8_t { Switch, Encoder, Fader, Pad };

/// One attached surface, implemented by a transport and called when the state it mirrors changes: @xref{why-a-surface-mirrors-rather-than-syncs}.
/// It is how a desk's motors and lights are driven: @xref{feedback-is-not-an-echo}.
class ControlSurface {
public:
    /// Destroyed through this interface, since ControlModule holds surfaces by base pointer.
    virtual ~ControlSurface() = default;

    /// A control's value changed, `value` being 0..255, or a ControlModule::PadState for a preset pad: @xref{why-a-value-not-a-color}.
    virtual void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) = 0;

    /// The Control card's display changed to `text`, for a transport whose desk has a display. Does nothing by default.
    virtual void sendDisplay(const char* text) { (void)text; }
};

/// @}
}  // namespace mm
