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
/// `sendLabel` is for text a desk can show, such as a scribble strip, and `sendPress` carries a pad press to the board whose presets the grid shows.

#include <cstdint>

namespace mm {

/// Which bank a surface control belongs to, so a kind plus an index names exactly one control.
enum class SurfaceControl : uint8_t { Switch, Encoder, Fader, Pad };

/// One attached surface, implemented by a transport and called when the state it mirrors changes.
class ControlSurface {
public:
    /// Destroyed through this interface, since ControlModule holds surfaces by base pointer.
    virtual ~ControlSurface() = default;

    /// A control's value changed, `value` being 0..255, the unit every surface control uses; for a preset pad, a ControlModule::PadState.
    virtual void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) = 0;

    /// A name for a control, where the transport can show one. Does nothing by default.
    virtual void sendLabel(uint8_t index, const char* text) { (void)index; (void)text; }

    /// A pad pressed on a board whose grid shows another board's presets, for the transport that reaches that board. Does nothing by default.
    virtual void sendPress(uint8_t pad) { (void)pad; }
};

/// @}
}  // namespace mm
