# Upstream PR: Lint fixes for ESP32-sveltekit

Target repo: https://github.com/theelims/ESP32-sveltekit

## Summary

While migrating to ESLint 9 flat config on a fork of this project, several real bugs and
anti-patterns were found in the SvelteKit frontend. These are worth fixing upstream so forks
can benefit from the corrections.

**Status legend:** ✅ Fixed locally + submitted upstream | 🏠 Fixed locally only | ⏳ Pending

---

## 1. `setInterval` bug in BatteryMetrics.svelte (real bug — wrong delay) ✅

**File:** `interface/src/routes/system/metrics/BatteryMetrics.svelte`

The `setInterval` call is wrapped in an arrow function that forms a comma expression. This
means the interval fires with **no delay** (0 ms) instead of the intended 5000 ms, and the
second argument `5000` is silently discarded as an unused expression.

```js
// ❌ Current — fires every 0 ms, 5000 is a no-op expression
setInterval(() => {
    (updateData(), 5000);
});

// ✅ Fix
setInterval(updateData, 5000);
```

**Fixed locally in commit `a20860a9`.**

---

## 2. `{:then nothing}` anti-pattern (unused binding in await blocks) 🏠

**Files:** `MQTT.svelte`, `MQTTConfig.svelte`, `NTP.svelte`, `Ethernet.svelte`,
`SystemStatus.svelte`, `Accesspoint.svelte` (×2), `Wifi.svelte`, `user/+page.svelte`

Svelte `{:then nothing}` creates an unused variable named `nothing`. When the promise result is
not needed, the binding should be omitted entirely.

```svelte
<!-- ❌ Current — binds resolved value to unused variable `nothing` -->
{:then nothing}

<!-- ✅ Fix — no binding when result is not used -->
{:then}
```

**Fixed locally.** 9 occurrences across 8 files.

---

## 3. Missing keys in `{#each}` blocks 🏠

Svelte requires keys on `{#each}` blocks when items can be reordered or added/removed, to
allow efficient DOM reconciliation.

`{#each}` blocks fixed (key added):
- `Accesspoint.svelte` — `{#each provisionMode as mode (mode.id)}`
- `Wifi.svelte` — `{#each connectionMode as mode (mode.id)}`, `{#each txPowerIDToText as mode (mode.id)}`
- `Scan.svelte` — `{#each listOfNetworks as network, i (network.bssid)}`
- `NTP.svelte` — `{#each Object.entries(TIME_ZONES) as [tz_label, tz_format] (tz_label)}`
- `user/+page.svelte` — `{#each securitySettings.users as user, index (user.username)}`
- `GithubFirmwareManager.svelte` — already had `(release.id)` key ✅

**Fixed locally.**

---

## 4. Stale `svelte-ignore` comments 🏠

Two `<!-- svelte-ignore a11y_click_events_have_key_events -->` comments suppress warnings
that are no longer triggered (stale suppression):

- `src/routes/+layout.svelte` — in the `<Modals>` snippet backdrop
- `src/routes/wifi/sta/Wifi.svelte` — in the `DraggableList` children snippet

**Fixed locally** — both removed.

---

## 5. Useless escape sequences 🏠

**File:** `src/routes/connections/ntp/NTP.svelte`

A character class in a regex contains `\+` which is a useless escape (in a character class,
`+` has no special meaning and does not need escaping):

```js
// ❌
const regex = /[-a-zA-Z0-9@:%_\+.~#?&//=]{2,256}.../

// ✅
const regex = /[-a-zA-Z0-9@:%_.~#?&//=]{2,256}.../
```

**Fixed locally** — both occurrences in the URL regex.

---

## Notes on scope

- Item 1 (setInterval bug) is a real runtime bug — interval was firing every 0 ms.
- Items 2–5 are quality/correctness improvements found during ESLint 9 migration.
- Items 2–5 were applied locally rather than waiting for upstream because the user accepted
  the merge conflict risk. When upstream fixes these, expect minor conflicts in the affected files.
- Pure style changes (Prettier reformatting of upstream files) were intentionally excluded to
  keep merges clean.

---

## What's been set up in MoonLight (linting infrastructure)

### Frontend (`interface/`)

| Tool | Config | Scope |
|---|---|---|
| ESLint 9 (flat) | `eslint.config.js` | MoonLight files only (`src/routes/moonbase/`, `src/lib/components/moonbase/`, moonbase stores/types, `vite-plugin-littlefs.ts`) |
| Prettier | `.prettierrc` / `.prettierignore` | Same MoonLight boundary |
| svelte-check | `tsconfig.json` | All files |

Run: `npm run lint` (Prettier check + ESLint) · `npm run format` (auto-fix Prettier)

