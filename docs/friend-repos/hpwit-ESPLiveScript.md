# hpwit/ESPLiveScript: monthly activity digest

What landed on [hpwit/ESPLiveScript](https://github.com/hpwit/ESPLiveScript), month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

The library: Yves Bazin's (hpwit) C-like compiler/interpreter for the ESP32, small scripts (e.g. LED effects) compiled and run live on-device without a full recompile-and-flash cycle. Summarised via the GitHub commits API.

**Branch note:** `main` is quiet (last touched June 2025), but this repo develops on a long series of **version branches** (`v2`…`v4.3`, plus `vjson`/`vjson2`/`vdrop`/`memory*`), and that's where the recent work is. The activity below is read across those branches, not just `main`.

## August 2026

No activity. Nothing landed on `main` in August 2026, and nothing moved on any of the 37 other branches either, including the version branches where the work normally happens. The newest of those, `vjson2`, last moved on 2026-02-15; `main` last moved in June 2025.

No issues were opened or closed. No release; the most recent is 1.3.2 from February 2025.

_Checked: `repos/hpwit/ESPLiveScript/commits?sha=main` for 2026-08-01..2026-09-01 (0), and the same window on all 38 branches individually, version branches included (0 on every one); releases published (none); issue search `repo:hpwit/ESPLiveScript is:issue created:2026-08-01..2026-08-31` and the same with `closed:` (0 results each)._

## July 2026

No user-facing activity: no commits on `main` **or any of the 38 version branches** (v2.x/v3.x/v4.x, `vjson`/`vjson2`/`vdrop`, `dev`, `mem*`) in July 2026, and no notable issues. (Latest commit on `main` predates the window, June 2025; the newest commit anywhere is `vjson2`, February 2026.)

_Checked: commits author-dated 2026-07-01..2026-07-31 on `main` and every one of the 38 branches, 0 on each; issues created / closed / updated 2026-07-01..2026-07-31 (0 each); PRs created in-window (0); no versioned release published in July 2026._

## June 2026

No user-facing activity: no commits on `main` **or any of the ~30 version branches** (v2.x/v3.x, dev, mem*) in June 2026, and no notable issues. (Latest commit on `main` predates the window, June 2025.)

_Checked: commits author-dated 2026-06-01..2026-06-30 on `main` and every version branch (`v2`…`v3.3`, `dev`, `mem2`…`mem4`, `memory`), 0 on each; issues created / closed / updated 2026-06-01..2026-06-30 (0 each); PRs created in-window (0); no versioned release published in June 2026._

## February 2026

*Latest in-window activity, on the `vjson2` branch.*

- **JSON exchange refinements** (`vjson2`), continued work on the script↔host JSON path begun in mid-2025, plus a code-refactoring cleanup pass.

## March 2025 (and earlier 2025, on the `v4.x` branches)

*The `v4` → `v4.3` line, Jan–Mar 2025.*

- **New HSV function** with a FastLED-style example; register-variable handling and array management improved for speed.
- Documentation + interrupts work; assorted bug corrections. This is the most substantial recent feature stretch.

## Mid-2025 baseline (`main`, June 2025, tag `1.3.2`)

- A **JSON option/return path** (`enjoy json`) so scripts can exchange JSON values with the host firmware; interpreter speedups; a "does this function exist?" pre-call check; `void*` parameter support.

*No `main`-branch commits fall in the Sept 2025 – May 2026 window; the in-window work lives on `vjson2`. This file gets a new `## <Month Year>` section as either `main` or the active version branch sees further commits.*
