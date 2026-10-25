# Plan: layer compositing, masks and transitions

Three requests turn out to be one mechanism. A user asks for a mask that grows any effect in from one end of a strip with a soft glowing front (Macho_roller on Discord, 2026-10-11). Effects need transitions, the old one fading out while the new one fades in. Layers have only two blend modes, alpha and additive. All three are compositing: how two images combine into one, per light. This plan builds one compositing stage that serves all three, by the textbook, in three steps that each ship on their own.

## The textbook

- **Separable blend modes**, as the W3C *Compositing and Blending Level 1* specification defines them: a per-channel function B(source, backdrop), then the result weighted by the layer's opacity, `result = lerp(backdrop, B(source, backdrop), opacity)`. The same set appears in every image editor. The modes, by their specification names: normal, multiply, screen, overlay, darken, lighten, color-dodge, color-burn, hard-light, soft-light, difference, exclusion. Porter and Duff's *plus* is our additive.
- **A mask (matte)** is a per-pixel weight on that combination. A luminance mask is the multiply mode: white keeps, black hides, grey dims.
- **A transition** is, in video terms, a dissolve (a crossfade, one opacity moving over time) or a wipe (a matte whose edge moves over time). Both are compositing with a parameter that changes per frame.

## The pipeline today

Per frame and per layer, effects draw into the layer's buffer in child order, the modifiers fold coordinates (once at mapping time, or per frame through `modifyLive`), and then the Drivers container composites every enabled layer into the physical frame through [BlendMap](../../../src/light/layers/BlendMap.h): the bottom layer overwrites, every layer above blends with its `blendMode` (alpha or additive) and `opacity`. Then brightness, correction and output.

Alpha is already `lerp(backdrop, source, opacity)`, the specification's normal mode, so the existing stage has the right form: new modes add a function, not a stage. Modifiers stay geometry: they decide where a color comes from, never how bright it is.

## The steps

