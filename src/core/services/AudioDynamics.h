#pragma once

#include <cmath>     // the level is a logarithm
#include <cstddef>
#include <cstdint>

namespace mm {

/// The smallest value of a sliding window, kept as two half-window minima: one window's memory in two floats.
struct WindowMinimum {
    float current = 0.0f;    ///< the smallest in the half window running now
    float previous = 0.0f;   ///< the smallest in the half window before it
    float elapsedMs = 0.0f;  ///< how far the running half window is
    bool  primed = false;    ///< whether a first value seeded both halves

    /// Take one value `dtMs` after the last and return the window's minimum.
    float update(float v, float dtMs, float halfMs) {
        if (!primed) { current = previous = v; primed = true; }
        if (v < current) current = v;
        elapsedMs += dtMs;
        if (elapsedMs >= halfMs) { previous = current; current = v; elapsedMs = 0.0f; }
        return current < previous ? current : previous;
    }
};

/// The audio's dynamics as four quantities kept apart: the level, the floor under it, the gate above the floor, and the top of the display window, so silence is dark, a quiet passage stays quiet and a loud one fills the rig.
///
/// @attribution troyhacks, softhack007
///
/// @technical
///
/// ## Four quantities, none reading its own output
///
/// The **level** is each block's RMS in dBFS, a full-scale sine reading 0. The **floor** is the quietest block of the last 10 to 20 s, minimum statistics (Martin 2001): it follows a room growing noisier in any step, forgets a dip, and music lifts it only to the song's own quietest block.
///
/// The **gate** opens and closes as a studio gate does: fast to open, slow to close, with hysteresis. The **top** follows the loudest band while there is sound over the close threshold and holds still otherwise; the level keeps a top of its own, since a block's RMS sits over its loudest band.
///
/// ## One window, capped by the room
///
/// Every band maps through the 40 dB window under the top. The window's bottom never sinks below the bands' own floor, the quietest loudest band, since a band's power sits under the block's RMS. Near the floor the window narrows rather than the top rising, so the noise maps to nothing however long the music pauses, and no lift setting is needed.
struct AudioDynamics {
    /// How far above the floor the gate opens, in dB.
    float squelchDb = 10.0f;

    float levelDb = -120.0f;   ///< this block's RMS, dBFS
    float floorDb = -120.0f;   ///< the room's noise under the music, dBFS
    float bandFloorDb = -120.0f;   ///< the loudest band's floor, the bands' window bottom, dBFS
    float topDb = -120.0f;     ///< the bands' display window's top, dBFS
    float levelTopDb = -120.0f;   ///< the level's own window top, since a whole block's RMS sits over its loudest band
    bool  gateOpen = false;    ///< whether there is sound to show

    static constexpr float kWindowDb = 40.0f;          ///< the display window's span under the top
    static constexpr float kHysteresisDb = 4.0f;       ///< the gate closes this far under where it opened
    static constexpr float kFloorHalfWindowMs = 10000.0f;   ///< the floor remembers the quietest block of one to two of these
    static constexpr float kCloseHoldMs = 250.0f;      ///< the level stays under the close threshold this long before the gate shuts
    static constexpr float kAttackMs = 20.0f;          ///< the top's rise to a louder band
    static constexpr float kReleaseMs = 3000.0f;       ///< the top's fall from a quieter one

    /// Forget everything: the next block seeds the floor and the top.
    void reset() { const float squelch = squelchDb; *this = AudioDynamics{}; squelchDb = squelch; }

    /// One block: its RMS level and its loudest band, both in dBFS, and how long the block took: @xref{four-quantities-none-reading-its-own-output}.
    void update(float blockLevelDb, float loudestBandDb, float dtMs) {
        levelDb = blockLevelDb;
        if (!primed_) { topDb = loudestBandDb; levelTopDb = blockLevelDb; primed_ = true; }
        floorDb = floor_.update(levelDb, dtMs, kFloorHalfWindowMs);
        bandFloorDb = bandFloor_.update(loudestBandDb, dtMs, kFloorHalfWindowMs);
        // The gate: open above the squelch, closed a hysteresis lower after the hold.
        const float openAt = floorDb + squelchDb;
        if (levelDb >= openAt) { gateOpen = true; belowMs_ = 0.0f; }
        else if (gateOpen && levelDb < openAt - kHysteresisDb) {
            belowMs_ += dtMs;
            if (belowMs_ >= kCloseHoldMs) gateOpen = false;
        } else belowMs_ = 0.0f;
        // The top: following only while there is sound over the close threshold, so a pause, the gate's hold included, leaves it where the music had it.
        if (gateOpen && levelDb >= openAt - kHysteresisDb) {
            topDb += (loudestBandDb - topDb) * follow(dtMs, loudestBandDb > topDb ? kAttackMs : kReleaseMs);
            levelTopDb += (levelDb - levelTopDb) * follow(dtMs, levelDb > levelTopDb ? kAttackMs : kReleaseMs);
        }
    }

    /// A band's value in dBFS through the bands' window, 0 while the gate is shut.
    uint8_t toByte(float db) const { return gateOpen ? position(db, topDb, bandFloorDb) : 0; }
    /// The level in dBFS through its own window, 0 while the gate is shut.
    uint8_t levelToByte(float db) const { return gateOpen ? position(db, levelTopDb, floorDb) : 0; }
    /// Where the gate opens on the bands' scale, at least 1 so a frame tells a measured gate from none, whether open or shut.
    uint8_t gateToByte() const { const uint8_t at = position(bandFloorDb + squelchDb, topDb, bandFloorDb); return at ? at : 1; }

private:
    bool  primed_ = false;
    WindowMinimum floor_, bandFloor_;
    float belowMs_ = 0.0f;   ///< how long the level has been under the close threshold

    /// `db` through the window under `top`, never reaching under `floor`, 0..255.
    static uint8_t position(float db, float top, float floor) {
        const float bottom = std::fmax(top - kWindowDb, floor);   // @xref{one-window-capped-by-the-room}
        const float t = (db - bottom) / std::fmax(top - bottom, 1.0f);
        if (t <= 0.0f) return 0;
        if (t >= 1.0f) return 255;
        return static_cast<uint8_t>(t * 255.0f);
    }

    /// The share of the way a one-pole follower moves in `dtMs` toward its input, for time constant `tauMs`.
    static float follow(float dtMs, float tauMs) { return 1.0f - std::exp(-dtMs / tauMs); }
};

}  // namespace mm
