// MoonLight install picker — shared by the on-device UI (OTA flash) and the
// GitHub Pages installer (first flash via Web Serial). Renders Release +
// Board + Firmware dropdowns and an Install button; the caller wires the
// onInstall callback to the right install transport.
//
// Same data source, two install transports:
//   - Device UI: caller POSTs the chosen .bin URL to /api/firmware/url; the
//     device's HTTPS client downloads + writes to the OTA partition. No CORS
//     in the data path.
//   - Web installer: caller hands the chosen manifest URL to the custom
//     install-orchestrator.js (which drives esptool-js + improv-wifi-serial-sdk
//     over Web Serial). Manifest and binaries must be same-origin with the
//     page (CORS), which is why the Pages site self-hosts the last N releases.
//
// The picker is a presentation+state machine; it does not decide *how* to
// install. The caller passes an onInstall(firmware, manifestUrl, binaryUrl)
// callback and wires it to the right transport.
//
// "Firmware" here is the compiled binary variant (chip + radios + sdkconfig
// fragments), not the physical board. See docs/explanation/architecture/mooninstaller.md § Firmware
// vs board. Release assets are named per firmware variant
// (firmware-<variant>-v<ver>.bin, manifest-<variant>.json).
//
// Sections (top to bottom):
//   1. Constants + module state
//   2. GitHub Releases API + sessionStorage cache
//   3. Asset parsing (manifest-*.json → firmware, firmware-*.bin → binary)
//   4. Compatibility filter (OTA only)
//   5. Relative-time helper
//   6. DOM construction + event wiring
//
// Tests: the pure helpers below — `isCompatible`, `parseFirmwaresFromAssets`,
// `mergeFirmwares` — are exported (alongside `installPicker`) and unit-tested in
// test/js/installer-firmware-merge.test.mjs (run: `node --test test/js`). They're
// pure (data in, data out, no DOM), so the test imports and exercises the real
// functions. The DOM-wiring half of this file isn't node-testable; its contracts
// are pinned by the catalog/firmware check scripts + the scenario suite instead.

// ---------------------------------------------------------------------------
// 1. Constants + module state
// ---------------------------------------------------------------------------

const API_URL = "https://api.github.com/repos/MoonModules/projectMM/releases?per_page=10";
const CACHE_KEY = "MoonLight.releases.v1";
const CACHE_TTL_MS = 5 * 60 * 1000;  // 5 min — short enough to surface new RCs, long enough to avoid rate-limit thrash

// Persisted user selection — survives page reloads and full browser restarts,
// so a returning user doesn't have to re-pick their firmware every time. Keyed
// separately from the API cache (which is sessionStorage with a TTL); these
// are intent, not data, and never expire on their own.
const PREF_RELEASE_KEY  = "MoonLight.picker.releaseTag";
const PREF_FIRMWARE_KEY = "MoonLight.picker.firmware";
const PREF_BOARD_KEY    = "MoonLight.picker.board";

// A link may name what to preselect: `?release=v5.0.0&firmware=esp32s3-n16r8&board=...`.
// It outranks the saved preference, since a link is the sender's intent and the preference
// is the visitor's habit, and a support link that lands on the wrong board helps nobody.
// An unknown value falls through to the saved preference rather than selecting nothing.
function urlParam(name) {
    try {
        return new URLSearchParams(window.location.search).get(name);
    } catch {
        return null;   // a document with no location, such as a test harness
    }
}

// Firmware variants published but NEVER RUN ON HARDWARE — flagged in the dropdown so a
// user knows before flashing. The P4 rev3 images are built for the current v3.x silicon,
// which no bench board has (both are v1.3 "engineering samples"), so they are published
// for someone with a v3 board to try: without them a v3 board has no image at all, since
// the rev1 binary is rejected by its bootloader. Remove a key once its variant is
// bench-verified.
//
// esp32p4rev1-eth-wifi is NOT experimental: on IDF v6.1-rc1 with
// CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n it boots, associates and serves the UI.
const EXPERIMENTAL_FIRMWARES = new Set(["esp32p4rev3-eth", "esp32p4rev3-eth-wifi"]);

// One picker instance per init() call. Each tracks its own state so multiple
// pickers on a page (unused today but possible) don't fight over selections.
function makeState() {
    return {
        container: null,
        ownFirmwareKey: null,
        onInstall: null,
        onDetect: null,        // web installer only: opens serial, returns the
                               // detected chip-family string ("ESP32" / "ESP32-S3").
                               // null on on-device OTA (no local serial), which is
                               // also what gates the Detect button off there.
        detectedChip: null,    // chip family from the last successful Detect, or null
        runDetect: null,       // render() fills this with the detect routine when
                               // the Detect button is present; runDetect() public
                               // method calls it (host's auto-fire on port grant)
        enableBoardPicker: true, // true on web installer, false on on-device OTA
        // Optional caller-owned DOM element rendered just above the Install
        // button row. The web installer uses this to slot its "Erase chip
        // first" checkbox between the firmware dropdown and the Install
        // button without giving the picker any erase-specific knowledge —
        // keeps the picker reusable on the on-device OTA UI (where erase
        // makes no sense). The picker takes ownership: it appendChild's
        // the node on every render(), so the same instance survives the
        // re-renders triggered by release-list reloads.
        installRowExtras: null,
        extrasAfterInstall: false,
        moonbaseOnly: false,
        releases: [],          // normalised release records from the API
        sortedReleases: [],    // releases sorted newest-first; render() fills this
        releaseIdx: 0,         // index into sortedReleases
        firmware: null,        // selected firmware key
        boards: [],            // parsed mooninstaller/deviceModels.json, [] if unavailable
        selectedBoard: null,   // user pick from board <select>; "" for (any board)
        hasPort: null,         // web installer only: () => bool, "is a USB port
                               // picked?". When set, Install is disabled until it
                               // returns true (the host re-evaluates via
                               // notifyPortChanged on every port change). null on
                               // the on-device OTA picker (no serial-port concept),
                               // which leaves the button ungated as before.
        boardSupport: null,    // board-catalog + chip-detect helpers injected by
                               // the web installer (install-picker-boards.js); null
                               // on-device, so no board code ships in the firmware.
    };
}

