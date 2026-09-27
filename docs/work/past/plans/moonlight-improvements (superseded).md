# Effect improvements over MoonLight

> **Superseded on 2026-09-22** by [Plan-20260922 - MoonLight, from v5.0.0 to the rename](../../present/Plan-20260922%20-%20MoonLight,%20from%20v5.0.0%20to%20the%20rename.md), which consolidates the five MoonLight files into one. Kept for the reasoning behind decisions already taken.

Where a migrated MoonLight effect **deliberately behaves differently from the MoonLight original** — a
change that *improves* the effect (more correct, smoother, works at more grid sizes, a control that
matches its label) rather than a straight port. The migration mandate is fidelity ("effects look like
MoonLight"), so every intentional divergence is registered here with its reason, so it's a decision on
record, not accidental drift.

Distinct from [moonlight-fidelity-tensions.md](moonlight-fidelity-tensions (superseded).md): that log
holds *undecided* fidelity-vs-principle conflicts awaiting a call; this doc holds *decided* improvements
the product owner approved (correctness/UX wins that ship).

Product-owner ruling (2026-07-01): improvements that increase user satisfaction are allowed even when
they diverge from MoonLight — register each here.

| Effect / primitive | MoonLight behaviour | MoonLight behaviour | Why it's an improvement |
|---|---|---|---|
| `math8::map8` (audio bar heights) | `lo + scale8(in, hi-lo)` — the input top never reaches `hi`, so a one-step span (bar height 1) collapses to 0 | `lo + in*(hi-lo)/255` — the input top reaches `hi` exactly (FastLED's documented `map8 == map(in,0,255,lo,hi)`) | Audio bars reach full height; a 1-row bar is now possible. Matches FastLED's *documented* semantics. Affects every audio effect's bar heights slightly. |
| **FreqSaws** (band timing on wide panels) | Each band's sawtooth physics advanced once per **column**, so a band spanning K columns integrated K× per frame — it ran and decayed ~K× too fast | Each of the 16 bands integrates exactly **once per frame**; the column loop just draws the cached per-band Y | Animation speed/decay no longer depend on panel width — identical on a 32-wide and a 256-wide grid. Matches WLED's per-band-per-frame physics. |
| **SphereMove** (motion smoothness) | `time_interval` did the first divide as **integer** (`ms/(100-speed)`), so the origin/diameter only advanced when that ticked to a new whole number — visible stutter (~20 updates/s at 60 FPS, worse at low speed) | Full expression in float, so the shell sweeps and breathes smoothly every frame | Smooth motion at all speeds instead of discrete jumps. (MoonLight *intended* float here, so this is also more faithful.) |
| **Lissajous** (thin grids) | A 1-wide or 1-tall grid mapped every sample to coordinate **1**, which clips (only index 0 is valid) → blank | The size-1 axis maps to coordinate **0**, so the figure draws | Produces visible output on degenerate/thin grids instead of nothing; normal grids (both axes ≥2) unchanged. |
| **PaintBrush** (large grids) | Oscillator endpoints truncated into `uint8_t`, so on grids >256 per axis strokes swept only a small low corner; the top band/hue were unreachable (off-by-one) | Oscillators generated 0..255 then scaled to the full grid; loop uses `numLines-1` as the map high bound | Strokes span the full grid at any size and use the full palette/band range. ≤256-per-axis grids: identical pixels. |
| **FixedRectangle** (RGBW white channel) | On RGBW with `alternateWhite`, the **W channel was written on every box cell** (tinting colored tiles), and left stale when `white==0` | W follows the checker: only white tiles carry `W=white`; colored tiles clear W to 0 | Colored tiles render as pure RGB instead of RGB+white; no stale W persists. The checker actually alternates color vs white. |
| **GEQ3D** (projector sweep cadence) | The sweep advanced by a per-frame counter (`counter++ % (11-speed)`), so its speed tracked the device frame rate — faster on a 280 FPS board than a 30 FPS one at the same `speed` | Time-based triangle wave (`triwave8(beat8(speed·3, elapsed()))`), so the projector is at the same wall-clock position on every device | The `speed` control means the same thing on every device (frame-rate-independent, the MoonLight convention). A fast board renders the same sweep more smoothly, a slow one choppier — neither is throttled to the other. Once-per-frame calc, no per-pixel cost. |
| **GEQ3D** (narrow-grid bars) | Bar width is `cols / NUM_BANDS`, which truncates to 0 when `cols < numBands` (e.g. 8 cols, 16 bands) → every bar collapses to x=0 | The drawn band count is clamped to the column count, so bar width stays ≥ 1 and bars spread across the available width | Bars render on grids narrower than the band count instead of piling at x=0. Invisible on normal grids (cols ≥ numBands → the clamp is a no-op). |
| **AudioFrame** (raw + smoothed level) | WLED exposes both `volumeRaw` (instant) and `volume`/`volumeSmth` (smoothed); a straight port only had our raw `level` | Added `levelSmoothed` (an EMA of `level`); each audio effect reads the value matching its intent — NoiseMeter uses raw `level` (VU snaps to beats), FreqMatrix / AudioSpectrum VU bar / AudioVolume use `levelSmoothed` (breathing/flowing) | Effects that should glide with the music no longer jitter per audio block, and beat-reactive ones stay snappy — the full WLED raw/smoothed pair instead of one value forced everywhere. |

<!-- Invisible fixes (not listed — no visible change): GEQ/StarSky/PaintBrush overflow guards on huge
grids, Tetrix 49-day millis wrap, GoL 3D OOB read + colorIndex≠0 marker, RubiksCube float→int (identical
pixels), Solid/NoiseMeter/Blurz comment+robustness. The DemoReel extrude improvement was reverted (crash)
and backlogged — see backlog-light.md. -->

<!-- StarField: the blur control was flagged as inverted, but it matches MoonLight (fadeToBlackBy(blur):
higher = stronger fade = shorter streaks). No code change — the doc comment was corrected to state the
real direction. Not an improvement, just a doc fix. -->
