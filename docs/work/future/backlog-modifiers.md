# Backlog: Modifiers

To-build items for Modifiers, grouped by module, each with its fields on one line. Other areas: [the future index](index.md).

## MultiplyModifier

### MultiplyModifier mapping-LUT memory at large grids

Kind: Investigation · Platform: classic · Trigger: a classic board is connected

The modifier reduces compute and costs mapping-LUT memory, measured per grid in [performance.md](../../reference/performance.md#multiplymodifier-compute-down-memory-up-noise-effect). Under the physical→logical fold build each physical light contributes ≤1 destination, so the destinations array is bounded by the real light count regardless of chain depth, so there is no build-time fan-out.

**This is NOT a no-PSRAM blocker:** 16K Noise + Multiply has run on a classic ESP32 (no PSRAM, 320KB internal) before at **10–20 FPS** (WiFi vs Ethernet), sending frames out over **ArtNet to a display, not physical LED drivers**. It works there because classic's `nrOfLightsType` is `uint16_t` (half the LUT size) and the modifier shrinks the logical render grid. So the action is **re-verify the working classic setup when a classic board is connected** (find the config, its grid, mirror and ArtNet target, that reproduces the historical 10–20 FPS), not "fix an impossibility." Worth investigating only if that re-verification shows the LUT memory has regressed since: the destinations array is the obvious lever (it stores a `nrOfLightsType` per physical destination; a 2× kaleidoscope is 1:1 in *count* so the LUT need not store fan-out > the physical count: confirm it isn't over-allocating to `maxMultiplier()` when the effective fan-out is 1). Capture the classic numbers into performance.md's multi-board table first.

## Across modules

### Retro looks on a picture: scanlines, chroma bleed, tracking glitches (robwomp, 2026-10-21)

Kind: Feature

Asked for on Discord by robwomp, who loves the glitchy analog and digital look of the 90s and 00s and has Rockchip boards with HDMI and CSI inputs.
A captured picture becomes worth watching on lights once it can be degraded on purpose: scanlines, a color channel bleeding sideways, a frame that tears and rolls like a VHS tracking error.
Each is a modifier on what the Picture effect draws, so it works on any source, a capture, a GIF or a browser tab.
Ask robwomp which looks matter most before building, and have them tested on robwomp's capture hardware.
