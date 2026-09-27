# Plan: Config backup & restore (tier 1: browser-side, zero firmware change)

## Context

Upgrading a 4 MB board to the MoonBase partition table requires a full serial flash that loses all
on-device config (MIGRATING.md), and boards in the field run months-old firmware (Schelpje: July,
2.1.0-dev). Users need a save → flash → restore path that works against firmware ALREADY deployed -
which rules out anything requiring new device code for the capture side. Tier 1 (this plan) is pure
browser: back up via the existing file API, restore the same way, and report what didn't carry over
instead of losing it silently. Tiers 2 (single-archive endpoint) and 3 (MoonBase-hosted restore)
come later, in that order (PO decision). The feature is also one of the three deciding factors for
"MoonBase as the only update mechanism" (backlog-core).

Schema drift across versions is handled per ADR-0013 (no migration code: the robust loader ignores
unknown keys, defaults absent ones, clamps stale values), the restore's job is to make the
outcome VISIBLE (a report) and to rescue the known renames client-side (a data map sourced from
MIGRATING.md; it lives in the browser, so firmware stays migration-free).

Verified live against Schelpje's July firmware: /api/dir?hidden=1 and /api/file work as needed.
PO decisions: single JSON bundle format (not .zip, also required by the flash-budget rule in
backlog-core.md:819 against bundling a zip lib into app.js); tier order 1 → 2 → 3.

## Design

**Bundle format** (one downloaded .json, the diagnostics-bundle precedent at app.js:777-826):
```json
{ "format": "projectMM-config-backup", "version": 1,
  "capturedAt": "...", "origin": "...", "device": "<deviceName>",
  "firmware": "<firmware>", "build": "<build>",
  "files": { "/.config/NetworkModule.json": "<raw text>", "/scripts/x.mle": "...", ... } }
```
Text files as strings, byte-length verified against /api/dir's size. As built: an unreadable,
oversized-read or non-text file is skipped and reported by name; only a retried short read
(real truncation) aborts the backup, so the bundle holds every successfully read and verified
file rather than claiming the whole filesystem. Contains the WiFi password:
the UI says "keep this file private" at download.

**Backup** (new toolbar button ⤓ "Backup device config" in the File Manager bar, after ↥ Upload,
app.js:4935 area): recursive walk via fmFetchDir(path, hidden=true) (app.js:4773), serial fetches
(the device serves one connection at a time; Connection: close per request), then the
diagnostics-download tail verbatim (Blob → objectURL → a.download → revoke-after-4s Safari
workaround). Filename: MoonLight-config-<deviceName>-<date>.json.

**Restore** (toolbar button ⟲ with the destructive styling + armPressTwice pattern of 🗑): hidden
<input type=file accept=.json> like the upload button (app.js:4941). Steps:
1. Parse + validate the bundle header.
2. Apply the RENAME MAP (below) to filenames, module-type values ("N.type" keys), and control
   keys, collecting "renamed and mapped" report entries.