// Module-level handle to the most recently mounted picker's state, so the
// host page can call installPicker.getSelectedBoard() without threading the
// state object through every callback. Web installer mounts exactly one
// picker per page; if a future page mounts multiple, this becomes wrong
// (returns whichever initialized last). See comment at makeState — pickers
// are otherwise isolated.
let _lastState = null;
// A setDesktopMode() that arrived before init() mounted the state. Applied by init().
let _pendingDesktopMode = null;

// ---------------------------------------------------------------------------
// 2. GitHub Releases API + sessionStorage cache
// ---------------------------------------------------------------------------

// Safe sessionStorage accessor — private/incognito Safari throws on
// getItem/setItem entirely, not just quota. Wrap every access so a host
// browser with storage disabled is a cache miss, not an exception.
function safeStorageGet(key) {
    try { return sessionStorage.getItem(key); } catch (_) { return null; }
}
function safeStorageSet(key, value) {
    try { sessionStorage.setItem(key, value); } catch (_) { /* ignore */ }
}

// Same wrappers for localStorage — the persisted user selection (release tag,
// firmware) survives across browser sessions. Same try/catch shape so a hostile
// storage backend is a "no preference" miss, not an exception.
function safeLocalGet(key) {
    try { return localStorage.getItem(key); } catch (_) { return null; }
}
function safeLocalSet(key, value) {
    try { localStorage.setItem(key, value); } catch (_) { /* ignore */ }
}

// Returns the parsed releases array, or null on hard failure (network, 403).
// Stale cache is served on transient failures so the dropdown doesn't go empty.
async function loadReleases({ bypassCache = false } = {}) {
    if (!bypassCache) {
        const raw = safeStorageGet(CACHE_KEY);
        if (raw) try {
            const obj = JSON.parse(raw);
            if (Date.now() - obj.ts < CACHE_TTL_MS) return obj.data;
        } catch (_) { /* corrupt cache — fall through to fetch */ }
    }
    try {
        const res = await fetch(API_URL);
        if (!res.ok) {
            // 403 = anonymous rate-limit (60/hr). Fall back to stale cache if any.
            const raw = safeStorageGet(CACHE_KEY);
            if (raw) try { return JSON.parse(raw).data; } catch (_) { /* fall through */ }
            return null;
        }
        const data = await res.json();
        safeStorageSet(CACHE_KEY, JSON.stringify({ ts: Date.now(), data }));
        return data;
    } catch (_) {
        const raw = safeStorageGet(CACHE_KEY);
        if (raw) try { return JSON.parse(raw).data; } catch (_) { /* fall through */ }
        return null;
    }
}

// ---------------------------------------------------------------------------
// 3. Asset parsing
// ---------------------------------------------------------------------------

// Each release's `assets[]` contains both manifest-<firmware>.json and
// firmware-<firmware>-v<ver>(.bin|-bootloader.bin|-partition-table.bin|-ota-data.bin).
// The picker needs: per-firmware → {manifestUrl, binaryUrl}. Manifest URL drives
// ESP Web Tools (web installer); binary URL drives /api/firmware/url (device OTA).
function parseFirmwaresFromAssets(assets, tag, moonbaseOnly = false) {
    if (!assets) return [];
    const firmwares = new Map();
    const manifestRe = /^manifest-(.+)\.json$/;
    const binaryRe = /^firmware-(.+)-v.+?\.bin$/;  // app .bin only (no -bootloader/-partition-table/-ota-data)

    for (const a of assets) {
        const m = manifestRe.exec(a.name);
        if (m) {
            const firmware = m[1];
            const entry = firmwares.get(firmware) || { firmware, manifestUrl: null, binaryUrl: null };
            entry.manifestUrl = a.browser_download_url;
            firmwares.set(firmware, entry);
        }
    }
    // A DESKTOP build ships an archive, not a .bin: projectMM-<os>-<arch>-v<ver>.{dmg,tar.gz,zip,deb}.
    // The device cannot flash itself over the air, so these are offered as a DOWNLOAD instead, and
    // the caller decides what that means. Keyed by the platform so the picker can show "macOS
    // arm64" rather than a filename.
    // The version carries dots, so the platform group has to be anchored on the -v rather than on
    // "everything up to a dot". A .deb names its arch, not the platform, so it maps to linux-x64.
    //
    // The Windows INSTALLER carries a suffix AFTER the version — projectMM-windows-x64-v1.2.3
    // -setup.exe — so the extension alternation alone could not match it and the installer never
    // reached the dropdown: a Windows user was offered the bare .zip while setup.exe sat in the
    // release. `.+` before the extension covers both shapes.
    const desktopRe = /^projectMM-(macos-arm64|windows-x64|linux-x64)-v.+\.(dmg|tar\.gz|zip|exe)$/;
    const debRe = /^projectmm_.+_amd64\.deb$/;
    for (const a of assets) {
        const d = desktopRe.exec(a.name);
        const plat = d ? d[1] : (debRe.test(a.name) ? "linux-x64" : null);
        if (!plat) continue;
        const key = `desktop-${plat}`;
        const entry = firmwares.get(key) || { firmware: key, manifestUrl: null, binaryUrl: null,
                                              isDesktop: true, assets: [] };
        entry.isDesktop = true;
        (entry.assets = entry.assets || []).push({ name: a.name, url: a.browser_download_url });
        // Prefer the friendliest form when a platform ships more than one: a .dmg to drag, a
        // setup.exe that installs, or a .deb apt can install, over the archive that is there for
        // scripting. An installer is what a person double-clicks, which is the whole point of
        // offering a download rather than a flash.
        const friendly = /(\.dmg|\.deb|-setup\.exe)$/.test(a.name);
        if (!entry.binaryUrl || friendly) entry.binaryUrl = a.browser_download_url;
        firmwares.set(key, entry);
    }

    for (const a of assets) {
        // MoonBase, when the caller asked for it. `shared-moonbase-<chip>.bin` is one image per
        // CHIP rather than per variant, and it has no manifest of its own, so it is admitted here
        // and exempted from the manifest requirement below.
        //
        // Only the ON-DEVICE card asks. The web installer writes MoonBase already, and always
        // has: a serial flash applies the whole manifest, which stages this same asset at the
        // factory offset (generate_manifest.py). What is new is installing it OVER THE NETWORK,
        // into a device whose recovery image is broken and which therefore cannot be reached the
        // usual way without a cable. So this mode is off by default and the installer is
        // unaffected: it was excluded outright while the only network route wrote the APP slot,
        // where an image that can install but not run the show would have been a brick.
        if (moonbaseOnly) {
            const mb = /^shared-moonbase-(.+)\.bin$/.exec(a.name);
            if (mb) {
                firmwares.set(mb[1], { firmware: mb[1], manifestUrl: null,
                                       binaryUrl: a.browser_download_url, isMoonBase: true });
            }
            continue;
        }
        // Reject the part-suffixed .bins (bootloader / partition-table / ota-data / moonbase):
        // they're install fragments, not the main image. The OTA path needs the app image only.
        if (/(?:-(?:bootloader|partition-table|ota-data)|moonbase[^/]*|-slot0)\.bin$/.test(a.name)) continue;
        const m = binaryRe.exec(a.name);
        if (m) {
            const firmware = m[1];
            const entry = firmwares.get(firmware) || { firmware, manifestUrl: null, binaryUrl: null };
            entry.binaryUrl = a.browser_download_url;
            firmwares.set(firmware, entry);
        }
    }
    // An OTA firmware needs BOTH a manifest and a binary: a partial upload (the
    // mid-release-publish race) shouldn't appear in the dropdown. A desktop archive has no
    // manifest by nature (there is nothing to flash), so it qualifies on its download alone.
    return Array.from(firmwares.values())
        .filter(f => f.binaryUrl && (f.manifestUrl || f.isDesktop || f.isMoonBase));
}