Current status: **0 errors, 49 warnings** (all `@typescript-eslint/no-explicit-any` — intentional, set to `warn`)

### Backend (`src/`)

| Tool | Config | Scope |
|---|---|---|
| cppcheck | `platformio.ini` [env] section | `src/**/*.cpp` + `src/**/*.h` (excl. moonmanpng.h) |
| clang-format | `.clang-format` | Google style, 2-space, col limit 1000 |
| clang-tidy | `.clang-tidy` | `src/**` (local only, needs `brew install llvm`) |

Run cppcheck (full): see comment block in `platformio.ini` for the manual command.
Run clang-tidy: `pio run -e esp32-s3 -t compiledb` then `run-clang-tidy -p . $(find src -name "*.cpp")`

Current status: **0 defects** (cppcheck clean after fixes in session 2026-03-02)

Recent fixes (session 2026-03-02):
- `E_MoonModules.h:413`: `uint16_t projector <= 0` → `== 0` (semantically equivalent, silences warning)
- `M_MoonLight.h:215`: `uint8_t swirlVal < 0` always false — suppressed with note (reverse swirl feature broken until type changes to `int8_t`)
- `E_WLED.h:1765`: Copy-paste bug — `drops[x].col == 0` was resetting `pos` instead of `col`; also changed `<= 0` to `== 0` for `uint16_t`
- `E_WLED.h:1866`: Suppressed `knownConditionTrueFalse` — `colIndex == falling` is always true by state machine logic in that branch
- `E_WLED.h:2024-2025`: Suppressed `knownConditionTrueFalse` — `brightness` hardcoded to 128 (original WLED used dynamic `getBrightness()`)
- `ModuleDrivers.h:126`, `ModuleEffects.h:210`: Suppressed `knownConditionTrueFalse` — first `if (!node)` always true after `node = nullptr`; intentional chain pattern
- `E_MoonLight.h:1169,1205`: Suppressed `duplicateCondition` — consecutive `if (debugPrint)` debug log lines; intentional
- All C-style casts (`(uint8_t*)`, `(byte*)`, `(CRGB*)`, `(Module*)`, `(ArtNetHeader*)`, `(DDPHeader*)`) converted to `reinterpret_cast<>` / `static_cast<>` (cppcheck 2.19 `dangerousTypeCast`)
- CI: Added `-DFT_ENABLED(x)=x` to cppcheck command (was causing `syntaxError` for function-like macro)

### CI (GitHub Actions)

`.github/workflows/lint.yml` — runs on push/PR to `main`:
- `npm run lint` in `interface/` (fails on Prettier or ESLint errors)
- `cppcheck` on `src/` (fails on warnings or errors)

---

## Recommended next steps (stricter linting)

### Frontend

1. **Escalate `any` to error** — Once generic module components (`FieldRenderer.svelte`,
   `RowRenderer.svelte`, etc.) are refactored to use proper types from `moonbase_models.ts`,
   change `'@typescript-eslint/no-explicit-any': 'warn'` to `'error'` in `eslint.config.js`.

2. **Add `svelte/no-unused-svelte-ignore`** — Catches stale `<!-- svelte-ignore -->` comments
   automatically (avoids item 4 recurring):
   ```js
   'svelte/no-unused-svelte-ignore': 'error'
   ```

3. **Add strict TypeScript checks** in `tsconfig.json`:
   ```json
   "noUncheckedIndexedAccess": true,
   "exactOptionalPropertyTypes": true
   ```

4. **Pre-commit hooks** (optional) — `husky` + `lint-staged` to auto-format on commit:
   ```json
   "lint-staged": { "*.{ts,svelte}": ["prettier --write", "eslint --fix"] }
   ```

5. **Extend lint to upstream files** — If upstream ever adopts ESLint 9, remove the ignore
   entries from `eslint.config.js` and `.prettierignore` gradually.

### Backend

1. **Enable clang-tidy in CI** — Requires either a pre-built Xtensa toolchain or running
   with a native (non-cross) compiler for header-only analysis. Complex but valuable.

2. **Add `--enable=performance` to cppcheck** — Currently only `warning` is enabled.
   Performance checks catch unnecessary copies, inefficient algorithms, etc.:
   ```ini
   cppcheck: --enable=warning,performance
   ```

3. **Add `cppcheck-misra`** — MISRA C++ 2008 compliance checking (safety-critical embedded
   coding standard). Requires MISRA addon license. Very strict — evaluate whether it’s worth the noise.

4. **clang-tidy in CI** — Once enabled locally, add to `.github/workflows/lint.yml`:
   ```yaml
   - run: pio run -e esp32-s3 -t compiledb
   - run: run-clang-tidy -p . $(find src -name "*.cpp") -warnings-as-errors="*"
   ```
