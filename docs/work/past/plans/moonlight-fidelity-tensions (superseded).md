# MoonLight migration — fidelity tensions

> **Superseded on 2026-09-22** by [Plan-20260922 - MoonLight, from v5.0.0 to the rename](../../present/Plan-20260922%20-%20MoonLight,%20from%20v5.0.0%20to%20the%20rename.md), which consolidates the five MoonLight files into one. Kept for the reasoning behind decisions already taken.

A running log of places where **strict fidelity to MoonLight's behaviour** (the migration mandate:
end users must see the same effect they always have) collides with a **MoonLight principle**
(robustness / no-crash-at-any-grid-size, correctness, hot-path discipline, *common patterns first*).

For each: what MoonLight does, what the principle wants, what was shipped, and the **decision needed**.
The product owner resolves these — the agent records them rather than silently choosing.

Status legend: 🟡 open (needs PO decision) · 🟢 resolved (decision recorded) · ⚪ accepted-as-is (kept faithful, no change wanted)

---

## 1. 🟢 GEQ3D — `cols / NUM_BANDS` narrow-grid collapse — RESOLVED (2026-07-01)

Resolved per the "same UX, improvements allowed" rule: **clamp the drawn band count to the column
count** so bars spread instead of piling at x=0 on a narrow grid. Invisible on normal grids
(cols ≥ numBands → no-op), so no fidelity loss where it matters. See
[moonlight-improvements.md](moonlight-improvements (superseded).md). (GEQ — the flat 2D one — was *not* affected:
it maps each column to a band, so it never had the collapse.)

## 2. 🟢 GEQ3D — frame-counter sweep → time-based — RESOLVED (2026-07-01)

Resolved: converted the projector sweep to a **time-based triangle wave** (`triwave8(beat8(...))`),
so `speed` means the same on every device (frame-rate-independent). Not throttled — a fast board
renders the same sweep more smoothly, a slow one choppier. Once-per-frame, no per-pixel cost. See
[moonlight-improvements.md](moonlight-improvements (superseded).md).

---

## 3. ⚪ SolidEffect / general — `scale8` vs integer `*bri/255` rounding

- **MoonLight:** brightness in several effects is plain integer `channel * brightness / 255`
  (truncating), not FastLED's `scale8` (which has a +1 video-rounding so `scale8(x,255)==x`).
- **Principle in tension:** MoonLight's standard channel-scale op is `scale8`. Using it would be the
  "common patterns first" choice, but it rounds ~1 LSB higher than MoonLight at non-255 brightness.
- **Shipped:** kept faithful — used MoonLight's exact `* bri / 255` where the source does (the 3a
  verifier caught and corrected a `scale8` slip in SolidEffect). At brightness=255 (default) both
  agree; the difference is sub-perceptual at other values. Accepted as-is (faithful), recorded so we
  know the rule: **match MoonLight's exact rounding per effect, don't reflexively swap in scale8.**

## 4. 🟡 Audio effects — `volume` normalization scale (0..1 float vs our 0..255 `level`)

- **MoonLight:** audio effects read `sharedData.volume` as a normalized float (roughly 0.0..1.0+),
  e.g. FreqMatrix's gate `volume > 0.25`. MoonLight's `AudioFrame::level` is a small integer
  (~0..255, the VU value).
- **Principle in tension:** fidelity needs the *same trigger points* (a beat that lights MoonLight
  must light ours), but the units differ, so a literal `> 0.25` is wrong against `level`.
- **Shipped (3d batch):** the ports scale the thresholds proportionally (e.g. `0.25` of full-scale →
  `~64` on the 0..255 `level`) and flag each with a comment. This is a **reconstruction**, not a
  verbatim match — the exact trigger point depends on how our AudioService scales `level` vs how
  MoonLight scales `volume`.
- **Decision needed:** confirm the level-scale mapping on hardware (does our `level` reach ~255 at
  the same loudness MoonLight's `volume` reaches ~1.0?), and whether `volumeRaw` (which we don't
  have separately — NoiseMeter uses it) needs a real raw value added to AudioFrame, or `level` is a
  good-enough stand-in. **PO call after bench.**