// Merge a release's published firmwares with locally-staged extras (preview only).
// Same-named local entry wins (it overrides the published bin with the local build);
// a local-only firmware (not yet in any release) is added. Returns a fresh array.
function mergeFirmwares(published, extras) {
    if (!extras || !extras.length) return published;
    const byName = new Map(published.map(f => [f.firmware, f]));
    for (const e of extras) {
        if (e && e.firmware && e.manifestUrl && e.binaryUrl) byName.set(e.firmware, e);
    }
    return Array.from(byName.values());
}

// ---------------------------------------------------------------------------
// 4. Compatibility filter (OTA only)
// ---------------------------------------------------------------------------

// Bespoke rule for MoonLight's firmware keys: strip the `-eth*` suffix from
// both sides; equal identities are mutually OTA-compatible. So `esp32` and
// `esp32-eth` can flash each other (same physical ESP32 silicon; the variant
// decides which radios are compiled in) — as can the legacy `esp32-eth-wifi`
// key (dropped in the variant collapse, but still reported by devices flashed
// before it; it strips to `esp32` and stays OTA-compatible). `esp32s3-n16r8` is
// only compatible with itself (different chip family AND different partition
// table). Web installer passes ownFirmwareKey=null → all candidates compatible.
function isCompatible(ownFirmwareKey, candidateFirmwareKey) {
    if (!ownFirmwareKey) return true;
    // A desktop reports "unknown": there is no ESP32 variant to name. Match it to the archive built
    // for the OS the browser is running on, so a Mac is offered the .dmg and not the Windows .zip.
    if (ownFirmwareKey === "unknown") return candidateFirmwareKey === desktopKeyForThisHost();
    const strip = f => f.replace(/-eth.*$/, "");
    return strip(ownFirmwareKey) === strip(candidateFirmwareKey);
}

// One name per desktop archive, used by both the Device row and the Firmware option so the two
// cannot drift apart.
const DESKTOP_LABEL = {
    "desktop-macos-arm64": "macOS arm64",
    "desktop-windows-x64": "Windows x64",
    "desktop-linux-x64":   "Linux x64",
};

// Which desktop archive belongs to the machine viewing this page. Null on an OS we do not package,
// which leaves the list empty rather than offering a download that will not run.
function desktopKeyForThisHost() {
    const p = (navigator.platform || "") + " " + (navigator.userAgent || "");
    if (/Win/i.test(p)) return "desktop-windows-x64";
    if (/Mac/i.test(p)) return "desktop-macos-arm64";
    if (/Linux|X11/i.test(p)) return "desktop-linux-x64";
    return null;
}

// ---------------------------------------------------------------------------
// 5. Relative-time helper
// ---------------------------------------------------------------------------

// "2 days ago", "in 3 hours", etc. Uses Intl.RelativeTimeFormat (Chrome 71+,
// every modern browser). 15 lines, no external library.
const rtf = new Intl.RelativeTimeFormat("en", { numeric: "auto" });
function relativeTime(iso) {
    if (!iso) return "";
    const diffMs = new Date(iso) - new Date();
    const units = [
        ["year",   365 * 24 * 60 * 60 * 1000],
        ["month",   30 * 24 * 60 * 60 * 1000],
        ["week",     7 * 24 * 60 * 60 * 1000],
        ["day",          24 * 60 * 60 * 1000],
        ["hour",              60 * 60 * 1000],
        ["minute",                 60 * 1000],
    ];
    for (const [unit, ms] of units) {
        if (Math.abs(diffMs) >= ms) return rtf.format(Math.round(diffMs / ms), unit);
    }
    return rtf.format(Math.round(diffMs / 1000), "second");
}

// ---------------------------------------------------------------------------
// 6. DOM construction + event wiring
// ---------------------------------------------------------------------------

// The board catalog + chip-detection logic (loadBoards / fillBoardOptions /
// applyDetectedChip) lives in install-picker-boards.js and is injected via
// init({ boardSupport }) — WEB INSTALLER ONLY. This file embeds into the device
// firmware (embed_ui.cmake), and the device's OTA picker passes no boardSupport,
// so none of that code ships on the board. render() reaches the injected
// functions through state.boardSupport; every use is guarded by it being set.

