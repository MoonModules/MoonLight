# Backlog — mixed (core + light)

Forward-looking items whose work genuinely spans **both** the core and light domains — a core mechanism interacting with a light driver/effect/modifier, where assigning it to one side would misrepresent it. Core-only items are in [backlog-core.md](backlog-core.md), light-only in [backlog-light.md](backlog-light.md). Index + overview: [README.md](index.md).

## Cross-domain

### Switching a MoonLive script from the card fails intermittently (2026-09-29)

Picking a second script for a `MoonLiveEffect` leaves the module running the first one, with nothing reported: the card shows the new name, the effect keeps rendering the old one.
Measured over six consecutive picks on the desktop: two switched, four did not, and the fourth failure recovered on its own.

**What is established.** The file is on the device and complete (`noise.mle`, 560 bytes, `/.moonlive`).
The first pick after the card opens usually works, and later ones fail in runs rather than randomly.
The picker dialog opens on a failing attempt, so the failure is inside row selection rather than in reaching the widget.
No alert fires, which is why the UI looks like it simply did nothing.

**The strongest lead.** An editor pane is present after the first pick, and the change handler does `await editor.save()` and refuses to switch while the pane is still dirty.
That path is unproven: the next step is to instrument the handler and read `editor.isDirty()` on a failing pick.

**Ruled out.** The duplicated download logic in the dropdown, which called `mlDownloadScript` directly instead of the shared `mlEnsureLocal`.
That was a real defect and is fixed, pinned by `test/js/ui-script-picker-download.test.mjs`, and it did not change this symptom.

**Worth deciding alongside the fix**: whether the script control should be editable at all, or whether a scripted module's script is chosen once through the module picker.
The module picker path works reliably, so a read-only control would remove the failing surface rather than repair it.


### LightsControl: not building it (decided 2026-09-01)

The idea was one module owning global light state (master on/off, brightness, palette, bpm) and
acting as the integration point for external controllers, modelled on MoonLight's
`ModuleLightsControl`. **It is not being built**, and the two reasons are worth keeping:

**The generic ControlModule is already the hub.** It carries the surface (switches, encoders,
faders, pads) that every transport writes: OSC, MQTT, the WLED bridge, the web UI, and now the
physical inputs. A second hub beside it would be the split brain the OSC plan forbids.

**A global light param belongs to the module that uses it.** `palette` lives on `Drivers` because
that is where it is consumed (effects read it through `Palettes::active()`), and the same holds for
`brightness` and `on`. Moving them into a hub separates a value from its consumer and buys nothing:
encapsulation, not centralisation.

So the surface reaches INTO those modules rather than owning their state, which is already how it
works: `fader1` targets `Drivers.brightness`, `switch1` targets `Drivers.on`, and `encoder1` targets
`Drivers.palette`. Those bindings are hardcoded today and become user-assignable as a UI plus
persistence job (see [power-functions-analysis-top-down.md](power-functions-analysis-top-down.md),
per-control assignment).

