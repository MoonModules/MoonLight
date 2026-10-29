# Backlog: Layouts

Generated from [the Project](https://github.com/users/MoonModules/projects/5) by `moondeck/report/project_mirror.py`: edit the cards, not this file.

## Across modules

### Render larger than the fixture and pool down onto a few lights (Hue, PAR)

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287082192)

A fire or plasma effect on a virtual 32x32 grid driven onto ten Hue bulbs or a handful of DMX PARs, each light showing the mean of the non-black pixels in its region, so the fixture reads as one coherent wash instead of ten stray samples. It is a Layer feature, not a modifier: a modifier's live pass gathers one source coordinate per destination, and the logical box never exceeds the physical light count, so there is nothing to pool from. Two parts: a render box allowed to exceed the physical layout, and an N:1 averaging map beside the four mapping types, with regions cut per axis from the physical dimensions and no user control. The pooling math and the region split are pure and host-testable. The design and the architecture findings: [the downscale study](../../design/virtual-layer-downscale.md).

### Fixture model: moving heads and beams

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287082264)

Today a "light" is a point at a static coordinate with a color. A **moving head** is a fixture that emits a *beam* in a direction it controls live (pan + tilt), plus color, beam-width, etc. — per-light **vector** state, not just color, and a different draw (a cone/ray, not a disc). The static-positions-`0x03` + color-`0x02` split can't express "this fixture's beam now points here." The industry-standard model is **DMX/GDTF fixtures**: a fixture has a position *and* a set of typed attributes (color, pan, tilt, beam). The preview becomes a fixture renderer (disc for a pixel, cone for a beam); this is also the "make Preview a general-purpose module, not light-specific" goal. A domain-model change (the fixture/attribute model), not just transport. Plan when moving heads are actually on the bench.

**Sub-item — per-emitter targets, and fixing the Yellow/UV RGB-synthesis placeholder.** `Correction::apply()` today synthesizes every non-RGB emitter (White, WarmWhite, Yellow, UV) from RGB via the one `whiteMode` gate. That conflates two different physics: **White/WarmWhite are broadband illumination** with a real achromatic basis (`min(R,G,B)` is a sound approximation — warm vs cold differ only in phosphor CCT a byte value can't express), but **Yellow/Amber (~590 nm) and UV (~400 nm) are saturated/out-of-gamut emitters with no honest RGB pre-image.** Yellow's `min(R,G)` stand-in reads greener than a true amber die AND fires on far too much (any red+green content — yellows, whites, skin tones — muddies the fixture); UV's blue-excess is an eyeball hack. Amber is a *real, common, targetable* emitter (RGBA / RGBAW PARs ship a dedicated ~590 nm die), so the right model is an **effect that targets amber DIRECTLY** (a typed attribute), not RGB-and-synthesize. The fixture model is where that lands. **Decision to make when built:** whether Yellow/UV should default to **off** (0, like `whiteMode::None`) until an effect drives them — more honest than always firing a wrong approximation — vs. keeping the "light it up to eyeball the wiring" placeholder. **Practical test to run then:** on a real RGBA/RGBAW+UV PAR, compare the synthesized amber against a directly-driven amber die (does `min(R,G)` look acceptably yellow, or muddy?), and confirm the White subtraction under `Accurate` reads correct with all emitters present. Rationale + the current formulas live in `Correction.h`'s emitter-field comment.

### Mixing light types in one Layouts

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287082337)

Today each layout child describes one light type (all LED strips, or all par lights), and the current model is one Layouts container per light type. Whether a single Layouts should hold mixed types (LED strips + par lights together), and how the per-channel layout would reconcile across them, isn't designed. Deferred until a concrete need forces it; it's adjacent to the fixture model above (a real fixture/attribute model may reframe how mixed types are expressed).