// Builds the picker UI into `state.container`. Idempotent — calling more than
// once just rebuilds.
//
// Uses the same `.control-row` / `.control-label` / `<select>` shape as the
// rest of `createControl()` in app.js so the picker visually integrates with
// the card it's mounted in. The web installer overrides these with its own
// styles in mooninstaller/index.html, which gives the installer page the same
// look without app.js loading.
// Draw the field rows immediately, before the network fetches resolve, so the
// user sees the full form straight away instead of a lone "Loading…" line that
// pops into Release/Board/Firmware on a slow connection. Each <select> is
// disabled and shows a spinning "Loading…" placeholder until render() swaps in
// the real options. Same row markup as render() so the swap is seamless. The
// Board row is included whenever the picker is in board-picker mode — if the
// catalog ends up empty, render() simply omits it (the skeleton row vanishes on
// the swap, which on a fast same-origin deviceModels.json fetch is imperceptible).
function renderSkeleton(state) {
    // A select can't host an animated element in its option text, so the spinning
    // ring (.rp-spinner, styled by the installer page) sits in the row next to a
    // disabled select showing a plain "Loading…" placeholder. Where .rp-spinner
    // isn't defined (the on-device picker's host page), the span is simply empty —
    // the "Loading…" text still reads as a waiting state.
    const field = `<span class="rp-spinner"></span>` +
        `<select class="rp-select" disabled><option>Loading…</option></select>`;
    const row = (label) =>
        `<div class="control-row"><span class="control-label">${label}</span>${field}</div>`;
    const boardRow = state.enableBoardPicker ? row("Device") : "";
    state.container.innerHTML = row("Release") + boardRow + row("Firmware") + `
        <div class="control-row rp-status-row">
            <span class="control-label"></span>
            <span class="rp-status">Fetching releases…</span>
        </div>`;
}

