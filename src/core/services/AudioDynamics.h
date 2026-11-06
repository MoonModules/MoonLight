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
/// The **gate** opens and closes as a studio gate does: fast to open, slow to close, with hysteresis. Under the close threshold it shuts after 250 ms once the music has left the window, its loudest band over 30 dB under the top, as stopped music does.
///
/// Otherwise it shuts after 3 s, so a soft passage near a floor that rose into the music keeps its bars, and a room whose noise sits within the window still goes dark.
///
/// The **top** follows the loudest band while there is sound over the close threshold and holds still otherwise; the level keeps a top of its own, since a block's RMS sits over its loudest band.
///
/// ## One window, capped by the room
///
/// Every band maps through the 30 dB window under the top, a hardware analyzer's scale range and Troy's, so the bars show the top of the music with its contrast.
///
/// The window's bottom never sinks below the room's bands' floor, learned only while the gate is shut, so the noise maps to nothing however long the music pauses, while a song playing longer than the floor's window does not narrow the window from under its bars. The gate's own floor learns from every block, so a soft passage keeps the gate open.
struct AudioDynamics {
    /// How far above the floor the gate opens, in dB.
    float squelchDb = 10.0f;

    float levelDb = -120.0f;   ///< this block's RMS, dBFS
    float floorDb = -120.0f;   ///< the room's noise under the music, dBFS; the gate's floor, learned from every block
    float bandFloorDb = -120.0f;   ///< the loudest band's floor the gate sees, dBFS
    float roomBandFloorDb = -120.0f;   ///< the room's bands' floor learned only while the gate is shut, the bands' window bottom, dBFS
    float topDb = -120.0f;     ///< the bands' display window's top, dBFS
    float levelTopDb = -120.0f;   ///< the level's own window top, since a whole block's RMS sits over its loudest band
    bool  gateOpen = false;    ///< whether there is sound to show

    static constexpr float kWindowDb = 30.0f;          ///< the display window's span under the top
    static constexpr float kHysteresisDb = 4.0f;       ///< the gate closes this far under where it opened
    static constexpr float kFloorHalfWindowMs = 10000.0f;   ///< the floor remembers the quietest block of one to two of these
    static constexpr float kCloseHoldMs = 250.0f;      ///< the level stays under the close threshold this long before the gate shuts, the music gone from the window
    static constexpr float kQuietHoldMs = 3000.0f;     ///< or this long, the music still in it
    static constexpr float kAttackMs = 20.0f;          ///< the top's rise to a louder band
    static constexpr float kReleaseMs = 3000.0f;       ///< the top's fall from a quieter one

    /// Forget everything: the next block seeds the floor and the top.
    void reset() { const float squelch = squelchDb; *this = AudioDynamics{}; squelchDb = squelch; }

    /// One block: its RMS level and its loudest band, both in dBFS, and how long the block took: @xref{four-quantities-none-reading-its-own-output}.
    void update(float blockLevelDb, float loudestBandDb, float dtMs) {
        levelDb = blockLevelDb;
        if (!primed_) { topDb = loudestBandDb; levelTopDb = blockLevelDb; primed_ = true; }
        // The gate's floor learns from every block, so a song with no pause lifts it and the gate stays open through soft passages.
        floorDb = floor_.update(levelDb, dtMs, kFloorHalfWindowMs);
        bandFloorDb = bandFloor_.update(loudestBandDb, dtMs, kFloorHalfWindowMs);
        const float openAt = floorDb + squelchDb;
        // The window's bottom learns only while the gate is shut, the room heard without music: under `openAt` alone, a song's soft blocks would teach it the song once the gate's floor rose into the music.
        if (!gateOpen) roomBandFloorDb = roomBandFloor_.update(loudestBandDb, dtMs, kFloorHalfWindowMs);
        updateGate(openAt, loudestBandDb, dtMs);
        // The top: following only while there is sound over the close threshold, so a pause, the gate's hold included, leaves it where the music had it.
        if (gateOpen && levelDb >= openAt - kHysteresisDb) {
            topDb += (loudestBandDb - topDb) * follow(dtMs, loudestBandDb > topDb ? kAttackMs : kReleaseMs);
            levelTopDb += (levelDb - levelTopDb) * follow(dtMs, levelDb > levelTopDb ? kAttackMs : kReleaseMs);
        }
    }

    /// A band's value in dBFS through the bands' window, 0 while the gate is shut.
    uint8_t toByte(float db) const { return gateOpen ? position(db, topDb, roomBandFloorDb) : 0; }
    /// The level in dBFS through its own window, 0 while the gate is shut.
    uint8_t levelToByte(float db) const { return gateOpen ? position(db, levelTopDb, floorDb) : 0; }
    /// Where the gate opens on the bands' scale, the one `toByte` draws on, at least 1 so a frame tells a measured gate from none, whether open or shut.
    uint8_t gateToByte() const { const uint8_t at = position(bandFloorDb + squelchDb, topDb, roomBandFloorDb); return at ? at : 1; }

private:
    bool  primed_ = false;
    WindowMinimum floor_, bandFloor_;          ///< the gate's floors, fed every block
    WindowMinimum roomBandFloor_;              ///< the window's bottom, fed only while the gate is shut
    float belowMs_ = 0.0f;   ///< how long the level has been under the close threshold

    /// The gate: open above the squelch, closed a hysteresis lower after its hold, the shorter once the music has left the window.
    void updateGate(float openAt, float loudestBandDb, float dtMs) {
        if (levelDb >= openAt) { gateOpen = true; belowMs_ = 0.0f; return; }
        if (!gateOpen || levelDb >= openAt - kHysteresisDb) { belowMs_ = 0.0f; return; }
        belowMs_ += dtMs;
        const bool leftWindow = loudestBandDb < topDb - kWindowDb;
        if (belowMs_ >= (leftWindow ? kCloseHoldMs : kQuietHoldMs)) gateOpen = false;
    }

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