**What an encoder bound to a select needs:** the option NAME under the knob, not the index, so
turning through palettes reads "Rainbow" rather than "37". One rule in the generic UI (an encoder
bound to a select renders its target's option label) rather than per-module code, which is how the
rest of the UI already works. A `uint8_t` encoder covers 256 options, comfortably past the palette
count, and whether it wraps or clamps at the ends follows the bound control's type: a palette ring
wraps, a brightness does not.

### Light presets — save / load / loop a whole effect configuration

MoonLight's preset system (part of `ModuleLightsControl`): **64 named slots**, each capturing the *complete* effect-tree configuration (all module control values), with save / load / delete, **preset looping** on a timer (rotate between a first/last slot), and Home-Assistant registration for external recall. The product owner has flagged this as **definitely needed**.

**Partly shipped since this was written.** `ControlModule` owns the preset system: a pad grid of slots, save and apply, and `capture` controls that choose which subtrees a save includes. So the home question is settled (the surface, not a hub), and what remains from the description above is **preset looping** on a timer, rotating between a first and last slot, plus Home Assistant registration for external recall. Persistence already round-trips control values, so the mechanism is there; the loop timer is the missing piece. Complements the [presets UI note](backlog-core.md) (control-value bundles).

### MultiplyModifier mapping-LUT memory at large grids (investigation, re-verify on classic)

`scenario_perf_full` on the S3 (2026-06-17) measured the MultiplyModifier's cost across grid sizes. The finding, stated correctly: the modifier **reduces compute** (with the default 2×2 kaleidoscope the effect renders only the ¼-size logical quadrant — Noise+Multiply at 16K is 29,647µs vs 50,555µs for Noise alone), and its real cost is **memory** — the mapping LUT's destinations array. Measured modifier heap cost on the S3: 16²→1.7KB, 32²→10.8KB, 64²→23.5KB, **128²(16K)→93KB** (`nrOfLightsType` is `uint32_t` on a PSRAM board). On the S3's 8MB PSRAM this is trivial. Under the physical→logical fold build each physical light contributes ≤1 destination, so the destinations array is bounded by the real light count regardless of chain depth — there is no build-time fan-out.

**This is NOT a no-PSRAM blocker** — 16K Noise + Multiply has run on a classic ESP32 (no PSRAM, 320KB internal) before at **10–20 FPS** (WiFi vs Ethernet), sending frames out over **ArtNet to a display, not physical LED drivers**. It works there because classic's `nrOfLightsType` is `uint16_t` (half the LUT size) and the modifier shrinks the logical render grid. So the action is **re-verify the working classic setup when a classic board is connected** (find the config — grid, mirror, ArtNet target — that reproduces the historical 10–20 FPS), not "fix an impossibility." Worth investigating only if that re-verification shows the LUT memory has regressed since: the destinations array is the obvious lever (it stores a `nrOfLightsType` per physical destination; a 2× kaleidoscope is 1:1 in *count* so the LUT need not store fan-out > the physical count — confirm it isn't over-allocating to `maxMultiplier()` when the effective fan-out is 1). Capture the classic numbers into performance.md's multi-board table first.

### NoiseEffect simplex cost on ESP32 (investigation)

With mirror XY at 128×128, NoiseEffect renders the 64×64 logical quadrant in **~11 ms/tick** on the Olimex (measured) — the simplex math dominates, since the Xtensa LX6 has no FPU and float math is software-emulated. (RainbowEffect on the same pipeline is much cheaper.) This is correct, non-degraded behaviour; it's only worth revisiting if a deployment needs Noise faster than ~11 ms at this grid.

Worth investigating if so:

- **Q16 fixed-point simplex** instead of float (kills the software-float emulation cost).
- **Lower-precision hash** — current simplex uses a 256-entry permutation lookup; a smaller / SIMD-friendly hash may be faster on Xtensa.
- **Strided sampling + interpolation** — render at 32×32, bilinear up to 64×64. Visual quality cost; needs A/B comparison.
- **Inline / unroll the inner per-pixel loop** to keep the simplex state in registers.

None of these are obviously free, and a fixed-point port may shift the visual signature. Defer until there's a real use case — on the no-PSRAM Olimex at large grids the tick is dominated by the synchronous ArtNet send (~35 ms), not Noise, so the effect is rarely the bottleneck there.

**S3 render-only data point (2026-06-17, `scenario_perf_full`):** on the PSRAM S3 with **no output driver**, Noise is the dominant cost at every grid and there's no ArtNet floor to hide it: 16²→738µs, 32²→2,831µs, 64²→11,235µs, **128²(16K)→50,555µs (~20 FPS)** — clean ~linear-in-pixels (67×), so no fragmentation/realloc pathology, just raw simplex compute. The light effect (Checkerboard) on the same sweep is 6–11× faster (16K→7,949µs, ~128 FPS). So on a PSRAM board the heavy effect IS the 16K bottleneck (where on the Olimex the network send was). This is the strongest case for the fixed-point/strided-sampling ideas above, since a PSRAM board can run 16K grids that the network-bound Olimex never reaches. The S3 has a real FPU (LX7), so the win is less about software-float emulation and more about per-pixel simplex work; profile before committing.