1. ✅ **Blend modes.** Built: multiply, screen, darken, lighten, difference and exclusion, appended to `blendMode` so saved indices keep their meaning; overlay, hard-light and soft-light wait for their measured cost. Extend `BlendOp` with the separable set, multiply first, then screen, darken, lighten, difference and exclusion, and overlay, hard-light and soft-light if their cost holds. Each mode is one per-channel function with one specialized loop, chosen once per layer as today, integer math only. Opacity applies to every mode through the lerp. A multiply layer above any effect is then a mask over everything below it, soft-edged when the mask effect draws a soft edge. Tests pin each mode's formula at its corner values (0, 128, 255) and at partial opacity.
2. ✅ **Color reads for effect scripts.** Built: `getRGB` (the light's color as an `RGB`, as `paletteRGB` answers a palette color), and `mulRGB`, `addRGB` and `blendRGB`. MoonLive effects can write a light but not read one. `getRGB`, plus `addRGB` (a saturating add), `mulRGB` (scale each channel by 0 to 255) and `blendRGB` (mix toward a color by an amount), let a script read what is already in the buffer. A mask effect placed after the main effect in the same layer then masks that layer only, since effects in a layer draw in order on one buffer; trails, glow and blur scripts gain the same reads. The names follow the existing `setRGB`.
3. ⏳ **Transitions.**
   - A per-layer opacity ramp: a target and a duration, applied live, interpolated per frame. A layer fading in or out is the ramp on its opacity.
   - A dissolve between effects: on an effect change, the old effect keeps running in a temporary layer under the new one while the new layer's opacity ramps up; the temporary layer goes when the ramp ends.
   - A wipe: the same, with a mask layer whose edge moves instead of a uniform opacity, so every wipe is soft-edged for free.
   - The cost, only while a transition runs: one extra layer buffer, one extra effect tick and one extra blend pass per frame. The single-layer fast path (the driver reading the layer buffer directly) pauses while two layers exist, so the memory degradation rules decide what a classic ESP32 without PSRAM does.

## Coverage: a layer blends only where it has lights

A layer's mapping decides which lights it reaches, and the compositing pass visits only those. A layer narrowed by a region modifier therefore blends inside its region only, in every mode: outside it the layers below show unchanged, the textbook reading that a layer's coverage is zero where it has no pixel. A multiply mask on a region masks that region and leaves the rest as it was. The bottom layer clears the whole frame first, so a light no layer reaches stays black.

## Cost

| Step | Flash | Memory | Hot path |
|--|--|--|--|
| 1. Blend modes | Two loops per mode (dense grid and mapped), measured per target at the commit | None: no buffer, the mode table is constant | Unchanged for overwrite, alpha and additive (alpha keeps its arithmetic in the shared loop); a layer that picks a new mode pays one multiply or compare per channel, the order of alpha |
| 2. Script color calls | Three small call handlers | None | Only a script that calls them: one call per light, as `setPaletteColor` |
| 3. Transitions | The ramp type and the layer lifecycle | During a transition only: one more layer buffer (channels per light times lights, 36 KB at 12,288 RGB lights) and the old effect's own state | During a transition only: one more effect tick and blend pass per frame, and the single-layer fast path pauses; nothing once it ends |

## Relation to the boot soft start

The 5-second soft start after boot raises the global brightness that the correction tables are built from. It belongs where it is: it protects the power supply, so it must cover the whole output whatever the layers, their modes or safe mode do, and baked into the tables it costs nothing per light. A layer opacity ramp is a creative control inside the composition and cannot give that guarantee: with two layers, ramping one leaves the other at full. What the two share is the primitive, a value moving to a target over a duration, so step 3's ramp and the soft start use one small ramp type rather than two hand-rolled interpolations.

## Not chosen

- **Color access in modifiers** (a per-light color callback in the live pass): it makes a modifier two concepts, geometry and color, and serves neither blend modes nor transitions.
- **A mask slot per layer** (a dedicated mask child and buffer): it implements the multiply function a second time in a second place. Step 1 covers a mask over the stack, and step 2 covers a mask on one layer with no new mechanism. It returns only if a mask must apply to one layer in the middle of a stack and the order of layers cannot express it.

## Open questions

- Which modes beyond the built six earn their flash, measured per mode on a classic ESP32.
- How the UI offers a transition: a duration and a style on the Effects container, or per layer. **Recommended (2026-10-25):** on the Effects container, one duration and one style, as a lighting console has one fade time per playback; per layer only once someone asks for it.
- How motion bytes combine across layers. Opacity and blend modes are color operations, and a pan or tilt byte is a position. In a dissolve, do the heads crossfade their aim, or does the top layer take it outright? Today an upper layer's motion bytes blend like color; the bottom layer's pass through unfaded. **Recommended (2026-10-25):** latest takes precedence (LTP), the console rule for position channels: the top layer's aim wins, and in a dissolve the heads move from the old aim to the new over the fade, as a cue fade moves them. A position is not a color, so it is not blended like one.
- Whether the additive mode takes the specification name *plus*, now that the list follows the specification. **Recommended (2026-10-25):** yes, `plus`, the specification's own operator.

## Inspiration

WLED's segment blending and transitions, studied in its upstream and in a developer fork: segments render into their own buffers and a compositing pass blends them, the same shape MoonLight's layers already have. Its transitions copy the whole segment and render the old and new effect side by side, with hard-edged clip shapes for the wipes. This plan takes the shape from the textbook rather than from that code: nothing is named or structured after it.

## The Discord reply, once step 1 ships

> Put your effect on one layer, and a second layer above it with blend mode **multiply** and a mask effect: white where the glow has reached, a soft ramp at the front, black beyond it. Multiply keeps what is under white, dims under grey and hides under black, so it works with any effect below it. The mask is a few lines of MoonLive, with `t` moving the front.