3. mkdir the directory chain for every path (POST /api/dir, non-recursive → parents first).
4. Serial POST /api/file per file (raw body, Content-Type application/octet-stream; the
   fmDropUpload shape at app.js:5196-5210; respect the 256 KB per-file cap → report, don't die).
   The device coalesces prepareTree across the writes.
5. After the last write, fetch /api/state and DIFF (the report): for each restored
   /.config/<Type>.json, <Type> must exist as a live module type (top-level or via "N.type"
   child chains); each un-prefixed key must exist among that node's persistable controls
   (skip ReadOnly/Progress, never persisted). Classes: "module type unknown" (re-add by hand),
   "control unknown" (value dropped, back at default), "renamed and mapped" (informational).
   Each entry links MIGRATING.md.
6. Offer "reboot device" (POST /api/reboot) so everything applies from a clean boot.

**Rename map** (new src/ui/migrate.js, data not code, exported for tests): the concrete
pairs the exploration extracted from MIGRATING.md, file renames (Layers.json→Effects.json,
LayoutGroup.json→Layouts.json, DriverGroup.json→Drivers.json), module types (I80LedDriver /
MoonI80LedDriver / MultiPinLedDriver / MoonLedDriver / ParlioLedDriver → ParallelLedDriver with
the peripheral value, Layers→Effects), control renames (fps→targetFps on PreviewDriver,
preset→lightPreset, sync→mode+send-audio*, forceRing→useRing*, shiftRegister→pinExpander,
asyncTransmit→doubleBuffer). Entries marked * change VALUE SEMANTICS, not just names: those map
the name and add a "review this value" report entry rather than guessing. The chip-dependent
peripheral option values (i80→I2S-IDF/LCD-IDF) are NOT auto-mapped: report-only, since the right
answer depends on the target chip.

**Old-firmware backup (the chicken-and-egg fix, PO-approved addition)**: a device in the field
(Schelpje) has the file API but not the new Backup button. The installer page hosts a
bookmarklet, one self-contained copy of the walker (mooninstaller/backup-snippet.js, single
home, rendered as a draggable javascript: link + a copy-for-console fallback), that runs
same-origin on the old device's own page and downloads the same bundle. Verified live: the
July firmware already sends Access-Control-Allow-Origin: * on /api/dir and /api/file, so only
mixed content (https page → http device) rules out a hosted backup page; same-origin does not
care. Restore never has the problem: it always targets freshly flashed current firmware.

**mooninstaller link**: the erase confirm at install.js:1037-1044 ("wipes WiFi credentials and
all module state") gains "back up your config first: open the device UI's File Manager and press
Backup", text/link only, the backup itself runs on the device UI (the mixed-content constraint
the diagnostics feature already documents, app.js:778-782).

## Files

- src/ui/app.js, the two toolbar buttons + backup walker + restore engine + report rendering
  (a simple list dialog, reusing the alert-or-better pattern of fmDropUpload's skipped-list).
- src/ui/migrate.js, the rename map (pure data + a tiny applyMigrations(bundle) function),
  embedded like the other UI files (src/ui/embed_ui.cmake picks up the directory, verify;
  otherwise add the file to its list + HTTP route, the safe-storage precedent noted in
  backlog-core.md:417 describes the glue).
- mooninstaller/install.js, the erase-confirm text gains the backup pointer.
- docs/moonmodules/core/services.md is wrong home; the File Manager card lives in
  docs/moonmodules/core/system.md § File Manager, add the Backup/Restore bullets + the
  "contains the WiFi password" caveat + the report semantics; MIGRATING.md header gains one line
  ("the device UI's Backup/Restore carries config across breaks and reports what didn't apply").
- docs/backlog/backlog-core.md, the tier-1 part of the backup/restore follow-up moves from
  backlog to shipped-by (deleted per subtraction; tiers 2/3 stay).

## Tests

- test/js/migrate.test.mjs, the map: file/type/control renames apply; the
  semantics-changed entries produce review flags not value guesses; unknown content passes
  through untouched.
- test/js/backup-bundle.test.mjs, pure helpers (extract them from app.js as testable functions
  in the same file pattern the installer tests use): bundle build from a mock dir/file fetcher
  (sizes verified, truncation detected); restore plan ordering (dirs before files); the report
  diff against a mock /api/state (all three classes + the skip of ReadOnly controls).
- Existing suites untouched; no firmware change, no scenario change.

## Verification

- node --test on the new suites; desktop build (embeds the UI) zero warnings; ctest unchanged.
- Live tier-1 dry run (PO watches): back up the DESKTOP instance, restore onto it, report empty.
- The real thing: back up Schelpje (2.1.0, July) over LAN → the report of that bundle against
  CURRENT firmware names the known renames → (separately, when PO chooses) USB-flash Schelpje to
  MoonBase table → join its AP → restore → reboot → device back with WiFi + config, report shown.

## Risks

1. Old-firmware quirks: 2.1.0's /api/file lacks(?) the atomic write + 256 KB cap, backup is
   read-only so safe; restore always targets CURRENT firmware (post-flash), so the modern
   contract applies. The walker must tolerate the old /api/dir's shape (verified same).
2. The report can only diff .config module files; scripts/presets restore byte-exact but their
   CONTENT may reference renamed types (preset "Layers"→"Effects" values), the map covers the
   two known preset value renames; anything else is report-blind (documented limitation).
3. app.js size growth: pure JS, no libraries; keep the walker/report compact (~250 lines), the
   flash budget rule that killed the zip lib is the ceiling to respect.
