#pragma once

#include <cstdint>

namespace mm {

/// One player's seat in a self-playing game: an input takes it, and the game plays it again once the input goes quiet.
///
/// A game's player controls are ordinary controls, so the control surface, the input services and the interface all reach them.
/// The game cannot tell those writers apart, and needs not: any write through `setControl` is a person playing.
/// Ten seconds without one hands the seat back, so an installation never stands idle waiting for a player who left.
/// Time is the caller's frame clock, so a seat costs no clock read of its own.
struct PlayerSeat {
    /// How long a seat stays a person's after their last input.
    static constexpr uint32_t kHoldMs = 10000;

    /// An input arrived at `nowMs`, so a person holds the seat from then.
    void touch(uint32_t nowMs) {
        until_ = nowMs + kHoldMs;
        touched_ = true;
    }

    /// Whether a person holds the seat at `nowMs`, the hold ending ten seconds after their last input.
    bool held(uint32_t nowMs) const { return touched_ && static_cast<int32_t>(until_ - nowMs) > 0; }

private:
    uint32_t until_ = 0;
    bool touched_ = false;
};

}  // namespace mm
