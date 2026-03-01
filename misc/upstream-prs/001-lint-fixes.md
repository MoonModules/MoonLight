# Upstream PR: Lint fixes for ESP32-sveltekit

Target repo: https://github.com/theelims/ESP32-sveltekit

## Summary

While migrating to ESLint 9 flat config on a fork of this project, several real bugs and
anti-patterns were found in the SvelteKit frontend. These are worth fixing upstream so forks
can benefit from the corrections.

---

## 1. `setInterval` bug in BatteryMetrics.svelte (real bug — wrong delay)

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

The same pattern likely exists in any other file that uses `setInterval` in this way.

---

## 2. `{:then nothing}` anti-pattern (unused binding in await blocks)

**Files:** `MQTT.svelte`, `MQTTConfig.svelte`, `NTP.svelte`, `Ethernet.svelte`,
`SystemStatus.svelte`, `Accesspoint.svelte`, `FileManager.svelte`, `GithubFirmwareManager.svelte`,
`user/+page.svelte`

Svelte `{:then nothing}` creates an unused variable named `nothing`. When the promise result is
not needed, the binding should be omitted entirely.

```svelte
<!-- ❌ Current — binds resolved value to unused variable `nothing` -->
{:then nothing}

<!-- ✅ Fix — no binding when result is not used -->
{:then}
```

---

## 3. Missing keys in `{#each}` blocks

Svelte requires keys on `{#each}` blocks when items can be reordered or added/removed, to
allow efficient DOM reconciliation.

Several `{#each}` blocks in the following files lack keys:
- `Accesspoint.svelte` — `{#each provisionMode as mode}` → `{#each provisionMode as mode (mode.id)}`
- `Wifi.svelte` — `{#each connectionMode as mode}`, `{#each txPowerIDToText as mode}`
- `Scan.svelte` — `{#each listOfNetworks as network, i}`
- `NTP.svelte` — `{#each Object.entries(TIME_ZONES) as [tz_label, tz_format]}`
- `GithubFirmwareManager.svelte` — `{#each githubReleases as release}`
- `user/+page.svelte` — `{#each securitySettings.users as user, index}`

Adding a unique key expression `(key)` as the third argument prevents subtle UI bugs when
the list changes.

---

## 4. Stale `svelte-ignore` comments

Two `<!-- svelte-ignore a11y_click_events_have_key_events -->` comments suppress warnings
that are no longer triggered (stale suppression):

- `src/routes/+layout.svelte` — in the `<Modals>` snippet backdrop
- `src/routes/wifi/sta/Wifi.svelte` — in the `DraggableList` children snippet

These can be removed.

---

## 5. Useless escape sequences

**File:** `src/routes/connections/ntp/NTP.svelte`

A character class in a regex contains `\+` which is a useless escape (in a character class,
`+` has no special meaning and does not need escaping):

```js
// ❌
const regex = /[....\+...]/

// ✅
const regex = /[....+...]/
```

---

## Notes on scope

- Items 1 (setInterval bug) and 2 ({:then nothing}) are the most impactful — item 1 is a
  real runtime bug, item 2 is semantically incorrect code.
- Items 3–5 are quality improvements that also suppress lint warnings under
  `eslint-plugin-svelte`.
- Pure style changes (unused variable renaming, removing `const response =`, Prettier
  reformatting) were intentionally excluded from this list as they would create unnecessary
  noise and are only relevant to our linting preferences.