function render(state) {
    // newest-first; releases without `published_at` (drafts) sort last.
    const sorted = state.releases.slice().sort((a, b) => {
        const aT = a.published_at ? Date.parse(a.published_at) : 0;
        const bT = b.published_at ? Date.parse(b.published_at) : 0;
        return bT - aT;
    });
    state.sortedReleases = sorted;

    // Row order: Release → Board → Firmware. Release first because it's the
    // version the user wants to flash (the picker's primary identity);
    // Board second so the firmware narrowing happens in front of Firmware;
    // Firmware last so it shows the narrowed list immediately below the
    // board that filtered it. The board row only renders when (a) the
    // caller didn't opt out and (b) the catalog actually loaded —
    // on-device OTA passes enableBoardPicker:false (the device already
    // knows its board); catalog-missing on the web installer (rare) falls
    // back to a two-row Release+Firmware layout with no board narrowing.
    // In desktop mode the device is not a board being flashed, it is the computer viewing the
    // page, so the row names it instead of offering a catalog to narrow.
    // In desktop mode the target is the computer viewing the page, not a board: no catalog to
    // narrow by. The web installer draws its own Device row (a picture grid it hides in this
    // mode), so naming the computer here would be a second one; only the on-device card, which
    // has no board row of its own, gets the static label.
    const desktopMode = state.ownFirmwareKey === "unknown";
    const boardRow = (desktopMode && !state.enableBoardPicker) ? `
        <div class="control-row">
            <span class="control-label">Device</span>
            <span class="rp-status">${DESKTOP_LABEL[desktopKeyForThisHost()] || "this computer"}</span>
        </div>` : (!desktopMode && state.enableBoardPicker && state.boards.length > 0) ? `
        <div class="control-row">
            <span class="control-label">Device</span>
            <select id="rp-board" class="rp-select"></select>
        </div>` : "";
    state.container.innerHTML = `
        <div class="control-row">
            <span class="control-label">Release</span>
            <select id="rp-release" class="rp-select"></select>
        </div>` + boardRow + `
        <div class="control-row">
            <span class="control-label">Firmware</span>
            <select id="rp-firmware" class="rp-select"></select>
        </div>
        <div class="control-row" id="rp-install-row">
            <span class="control-label"></span>
            <button id="rp-install" class="action-btn" type="button">Install</button>
        </div>
        <div class="control-row rp-status-row">
            <span class="control-label"></span>
            <span id="rp-status" class="rp-status"></span>
        </div>
    `;
    // installRowExtras: caller-owned element slotted right before the
    // Install row. Re-attached on every render() so the row survives the
    // innerHTML reset above. insertBefore moves the existing DOM node
    // rather than cloning — the caller wires the listeners once and they
    // keep firing across renders.
    if (state.installRowExtras) {
        const installRow = state.container.querySelector("#rp-install-row");
        // BEFORE the Install row by default, which is where an option that MODIFIES the install
        // belongs (the web installer's erase checkbox). `extrasAfterInstall` puts them after
        // instead, for rows that are alternative ways to install rather than options on this one:
        // the device card's URL and File rows, which otherwise pushed the Install button away
        // from the two dropdowns it acts on.
        if (state.extrasAfterInstall) installRow.after(state.installRowExtras);
        else state.container.insertBefore(state.installRowExtras, installRow);
    }

    const boardEl = state.container.querySelector("#rp-board");
    const releaseEl = state.container.querySelector("#rp-release");
    const firmwareEl = state.container.querySelector("#rp-firmware");
    const installBtn = state.container.querySelector("#rp-install");
    const statusEl = state.container.querySelector("#rp-status");

    if (boardEl && state.boardSupport) {
        // Full catalog, no chip filter yet. The "(any board)" pass-through
        // means the firmware dropdown shows every compatible firmware, just
        // as if the board picker didn't exist.
        state.boardSupport.fillBoardOptions(boardEl, state.boards, "(any board)");
        // Restore the user's last picked board if it's still in the catalog
        // (the catalog may have changed since their last visit; falling
        // through to "(any board)" if their pick is gone is the safe shape).
        const wantedBoard = urlParam("board") || safeLocalGet(PREF_BOARD_KEY);
        if (wantedBoard && state.boards.find(b => b.name === wantedBoard)) {
            state.selectedBoard = wantedBoard;
        }
        boardEl.value = state.selectedBoard || "";
    }

    // One option per release, newest-first. RC tags carry a "(beta)" suffix
    // and a different color so a casual user can't mistake them for a
    // production release. The compatibility filter at the firmware step
    // handles the "is this binary for my chip?" question, so the release
    // dropdown doesn't pre-filter on it — a user can still see every release
    // that exists, even ones whose binaries don't match their firmware.
    //
    // Use textContent rather than innerHTML so a tag name that contains
    // HTML (a compromised release tag, a typo with a literal `<`) renders
    // as text instead of getting injected. r.tag_name is GitHub-API-supplied
    // — outside our control once the picker fetches it.
    releaseEl.replaceChildren();
    sorted.forEach((r, i) => {
        const opt = document.createElement("option");
        opt.value = String(i);
        const flag = r.prerelease ? " (beta)" : "";
        const age = relativeTime(r.published_at);
        opt.textContent = `${r.tag_name}${flag} — ${age}`;
        releaseEl.appendChild(opt);
    });

    // Default selection order:
    //   1. Last release tag the user picked, if it's still in the list.
    //   2. Newest stable.
    //   3. Newest prerelease (falls through when no stable exists yet).
    const wantedTag = urlParam("release") || safeLocalGet(PREF_RELEASE_KEY);
    const savedIdx = wantedTag ? sorted.findIndex(r => r.tag_name === wantedTag) : -1;
    const firstStable = sorted.findIndex(r => !r.prerelease);
    state.releaseIdx = savedIdx >= 0 ? savedIdx
                     : firstStable >= 0 ? firstStable
                     : 0;
    releaseEl.value = String(state.releaseIdx);

    // Whether the firmware dropdown currently holds a valid, flashable selection.
    // refreshFirmwareDropdown() sets it; applyInstallEnabled() ANDs it with the
    // port gate so a port-change can re-enable/disable Install without rebuilding
    // the dropdown.
    let firmwareReady = false;

    // Final Install-button gate: a valid firmware AND (when the host supplied a
    // hasPort predicate — web installer) a USB port picked. The on-device OTA
    // picker passes no hasPort, so it's port-gate-free as before. Shows the reason
    // as the button title so the disabled state isn't a mystery.
    function applyInstallEnabled() {
        const portOk = !state.hasPort || state.hasPort();
        installBtn.disabled = !(firmwareReady && portOk);
        installBtn.title = !firmwareReady ? "Select a firmware to flash"
                         : !portOk ? "Select a USB port first"
                         : "";
    }
    state.applyInstallEnabled = applyInstallEnabled;

    function refreshFirmwareDropdown() {
        firmwareEl.disabled = false;  // re-enable in case prior state had a single-firmware board
        firmwareReady = false;
        const r = sorted[state.releaseIdx];
        if (!r) {
            firmwareEl.innerHTML = `<option value="">—</option>`;
            installBtn.disabled = true;
            return;
        }
        // A desktop archive and a flashable firmware are never alternatives in the same list:
        // the web installer with no ownFirmwareKey accepts every candidate, which would otherwise
        // offer "macOS arm64" as something to flash onto an ESP32. The target picked in the Port
        // row decides which kind is on offer.
        const wantDesktop = state.ownFirmwareKey === "unknown";
        let compatible = (r.firmwares || [])
            .filter(f => !!f.isDesktop === wantDesktop)
            // A MoonBase key is a CHIP ("esp32s3"), not a firmware variant, so the variant rule
            // (which strips -eth and compares) does not apply: an exact chip match is the whole
            // question, and offering another chip's image is what the device's own header check
            // exists to refuse.
            .filter(f => state.moonbaseOnly ? f.firmware === state.ownFirmwareKey
                                            : isCompatible(state.ownFirmwareKey, f.firmware));
        // Narrow by selected board (web installer only — selectedBoard stays
        // null on the on-device picker since the board <select> isn't rendered).
        // Defensive: a board the user picked that isn't in the catalog (e.g.
        // catalog edited mid-session) skips the narrow — better than rejecting
        // every firmware silently.
        if (state.selectedBoard && !wantDesktop) {
            const board = state.boards.find(b => b.name === state.selectedBoard);
            if (board) {
                compatible = compatible.filter(f => board.firmwares.includes(f.firmware));
            }
        }
        if (compatible.length === 0) {
            // Distinguish the no-match reasons:
            //   - device firmware "unknown" (build didn't propagate
            //     MM_FIRMWARE_NAME) → a build bug; surface it for the dev.
            //   - a board whose firmware exists in ANOTHER release we can see
            //     but not the selected one → a newer firmware variant (e.g. a
            //     board added after the last stable). Point the user at the
            //     release that has it instead of dead-ending. This is exactly
            //     the case for boards added between releases: their binary only
            //     lands in `latest` / the next tag, not the older stable.
            //   - otherwise → genuinely no build for this board/firmware.
            let reason;
            if (state.ownFirmwareKey === "unknown") {
                // "unknown" on a DESKTOP is correct, not a build bug: there is no ESP32 variant.
                // It only reaches here when the release ships no archive for this OS.
                reason = desktopKeyForThisHost()
                    ? "this release has no desktop build for your OS"
                    : "no desktop build is packaged for your OS: build from source";
            } else if (state.selectedBoard) {
                const board = state.boards.find(b => b.name === state.selectedBoard);
                const wanted = board ? board.firmwares : [];
                // Newest other release whose assets include a firmware this board needs.
                const elsewhere = sorted.find((rel, idx) =>
                    idx !== state.releaseIdx
                    && (rel.firmwares || []).some(f => wanted.includes(f.firmware)));
                reason = elsewhere
                    ? `${state.selectedBoard} needs a newer release — select ${elsewhere.tag_name}${elsewhere.prerelease ? " (beta)" : ""} above`
                    : `no compatible firmware for ${state.selectedBoard} in this release`;
            } else {
                reason = "no compatible firmwares in this release";
            }
            firmwareEl.innerHTML = `<option value="">— ${reason} —</option>`;
            installBtn.disabled = true;
            return;
        }
        // Same XSS guard as the release dropdown: textContent over innerHTML.
        // f.firmware comes from parseFirmwaresFromAssets parsing GitHub asset
        // names with a strict regex, so the risk is lower here, but
        // consistency wins.
        firmwareEl.replaceChildren();
        compatible.forEach(f => {
            const opt = document.createElement("option");
            opt.value = f.firmware;
            // The dropdown shows the bare firmware key, so a variant that flashes but
            // is unverified must carry its own visible warning here — the firmwares.json
            // `description` isn't loaded by the picker (it parses names from release asset
            // filenames), so a user picking blind would see nothing.
            // A desktop entry is a download, so name the file rather than the variant key: the
            // user is about to receive that file and will look for it by name.
            opt.textContent = f.isDesktop
                ? DESKTOP_LABEL[f.firmware] || f.firmware
                : EXPERIMENTAL_FIRMWARES.has(f.firmware)
                    ? `⚠️ ${f.firmware} (untested — no board to verify on)`
                    : f.firmware;
            firmwareEl.appendChild(opt);
        });
        // The button says what it will do. A desktop entry is downloaded and opened by the user;
        // a firmware is flashed by the device. Same control, two honest verbs.
        const selEntry = compatible.find(f => f.firmware === state.firmware) || compatible[0];
        installBtn.textContent = (selEntry && selEntry.isDesktop) ? "Download" : "Install";

        // Precedence: own firmware > last user pick > board default > first
        // compatible.
        //   1. The device's currently-flashed firmware (ownFirmwareKey) wins
        //      because the OTA picker's natural default is "re-flash what
        //      I'm running" — even if last week the user flashed something
        //      else. Only present on the on-device picker; the web installer
        //      passes null so this branch falls through.
        //   2. localStorage saved pick wins next: a returning user expects
        //      their last choice to stick across page loads, including the
        //      case where they once picked a non-default variant (e.g. esp32-eth
        //      on Olimex, whose default is esp32). Filtered through `compatible`
        //      so a stale saved value (release dropped that firmware) falls
        //      through harmlessly.
        //   3. The board's default firmware — the FIRST entry in its `firmwares`
        //      array (firmwares[0] is the default by convention). Fallback for
        //      first-time visitors.
        //   4. First option in the narrowed list — last-resort fallback.
        const savedFirmware = safeLocalGet(PREF_FIRMWARE_KEY);
        const savedHere = savedFirmware && compatible.find(f => f.firmware === savedFirmware);
        // A link naming a firmware outranks every default below it, including the running
        // one: the sender knows which image they mean.
        const linkFirmware = urlParam("firmware");
        const linkHere = linkFirmware && compatible.find(f => f.firmware === linkFirmware);
        let preferred = null;
        if (linkHere) {
            preferred = linkFirmware;
        } else if (state.ownFirmwareKey && compatible.find(f => f.firmware === state.ownFirmwareKey)) {
            preferred = state.ownFirmwareKey;
        } else if (savedHere) {
            preferred = savedFirmware;
        } else if (state.selectedBoard) {
            const board = state.boards.find(b => b.name === state.selectedBoard);
            const boardDefault = board && board.firmwares && board.firmwares[0];
            if (boardDefault && compatible.find(f => f.firmware === boardDefault)) {
                preferred = boardDefault;
            }
        }
        state.firmware = preferred || compatible[0].firmware;
        firmwareEl.value = state.firmware;
        // Single-firmware UX: when the narrow leaves exactly one option, the
        // <select> reads as a fixed badge (user sees what's being flashed but
        // can't change it — there's nothing to change to). Re-enabled at the
        // top of refreshFirmwareDropdown for the next call.
        firmwareEl.disabled = (compatible.length === 1);
        firmwareReady = true;
        applyInstallEnabled();   // a valid firmware — but still gated on the port
    }
    refreshFirmwareDropdown();

    releaseEl.addEventListener("change", () => {
        state.releaseIdx = Number(releaseEl.value);
        // Persist the tag name (not the index) — indexes shift when new releases
        // land, but tag names are stable identifiers.
        const r = sorted[state.releaseIdx];
        if (r) safeLocalSet(PREF_RELEASE_KEY, r.tag_name);
        refreshFirmwareDropdown();
    });

    firmwareEl.addEventListener("change", () => {
        state.firmware = firmwareEl.value;
        // Only a flashable variant is worth remembering. There is one desktop archive per
        // host and the OS picks it, so storing it would just overwrite the ESP32 variant a
        // returning user chose.
        if (!state.firmware.startsWith("desktop-")) safeLocalSet(PREF_FIRMWARE_KEY, state.firmware);
    });

    if (boardEl) {
        // Picking a board narrows the firmware dropdown and may pre-select
        // the board's default firmware (firmwares[0]). Persisted to localStorage so a
        // returning user (who usually flashes the same board over and over)
        // doesn't have to re-pick. Same rationale as PREF_FIRMWARE_KEY; if a
        // user is actually flashing a different board, they pick from the
        // dropdown and the new choice persists.
        boardEl.addEventListener("change", () => {
            state.selectedBoard = boardEl.value;
            safeLocalSet(PREF_BOARD_KEY, state.selectedBoard);
            refreshFirmwareDropdown();
        });
    }

    if (boardEl && state.onDetect && state.boardSupport) {
        // The detect routine: opens the serial port (via onDetect — the seam to
        // the serial/esptool code in install-orchestrator.js), reads the chip
        // family, narrows the board list to that family. The Detect BUTTON lives
        // in the host page (under the port picker), not here — the page calls
        // installPicker.runDetect() and shows the returned status string. This
        // keeps the button out of the firmware-embedded picker, and the
        // narrowing logic in the picker (which owns the board <select>).
        // Returns a status string for the caller to display ("" on success-less
        // states is never returned — applyDetectedChip always yields a message).
        state.runDetect = async (onStatus) => {
            if (onStatus) onStatus("Detecting…");
            // Clear any prior detection up front so a failed re-detect can't leave
            // the board list narrowed to a stale chip family (e.g. detect S3, then
            // a later detect fails on a wrong port — without this the list would
            // still hide the classic boards and claim an S3 was found).
            state.detectedChip = null;
            let status;
            try {
                state.detectedChip = await state.onDetect();   // "ESP32" | "ESP32-S3" | ...
                status = state.boardSupport.applyDetectedChip(state, boardEl);
            } catch (e) {
                // Restore the full, unfiltered board list — detection didn't land,
                // so don't keep any narrowing from a previous attempt.
                state.boardSupport.fillBoardOptions(boardEl, state.boards, "(any board)");
                state.selectedBoard = "";
                boardEl.value = "";
                // Detect is optional: the full catalog is still shown, so the user can
                // pick their board and flash regardless. Say so — a bare "Detect
                // failed" reads like a dead end. (A brand-new chip whose esptool-js /
                // device chip DB predates it can't be auto-identified yet, but its
                // firmware flashes fine once picked manually.)
                status = `Detect failed: ${e && e.message ? e.message : e} — pick your board below and flash anyway`;
            }
            safeLocalSet(PREF_BOARD_KEY, state.selectedBoard || "");
            refreshFirmwareDropdown();
            if (onStatus) onStatus(status);
            return status;
        };
    }

    installBtn.addEventListener("click", async () => {
        const r = sorted[state.releaseIdx];
        if (!r || !state.firmware) return;
        // Belt-and-suspenders: never flash without a picked port when the host
        // gates on one (the button is already disabled in that state, but a stale
        // enablement shouldn't let a flash through). The dropdown stays the place
        // to pick a port.
        if (state.hasPort && !state.hasPort()) { applyInstallEnabled(); return; }
        const entry = (r.firmwares || []).find(f => f.firmware === state.firmware);
        if (!entry) return;
        // Mismatch guard: if Detect ran and the user then overrode the board to
        // one of a different chip family, confirm before flashing the wrong
        // binary (which would fail at the bootloader with a cryptic error).
        // Gated on detectedChip, which is only ever set on the web installer —
        // never reached on the on-device OTA build.
        if (state.detectedChip && state.selectedBoard) {
            const board = state.boards.find(b => b.name === state.selectedBoard);
            if (board && board.chip && board.chip !== state.detectedChip
                && !confirm(`You picked ${state.selectedBoard} (${board.chip}) but the connected device is ${state.detectedChip}. Flash anyway?`)) {
                return;
            }
        }
        // Install click is the strongest "yes, this is my choice" signal, so remember it
        // explicitly in addition to the on-change writes above, in case the user reaches
        // this point without having touched the dropdowns (defaults restored, click
        // straight through). Desktop keys are excluded for the reason given at that write.
        safeLocalSet(PREF_RELEASE_KEY, r.tag_name);
        if (!entry.isDesktop) safeLocalSet(PREF_FIRMWARE_KEY, state.firmware);
        installBtn.disabled = true;
        statusEl.textContent = entry.isDesktop
            ? `Downloading ${r.tag_name}…`
            : `Installing ${r.tag_name} (${state.firmware})…`;
        try {
            await state.onInstall(state.firmware, entry.manifestUrl, entry.binaryUrl, entry);
            statusEl.textContent = entry.isDesktop
                ? `Download started. Open it to install.`
                : `Install request sent — watch device status for progress.`;
        } catch (e) {
            statusEl.textContent = `Error: ${e && e.message ? e.message : e}`;
        } finally {
            // Recompute via the gate, not a blind enable — a flash with no port
            // picked (host gates on one) must stay disabled after the attempt.
            applyInstallEnabled();
        }
    });
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

// Pure helpers exported for unit testing (test/js/installer-firmware-merge.test.mjs).
// They take data and return data — no DOM, no fetch — so a node test can exercise
// the real functions rather than a re-implemented copy.
export { parseFirmwaresFromAssets, mergeFirmwares, isCompatible };

export const installPicker = {
    /**
     * Web installer only: switch between flashing a board over USB and downloading the
     * desktop build for the machine viewing the page. The port dropdown offers "This
     * computer" as an entry alongside the USB ports, and picking it lands here.
     *
     * Reuses the on-device desktop path rather than adding a parallel one: a desktop
     * reports its firmware key as "unknown", which isCompatible() already maps to this
     * host's archive, so the labels, the Download button and the download itself all
     * follow with no second implementation. The board picker is hidden while it is on,
     * because no board is being flashed.
     */
    setDesktopMode(on) {
        const state = _lastState;
        // Called before init() finished mounting: remember it, and init applies it once the
        // state exists. Without this the host's opening default is silently dropped.
        if (!state) { _pendingDesktopMode = on; return; }
        const want = on ? "unknown" : null;
        if (state.ownFirmwareKey === want) return;
        state.ownFirmwareKey = want;
        render(state);
    },

    /**
     * Mount a picker into the given container.
     * @param {object} opts
     * @param {HTMLElement} opts.container - DOM element to mount into.
     * @param {string|null} opts.ownFirmwareKey - device's MM_FIRMWARE_NAME for
     *   OTA compatibility filtering, or null for the web installer (no filter).
     * @param {(firmware: string, manifestUrl: string, binaryUrl: string) => Promise<void>} opts.onInstall
     *   - Called when the user clicks Install. The picker doesn't decide how
     *     to install — the caller decides.
     * @param {() => Promise<string>} [opts.onDetect] - web installer only:
     *   opens the serial port and returns the connected chip-family string
     *   ("ESP32" / "ESP32-S3"). When provided, the picker renders a "Detect
     *   my board" button that narrows the board list to the detected family.
     *   Omit on the on-device OTA picker (no local serial) — the button then
     *   never renders. All serial/esptool work lives behind this callback.
     * @param {boolean} [opts.enableBoardPicker=true] - true on the web
     *   installer (renders a board <select> above firmware, narrows firmware
     *   list to the board's compatible variants); false on the on-device OTA
     *   picker where the device already knows its deviceModel (SystemModule).
     * @param {HTMLElement} [opts.installRowExtras] - optional caller-owned
     *   element rendered just above the Install button row. Web installer
     *   uses this for the "Erase chip first" checkbox; on-device OTA omits
     *   it. The picker re-attaches the SAME node on every render(), so
     *   listeners the caller wired on the element keep firing.
     * @param {object} [opts.boardSupport] - the board-catalog + chip-detection
     *   helpers ({ loadBoards, fillBoardOptions, applyDetectedChip }) from
     *   install-picker-boards.js. WEB INSTALLER ONLY — the Pages page imports
     *   that module and passes it here. Omitted on the on-device OTA picker, so
     *   the board code never has to ship in the firmware (this file embeds into
     *   the device; install-picker-boards.js does not). With no boardSupport the
     *   board <select> is simply not populated and the picker is Release+Firmware.
     *
     *   This is dependency injection: the host that needs the optional board
     *   capability supplies it, and this shared file never imports the board
     *   module itself. That keeps the dependency pointing the right way (the
     *   embedded-everywhere picker knows nothing installer-specific) AND keeps the
     *   board code physically out of the firmware — an `import` can't, since the
     *   device would then have to embed the imported file too (no bundler /
     *   tree-shaking here; embed_ui.cmake gzips this file verbatim).
     */
    async init({ container, ownFirmwareKey, onInstall, onDetect = null,
                 enableBoardPicker = true, installRowExtras = null, hasPort = null,
                 moonbaseOnly = false, extrasAfterInstall = false,
                 boardSupport = null, extraFirmwaresByTag = null }) {
        const state = makeState();
        state.container = container;
        state.ownFirmwareKey = ownFirmwareKey || null;
        state.onInstall = onInstall;
        state.onDetect = onDetect;
        state.enableBoardPicker = enableBoardPicker;
        state.installRowExtras = installRowExtras;
        state.extrasAfterInstall = extrasAfterInstall;
        // Offer the MOONBASE image instead of the app firmwares: one per chip, unversioned, and
        // installed into the factory slot. The on-device card sets this from its image selector.
        state.moonbaseOnly = moonbaseOnly;
        state.hasPort = hasPort;
        state.boardSupport = boardSupport;

        // Show the full field skeleton immediately (each select spins) instead of
        // a single "Loading…" row, so a slow GitHub fetch doesn't leave the form
        // looking empty under the Port row. render() replaces this on success;
        // the error branches below replace it with their message.
        renderSkeleton(state);
        const bypass = new URLSearchParams(location.search).get("nocache") === "1";
        // Parallel: GitHub Releases API (slow, ~200ms) + local deviceModels.json
        // (fast, ~5ms). The boards fetch only runs when the board picker is on AND
        // the host injected boardSupport (web installer) — the on-device OTA picker
        // does neither, so it skips the fetch and ships no board code.
        const [data, boards] = await Promise.all([
            loadReleases({ bypassCache: bypass }),
            (enableBoardPicker && boardSupport) ? boardSupport.loadBoards() : Promise.resolve([]),
        ]);
        state.boards = boards;
        _lastState = state;
        // Honour a mode the host chose while the release fetch was still in flight.
        if (_pendingDesktopMode !== null) {
            if (_pendingDesktopMode) state.ownFirmwareKey = "unknown";
            _pendingDesktopMode = null;
        }
        if (!data) {
            container.innerHTML =
                `<div class="control-row"><span class="control-label">Releases</span>` +
                `<span class="rp-status">Couldn't reach GitHub — refresh to retry.</span></div>`;
            return;
        }
        // Normalise: enrich each release with its parsed firmwares list, then
        // merge in any locally-staged firmwares for that tag (preview only — the
        // host injects extraFirmwaresByTag so a brand-new firmware that isn't in
        // the published release's assets yet is still flashable from local bins;
        // a local entry overrides a same-named published one).
        state.releases = data.map(r => ({
            tag_name: r.tag_name,
            name: r.name || r.tag_name,
            prerelease: !!r.prerelease,
            published_at: r.published_at || r.created_at,
            html_url: r.html_url,
            firmwares: mergeFirmwares(
                parseFirmwaresFromAssets(r.assets, r.tag_name, state.moonbaseOnly),
                extraFirmwaresByTag && extraFirmwaresByTag[r.tag_name]),
        }))
        // Drop releases with zero usable firmwares (no firmware-* / manifest-* assets).
        .filter(r => r.firmwares.length > 0);

        if (state.releases.length === 0) {
            container.innerHTML =
                `<div class="control-row"><span class="control-label">Releases</span>` +
                `<span class="rp-status">No releases with installable firmware found.</span></div>`;
            return;
        }
        render(state);
    },

    /**
     * Re-evaluate the Install button's enabled state — call from the host
     * whenever the USB-port selection changes, so the button enables once a
     * port is picked and disables again if it's cleared. No-op until the picker
     * has rendered (applyInstallEnabled is wired during render()).
     */
    notifyPortChanged() {
        if (_lastState && _lastState.applyInstallEnabled) _lastState.applyInstallEnabled();
    },

    /**
     * Returns the user-picked board name (catalog `name` field) from the
     * most recently mounted picker, or "" when the picker is in
     * "(any board)" mode, the catalog is unavailable, or the picker isn't
     * mounted yet. Used by the install-orchestrator to know what to push
     * via Improv APPLY_OP (the device-model ops are applied as REST-over-serial) after WiFi provisioning succeeds.
     */
    getSelectedBoard() {
        return _lastState ? (_lastState.selectedBoard || "") : "";
    },

    /**
     * The picked board's chip family from deviceModels.json ("ESP32-S3",
     * "ESP32-S31", …), or "" when no board is picked / not in the catalog. The
     * host uses it to special-case chips the browser flasher can't handle.
     */
    getSelectedBoardChip() {
        if (!_lastState || !_lastState.selectedBoard || !_lastState.boards) return "";
        const entry = _lastState.boards.find(b => b.name === _lastState.selectedBoard);
        return (entry && entry.chip) || "";
    },

    /**
     * The picked board's deviceModels.json TX-power cap
     * (controls.Network.txPowerSetting), or null when the board has none /
     * no board is picked. The orchestrator pushes it over Improv BEFORE
     * provisioning — brown-out-prone boards (a weak LDO / marginal supply) fail their first
     * association at full power, so the cap can't wait for the post-online
     * HTTP fan-out.
     */
    getSelectedBoardTxPower() {
        if (!_lastState || !_lastState.selectedBoard || !_lastState.boards) return null;
        const entry = _lastState.boards.find(b => b.name === _lastState.selectedBoard);
        // New catalog shape: each board's modules list carries its own controls;
        // the WiFi TX-power cap lives on the Network module's controls block.
        const net = entry && (entry.modules || []).find(m => m && m.id === "Network");
        const v = net && net.controls && net.controls.txPowerSetting;
        // The SET_TX_POWER RPC validates a whole-dBm value in 0..21 (platform.h);
        // reject anything outside that so a bad catalog value can't poison the
        // brown-out mitigation path.
        return (Number.isInteger(v) && v >= 0 && v <= 21) ? v : null;
    },

    /**
     * Chip family from the last successful Detect ("ESP32" / "ESP32-S3"), or ""
     * if Detect hasn't run / failed / isn't wired. Always "" on the on-device
     * OTA picker (no onDetect). Currently informational; the mismatch guard at
     * install time reads state.detectedChip directly.
     */
    getDetectedChip() {
        return _lastState ? (_lastState.detectedChip || "") : "";
    },

    /**
     * Forget the last Detect's chip — call when the user re-picks a port, so the
     * port dropdown doesn't briefly show the previous port's chip before the
     * fresh detect lands. runDetect() also clears it at its start; this covers
     * the window before that runs.
     */
    clearDetectedChip() {
        if (_lastState) _lastState.detectedChip = null;
    },

    /**
     * Detect the connected chip and narrow the board list to its family. Called
     * by the host's "Detect my board" button (under the port picker) and auto-
     * fired after a fresh port grant (the ESP Web Tools / ESPHome model: detect
     * immediately on connect). `onStatus(text)` is invoked with "Detecting…"
     * then the final status ("Detected ESP32-S3 — …" / "Detect failed: …") so
     * the page can show progress; the final string is also returned. No-op
     * returning "" if the picker isn't mounted or detect isn't wired (on-device
     * OTA). Never throws — failures come back as a status string.
     */
    async runDetect(onStatus) {
        if (_lastState && _lastState.runDetect) return await _lastState.runDetect(onStatus);
        return "";
    },
};
