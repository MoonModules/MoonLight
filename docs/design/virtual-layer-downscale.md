# Virtual-layer render and pooling down-map

> A forward-looking design study (backlog, present-tense-exempt). It captures a feature the product
> owner requested as a "hue mixer modifier" and the architecture verification that reframed it into a
> **Layer** feature. Unplanned for implementation; a future increment picks it up. Supersedes
> the earlier `DownscaleModifier-spec.md` draft (deleted), which framed it as a modifier.

## Goal

Run an effect authored for **many** pixels, a virtual 32×32 grid, and **summarize** it onto a
**small** number of physical lights: Philips Hue bulbs, or a handful of PAR/DMX fixtures. Each
physical light shows the **average color of its region of the virtual frame, ignoring black pixels**, so a fire / plasma / GEQ effect reads as a coherent ambient wash across N bulbs instead of N arbitrary single-pixel samples.

Concretely: a `Fire` effect on a 32×32 grid, driven onto 10 Hue bulbs, makes each bulb glow the mean of its ~102-pixel slice of the fire, the flames' *summary*, not one stray pixel.

The mirror case (**physical ≥ virtual**, e.g. a 128×128 wall from a 32×32 effect) is **already solved** by the [1:N mapping](../explanation/architecture/moonlight.md#mapping-and-blending). The genuinely new capability is the **downscale** direction (physical < virtual): render big, summarize small.

## Why this is a Layer feature

The request began as "make a modifier." Two verification steps ruled that out:

1. **Not `modifyLive`.** The per-frame modifier hook is a *backward 1:1 coordinate gather*: the Layer
   walks destination pixels and asks the modifier for **one** source coordinate, then copies **one** pixel (`Layer::applyLivePass`, Layer.h:~218). Pooling *combines* many source pixels into one output value (sum + count, skip black, divide), a computation a coordinate remap structurally can't express (it returns a coordinate, not a color).

2. **Not a modifier at all, the real blocker is render size.** A Layer's logical (render) box
   **starts at the physical light count** and modifiers only ever *shrink* it (`Layer::rebuildLUT`,
   Layer.h:343: `Coord3D box{physicalWidth_, physicalHeight_, physicalDepth_}` → each static modifier reshapes it → `width_ = logical.x`). So a Layer renders at *most* at its physical size. A 32×32 effect onto 10 physical lights **cannot happen**: the effect would render at 10, leaving nothing to pool *from*. A driver-side pool hits the same wall: the driver reads the layer's logical buffer (zero-copy) or the LUT-mapped physical `outputBuffer_` (Drivers.h:~264), both are already physical-sized.

So the enabling feature is a **Layer virtual/render size that can exceed the physical light set**, exactly MoonLight's `VirtualLayer` (a per-layer virtual buffer, render at a virtual size, map to physical), which the Layer doc already cites as prior art (Layer.h:33). Once a Layer can render at a virtual grid independent of N physical lights, "summarize down" is **the virtual→physical down-map rule**. It lives in the Layer / mapping, not a modifier.

## Proposed shape (for a future implementation plan)

Two parts; the first is the enabler, the second is the actual summarize:

### 1. Layer virtual size (the enabler)

A Layer control (or layout property) setting a **render box independent of, and allowed to exceed,** the physical light count. Effects render into that virtual box; the mapping reduces it to the physical lights. Care points a future plan must resolve:

- **`rebuildLUT` assumes logical ≤ physical.** It backward-maps each *physical* light to *one* logical
  cell (a fan-out / scatter). A virtual grid *larger* than physical needs the inverse: many logical cells → one physical light. The [four mapping types](../explanation/architecture/moonlight.md#mapping-and-blending) hold **no N:1-averaged type**: that's the new mapping the feature introduces.
- **Memory:** the virtual buffer is `virtualW × virtualH × virtualD × channels`, which can dwarf the
  physical light count (32×32×3 = 3 KB is fine; larger virtual grids need the same PSRAM-first allocation + degradation-status discipline the Layer already uses).

### 2. The pooling down-map (the summarize)

`physical[i] = mean(non-black pixels of virtual region i)`. Two open sub-designs for the future plan:

- **Where it runs:** a per-frame buffer pass (an N-slot accumulator, `rSum/gSum/bSum/count` per
  physical light, sized on the cold path, zeroed + filled per frame) attached to the **Layer**; OR a new `MappingLUT` N:1-average mapping type that `blendMap` applies. The accumulator approach is simpler and self-contained; the LUT-type approach is more uniform with the existing mapping model.
- **Region assignment (`regionOf`), derived from the physical shape, not a user control.** The split
  follows the *dimensionality of the physical light set* (read from `Layouts` / the physical dims the Layer already knows):
  - **1D physical** (N bulbs in a line): N contiguous bands along the virtual grid's dominant axis.
  - **2D physical** (a panel/cluster, Pw×Ph): a Pw×Ph tiling; each physical light = the mean of its
    tile.
  - **3D physical** (a cube): 3D blocks.

  One integer formula per axis (`region_a = coord_a × physDim_a / virtualDim_a`), no user control, no 1D-vs-2D mode. Add/remove lights or change the physical layout and it re-slices automatically (the
  *No reboot to apply* / robustness guarantees). The shape is *data*, so the general algorithm reading
  physical dims is the textbook construct, cleaner than shipping "1D bands first."

- **Skip black:** an unlit virtual pixel doesn't dilute the average (`count[slot]` only increments for
  non-black pixels; an all-black region → the physical light is off). Integer math only; no heap in the per-frame pass.

### Driver-agnostic

The output is N physical light values, whatever driver reads them (Hue over HTTP, a NetworkSend driver over ArtNet/E1.31 to N DMX PAR channels, an RMT strand of N pixels) shows the summary. The goal's "or a number of PAR lights for that matter" falls out for free.

## Tests (when built)

The pooling math and the `regionOf` split are **pure and host-testable**, wherever they end up living:

- **Unit:** feed a synthetic virtual buffer (gradient, half-black frame) + N; assert each physical
  value equals the mean of its non-black region; all-black region → off; correctness at N=1, N=10, N=(virtual pixel count) = identity, N > pixel count (empty slots).
- **Unit:** `regionOf`, contiguous, complete (every virtual pixel in exactly one region), no
  gaps/overlap, at awkward N (7, 13) and in 1D/2D/3D.
- **Scenario:** a Fire / Noise effect on a virtual grid → summarized onto a fake N-light driver →
  non-zero, stable output; robustness (change virtual size + N live, 0×0 grid) never crashes.

## Why parked

This is a Layer-level feature (a new render-size concept + a new N:1-average mapping), materially bigger than the "quick modifier" it was first requested as. It warrants its own spec + `/plan` + increment rather than being wedged into the modifier framework (where it fought three invariants: the 1:1 gather, free composition, and logical-space operation). Captured here so the design and the architecture findings aren't lost; a future increment picks it up.
