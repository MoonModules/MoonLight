# LED signal integrity: flicker on LEDs that should be off

Random wrong colors on LEDs the effect leaves black, most often a few stray pixels flickering, is, on a 3.3 V ESP32 driving WS2812 **directly**, almost always a **data-line signal-integrity** problem, not a firmware bug. WS2812 wants a logic-high near 0.7 × VDD (≈ 3.5 V on a 5 V strip), but the ESP32 drives only 3.3 V, so individual bits sit at the margin and noise tips them.

Confirm the firmware is innocent **before** reaching for the soldering iron. These checks are the bench diagnosis path (recorded in [lessons.md](../work/past/lessons.md)):

Steps: [Diagnose dark or flickering lights](../moonwalk.md#diagnose-lights).

When the three checks come back clean, the fix is electrical, in rough order of effectiveness:

- **Add a 3.3 → 5 V level shifter** on the data line (e.g. 74HCT125 / 74AHCT125), the single most effective fix; it restores the logic-high margin the LEDs expect.
- **Add a ~330 Ω series resistor** at the GPIO, close to the board, to damp reflections.
- **Shorten / shield the data wire**, and keep it away from the power leads and the antenna.
- **Share a solid, thick common ground** between the strip's supply and the board.
- If RF coupling was implicated by the TX power check, set a per-board `Network.txPowerSetting` cap (the same `deviceModels.json` mechanism the ESP32-S3 N16R8 Dev uses).