- **Synth-audio bench (2026-07-01, P4 mic-less + `simulate=music`):** under a known-good synthesized
  frame the FreqMatrix gate (`peakHz > 80 && levelSmoothed > 64`) opens on **398/400** frames, so the
  *threshold and code path are correct* — a synth signal lights the effect. This isolates the open
  question to **real-mic scaling only**: whether a live INMP441 `level`/`levelSmoothed` reaches the
  same ~64+ the synth does at the loudness a user calls "music playing". The earlier "FreqMatrix/Blurz
  show nothing on the S3 with music" report is therefore a *mic gain/scale* question (does the real
  `gain`/`floor` bring the signal over the gate), not an effect bug. **Cross-check on the S3 with the
  synth as the reference: if the synth lights it and the real mic doesn't, raise `gain` / lower the
  gate, don't touch the effect.**

## 5. 🟢 Audio effects — raw vs smoothed level — RESOLVED (2026-07-01)

The premise was backwards: `AudioFrame::level` was NOT smoothed — `computeLevel` recomputes it raw
per audio block, so it's already WLED's `volumeRaw` (the instantaneous, transient-snapping value).
NoiseMeter using it was correct, not a stand-in. The real gap was the *other* direction: no smoothed
value. Resolved by **adding `AudioFrame::levelSmoothed`** (an EMA of `level` in AudioService) so
effects that want WLED's calm `volume`/`volumeSmth` can read it, and doing an **audio-effect sweep**
to point each effect at the value matching its behaviour: NoiseMeter → raw `level` (unchanged, VU
snaps to beats); FreqMatrix, AudioSpectrum's VU bar, AudioVolume → `levelSmoothed` (breathing/flowing
look). Bands-driven effects (GEQ, GEQ3D, PaintBrush, FreqSaws, Blurz) read per-band magnitudes,
unaffected. See [moonlight-improvements.md](moonlight-improvements (superseded).md).

## 6. 🟡 Reconstructed logic — effects whose MoonLight source was incomplete (cross-check on bench)

The fetched MoonLight source for some effects was partial, so parts were **reconstructed** from the
WLED algorithm + the documented intent and marked `// RECONSTRUCTED` in the code. These render and
are plausible, but the exact behaviour needs a side-by-side bench check vs MoonLight/WLED:

- **Tetrix** — *no full source was available* (only condensed pseudocode). The fall-speed physics
  (`map(speed,1,255,40000,250)` descending, the per-frame drop, the brick-width formula) are
  reconstructed from the spec. (A real bug was caught + fixed: the speed-map result 250..40000 was
  being truncated into a `uint8_t`, wrapping the physics — now widened.) **Cross-check the fall
  cadence vs WLED 2D Tetrix.**
- **FreqMatrix** — the scroll-throttle gate is reconstructed (WLED uses a `micros()`-based 0..15
  "second hand"; the port reproduces the *intent* with an elapsed()-ms period). A **real D1
  dimensionality bug was caught + fixed** (it was writing along X; D1 writes the x=0 column along Y).
  **Cross-check scroll speed + orientation.**
- **Blurz** — two position branches (the `freqMap` log-frequency placement and the band-scan) were
  reconstructed (source incomplete). **Cross-check dot placement vs WLED Blurz.**
- **FreqSaws** — the `targetSpeed = volume * increaser * 257` overflow/scaling behaviour is
  uncertain (`volume*increaser` already exceeds 16-bit before `*257`); reproduced as-described but
  flagged. **Cross-check band-speed response.**
- **GEQ** — the falling-peak (`ripple`) markers and `fadeOut` are reconstructed to the standard WLED
  2D GEQ (the core band→bar math is verbatim-equivalent). **Cross-check peak-dot fall.**
- **NoiseMeter** — the `aux0/aux1` per-frame noise-scroll increments were unspecified in source;
  reproduced with `beatsin8(5/4,...)` (matches WLED mode). Minor; **cross-check drift speed.**
- **BouncingBalls / PacMan / Ant** — had full source; only standard `map()`/`random` helper bodies
  were reconstructed (FastLED-equivalent). Note: `random16(min,max)` was done as the contract's
  modulo form, NOT FastLED's scaled-draw, so the *seeded random sequence* differs slightly (Ant's
  initial velocities) — visually equivalent, not bit-identical.

These are all in the code with `// RECONSTRUCTED` markers; grep for them to find the exact lines.

## Resolved / accepted

_(⚪ entries above are kept-faithful, no change wanted unless the PO revisits; 🟡 entries await a decision.)_
