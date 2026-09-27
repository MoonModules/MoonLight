# hpwit/I2SClocklessVirtualLedDriver: monthly activity digest

What landed on [hpwit/I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver), month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

The library: Yves Bazin's (hpwit) "virtual pins" variant of the I2S clockless driver, drives far more strips than the chip has usable pins by fanning the I2S output through external shift registers. This multiplex technique is the load-bearing idea MoonLight's LED-driver analysis singles out (factoring the shift-register multiplex out of the I2S/LCD peripheral code). Summarised via the GitHub commits API, read across all branches (`main`, `integration`, `int2`, `variable`, `hpwit-patch-1`, `dev`, `optomize`), not just `main`.

## August 2026

No activity. The repository remains dormant: no commits on `main` or any other branch in August 2026, and `main` has not moved since November 2024.

No issues were opened or closed. No release; the most recent is v2.1 from January 2024.

_Checked: `repos/hpwit/I2SClocklessVirtualLedDriver/commits?sha=main` for 2026-08-01..2026-09-01 (0), and the same window on all 7 branches (0); releases published (none); issue search `repo:hpwit/I2SClocklessVirtualLedDriver is:issue created:2026-08-01..2026-08-31` and the same with `closed:` (0 results each)._

## July 2026

No user-facing activity this month: no commits merged to `main` (latest commit on `main` dates to November 2024), no commits on any other branch (newest anywhere is `variable`, December 2024), no releases published, and no issues opened, closed, or updated.

_Checked: commits with author-date 2026-07-01..2026-07-31 on `main` and every branch (`integration`, `int2`, `variable`, `hpwit-patch-1`, `dev`, `optomize`), 0 on each; issues created 2026-07-01..2026-07-31 (0), closed in that range (0), and updated in that range (0); PRs created (0); releases (none in July, latest versioned tag is 2.1, Jan 2024)._

## June 2026

No user-facing activity this month: no commits merged to `main` (latest commit on `main` dates to November 2024), no releases published, and no issues opened, closed, or updated.

_Checked: commits on `main` with author-date 2026-06-01..2026-06-30 (0 commits); issues created 2026-06-01..2026-06-30 (0), closed in that range (0), and updated in that range (0); releases (none in June, latest versioned tag is 2.1, Jan 2024)._

## No activity in the digest window (Sept 2025 – May 2026)

No branch has any commits in the window these digests cover; the project went quiet at the end of 2024. Its last active stretch:

- **December 2024** (`variable` / `int2` branches), variable-driver and integration work; this is the newest commit anywhere in the repo (`variable`, 2024-12-17).
- **November 2024** (`main`, tag `2.1`), **ESP32-S3 support** added and merged to `main` (2024-11-20).
- **Earlier 2024**: `i2sStop` moved into IRAM (June); ESP-IDF 5 corrections (June); shift-array support and examples (Jan–Oct).

*Nothing newer to report. This file gets a new `## <Month Year>` section the next time any branch sees in-window commits, watch `integration`/`variable`, where the development historically happened before merging to `main`.*
