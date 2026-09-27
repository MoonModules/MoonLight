// MoonLight web installer logic. Extracted from index.html's inline module script.
// A static GitHub Pages page, so an external module is free.

// Shared install-picker (release → board → firmware). Same file as the
// on-device OTA UI uses; only the onInstall callback differs:
//   - Device UI: POST the chosen .bin URL to /api/firmware/url; device
//     fetches the binary directly via esp_https_ota.
//   - Web installer (here): hand the manifest URL to the orchestrator,
//     which flashes via esptool-js then provisions WiFi via Improv,
//     all over the same SerialPort.
//
// Manifests + binaries must be same-origin with this page (Web Serial
// would happily flash from any URL, but the manifest fetch + part
// downloads via fetch() are subject to CORS). The release workflow
// self-hosts the last N releases into pages/install/releases/<tag>/.
// toLocalUrl rewrites the picker's absolute GitHub URLs to the local
// copies before handing them to the orchestrator.
import { installPicker } from "./install-picker.js";
import { myDevices }    from "./devices.js";
import { installer, ESPTOOL_JS_VERSION } from "./install-orchestrator.js";
// Board catalog + chip detection — mooninstaller only, kept out of the
// firmware-embedded install-picker.js and injected here via boardSupport.
import * as boardSupport from "./install-picker-boards.js";
import { BACKUP_SNIPPET } from "./backup-snippet.js";

// Windows-only hints (was a separate inline <script> in <head>): reveal .windows-only
// rows when the UA is Windows.
// The backup card: the bookmarklet IS the snippet (a javascript: href), the copy button the
// fallback for browsers without a draggable bookmarks bar. Wired at module top level (a module
// script runs after the DOM is parsed), NOT inside the installer's init below: the card must
// work even in a browser where installer init throws (Safari has no Web Serial), because
// backing up is exactly what a non-flashing browser can still do.
(() => {
  const link = document.getElementById("backup-bookmarklet");
  const copy = document.getElementById("backup-copy");
  if (!link || !copy) return;
  link.href = BACKUP_SNIPPET;
  link.addEventListener("click", (ev) => ev.preventDefault());   // run on the DEVICE page, not here
  copy.addEventListener("click", async () => {
    const text = BACKUP_SNIPPET.slice("javascript:".length);
    let ok = false;
    try { await navigator.clipboard.writeText(text); ok = true; }
    catch (_) {
      // http-over-LAN or older Safari: no async clipboard, the selection fallback.
      const ta = document.createElement("textarea");
      ta.value = text;
      ta.style.cssText = "position:fixed;opacity:0";
      document.body.appendChild(ta);
      ta.select();
      try { ok = document.execCommand("copy"); } catch (_) {}
      ta.remove();
    }
    copy.textContent = ok ? "Copied ✓" : "Copy failed, copy the link by hand";
  });
})();

document.addEventListener('DOMContentLoaded', () => {
  if (/Windows/i.test(navigator.userAgent)) {
    document.querySelectorAll('.windows-only').forEach(el => el.hidden = false);
  }
});


    // Show the project version next to the heading. library.json ships
    // alongside index.html (preview_installer.py + release.yml both copy
    // it). Fetch silently — if it's missing for any reason, leave the
    // chip hidden rather than rendering "?" noise.
    (async () => {
        try {
            const res = await fetch("./library.json");
            if (!res.ok) return;
            const lib = await res.json();
            if (!lib || !lib.version) return;
            const chip = document.getElementById("version-chip");
            chip.textContent = `v${lib.version}`;
            chip.hidden = false;
        } catch (_) { /* silent: cosmetic-only */ }
    })();

    // Show the pinned esptool-js version in the footer credit — so a flash failure
    // can be tied to the exact flasher build (e.g. the 0.6.0 compressed-write
    // regression). Sourced from the orchestrator's exported constant, the same
    // place the import URL is pinned.
    {
        const el = document.getElementById("esptool-version");
        if (el) el.textContent = ` v${ESPTOOL_JS_VERSION}`;
    }

    // Map a GitHub release-asset URL to its Pages-hosted mirror.
    //   https://github.com/MoonModules/projectMM/releases/download/<TAG>/<file>
    //   → ./releases/<TAG>/<file>
    function toLocalUrl(githubUrl) {
      const m = /\/releases\/download\/([^/]+)\/([^/]+)$/.exec(githubUrl);
      if (!m) return githubUrl;  // unrecognised shape: pass through unchanged
      const [, tag, name] = m;
      //   https://github.com/.../releases/download/<TAG>/<file>
      //   → ./releases/<TAG>/<file>  (same-origin, served from this dir)
      return `./releases/${tag}/${name}`;
    }

    // --- Install modal section toggling ---------------------------------
    const backdrop = document.getElementById("install-backdrop");
    const title = document.getElementById("install-title");
    const sections = {
      connecting:   document.getElementById("section-connecting"),
      wrongPort:    document.getElementById("section-wrong-port"),
      flashing:     document.getElementById("section-flashing"),
      wifiForm:     document.getElementById("section-wifi-form"),
      provisioning: document.getElementById("section-provisioning"),
      needsIp:      document.getElementById("section-needs-ip"),
      done:         document.getElementById("section-done"),
      error:        document.getElementById("section-error"),
    };
    function showSection(name) {
      for (const [k, el] of Object.entries(sections)) {
        el.classList.toggle("active", k === name);
      }
    }
    function openModal(titleText) {
      title.textContent = titleText;
      backdrop.classList.add("open");
      // Reset the log per install session so users see only the current run.
      document.getElementById("install-log").textContent = "";
      // Reset toggle to collapsed state.
      const log = document.getElementById("install-log");
      const toggle = document.getElementById("log-toggle");
      log.hidden = true;
      toggle.textContent = "Show log";
      // Reset the needs-ip dialog to its idle state — covers the case where
      // a prior install ended in retry-success (which leaves the disabled +
      // spinner state intact, since the WiFi-creds form took over the
      // visible card) and the next install hits the needs-ip path again.
      showNeedsIpRetrying(false);
      // Lock out the monitor button while an install runs. Web Serial
      // grants exclusive port access; a Monitor click mid-flash would
      // race the install for the SerialPort and either steal it
      // (corrupting the flash) or get a misleading "already open" error.
      // closeModal re-enables.
      monitorBtn.disabled = true;
      // Guard against accidental tab-close mid-flash. Browser shows a
      // generic "leave site?" prompt (Chrome ignores the custom text since
      // 2017 — security hardening). disarmUnloadGuard() runs on done /
      // error / cancel so the user can close the page when it's safe.
      armUnloadGuard();
    }
    function closeModal() {
      backdrop.classList.remove("open");
      disarmUnloadGuard();
      monitorBtn.disabled = false;
      // Wipe the password field on every modal close — success, cancel,
      // or error. The form is built lazily and re-used across installs in
      // the same page session; without this the typed password lingers
      // in the live `.value`. (We don't clear `defaultValue` — it stayed
      // as the form's initial empty string from buildWifiForm's innerHTML
      // assignment; setting `.value` clears the rendered value, which is
      // the only thing visible to a script reading the DOM after close.)
      // Same wipe runs at the start of each `wifi-creds-form` show, but
      // having both belts ensures the wipe runs even when the user
      // bypasses the form via Skip or the install fails before reaching
      // the creds step.
      const passEl = document.getElementById("wifi-password");
      if (passEl) passEl.value = "";
    }
    let unloadGuard = null;
    function armUnloadGuard() {
      if (unloadGuard) return;
      unloadGuard = (e) => {
        e.preventDefault();
        e.returnValue = "";  // legacy contract — Chrome ignores the string
      };
      window.addEventListener("beforeunload", unloadGuard);
    }
    function disarmUnloadGuard() {
      if (!unloadGuard) return;
      window.removeEventListener("beforeunload", unloadGuard);
      unloadGuard = null;
    }

    // Only http(s) URLs are safe as a link href (same guard as
    // myDevices.addProvisionedDevice) — a malformed value must never become a
    // javascript:/data:/file: link.
    function isHttpUrl(u) {
      try { const p = new URL(u); return p.protocol === "http:" || p.protocol === "https:"; }
      catch (_) { return false; }
    }
    document.getElementById("done-close").addEventListener("click", closeModal);
    document.getElementById("error-close").addEventListener("click", closeModal);

    // Board-details popup close button. The <dialog> also closes on ESC and a
    // backdrop click (native); this wires the explicit ✕. A click on the dialog
    // backdrop (the element itself, outside .bd-body) closes it too.
    const bd = document.getElementById("board-details");
    document.getElementById("bd-close").addEventListener("click", () => bd.close());
    bd.addEventListener("click", (e) => { if (e.target === bd) bd.close(); });

    // Log panel: collapsible, fed by the orchestrator's onLog callback.
    // Auto-scrolls to bottom on append so the latest line is always visible.
    const logEl = document.getElementById("install-log");
    const logToggle = document.getElementById("log-toggle");
    logToggle.addEventListener("click", () => {
      logEl.hidden = !logEl.hidden;
      logToggle.textContent = logEl.hidden ? "Show log" : "Hide log";
    });
    function appendLog(line) {
      logEl.textContent += line + "\n";
      logEl.scrollTop = logEl.scrollHeight;
    }

    // --- Orchestrator UI bindings --------------------------------------
    // Mapping from orchestrator's progress stage → which modal section to
    // show + any per-stage detail rendering. Centralizes the UI ↔ flow
    // contract so the orchestrator stays UI-agnostic.
    function handleProgress(stage, detail) {
      switch (stage) {
        case "request-port":
          showSection("connecting");
          document.getElementById("connecting-detail").textContent =
            "Pick the serial port the device is connected to.";
          break;
        case "wrong-port-retry":
          // Distinct section with a Try-again button gates the OS-picker
          // re-prompt so the user reads the guidance BEFORE the OS modal
          // covers the page (the picker's a native dialog, no way around
          // its overlay; if we just updated connecting-detail and went
          // straight to requestPort(), the message would never be seen).
          showSection("wrongPort");
          break;
        case "connect-flash":
          showSection("connecting");
          document.getElementById("connecting-detail").textContent =
            detail && detail.chipName
              ? `Detected ${detail.chipName}`
              : "Detecting chip…";
          break;
        case "fetch-firmware":
          showSection("flashing");
          document.getElementById("flashing-status").textContent =
            "Downloading firmware…";
          document.getElementById("flash-bar").style.width = "0%";
          break;
        case "erase": {
          showSection("flashing");
          document.getElementById("flashing-status").textContent =
            "Erasing flash (this takes ~12 s)…";
          // esptool-js's eraseFlash doesn't report progress — animate the
          // bar so the user sees the page isn't hung. flash stage clears
          // the class below and switches to determinate progress.
          //
          // Clear the inline width set by an earlier stage (fetch-firmware
          // forces `style.width = "0%"`); inline style beats the CSS class's
          // `width: 100%` rule, so the animation would run invisibly on a
          // zero-width bar without this reset.
          const bar = document.getElementById("flash-bar");
          bar.style.width = "";
          bar.classList.add("indeterminate");
          break;
        }
        case "flash": {
          showSection("flashing");
          const pct = detail && typeof detail.pct === "number" ? detail.pct : 0;
          const bar = document.getElementById("flash-bar");
          bar.classList.remove("indeterminate");
          document.getElementById("flashing-status").textContent =
            `Writing firmware… ${pct}%`;
          bar.style.width = `${pct}%`;
          break;
        }
        case "reboot":
          showSection("connecting");
          document.getElementById("connecting-detail").textContent =
            "Rebooting device…";
          break;
        case "connect-improv":
          showSection("connecting");
          document.getElementById("connecting-detail").textContent =
            "Connecting over Improv…";
          break;
        case "wifi-creds-form": {
          buildWifiForm();
          showSection("wifiForm");
          // Prefill the last-used SSID (not password — that stays out of
          // localStorage as a privacy / security-scanner concession). Focus
          // password if SSID is prefilled (user only needs to type the
          // password), else focus SSID.
          const ssidEl = document.getElementById("wifi-ssid");
          const passEl = document.getElementById("wifi-password");
          // Clear the password input on every show. The form is built
          // lazily once and re-used across multiple installs in the same
          // page session; without this clear, a re-install would show the
          // previous install's typed password in plaintext (well, dotted)
          // inside the field. Don't leave that sitting in the DOM longer
          // than the active install.
          passEl.value = "";
          try {
            const saved = localStorage.getItem("MoonLight.installer.lastSsid");
            if (saved) ssidEl.value = saved;
          } catch (_) { /* hostile storage */ }
          requestAnimationFrame(() => (ssidEl.value ? passEl : ssidEl).focus());
          break;
        }
        case "provisioning":
          showSection("provisioning");
          document.getElementById("provisioning-status").textContent =
            "Connecting to your WiFi…";
          break;
        case "set-board":
          showSection("provisioning");
          document.getElementById("provisioning-status").textContent =
            "Setting board identity…";
          break;
        case "apply-defaults":
          showSection("provisioning");
          document.getElementById("provisioning-status").textContent =
            detail && detail.board
              ? `Applying device defaults for ${detail.board}…`
              : "Applying device defaults…";
          break;
        case "needs-ip":
          showSection("needsIp");
          // Focus the input so the user can start typing immediately.
          requestAnimationFrame(() => {
            const el = document.getElementById("needs-ip-input");
            if (el) el.focus();
          });
          break;
        case "done":
          // onSuccess fires next; section toggles there once URL is known.
          break;
      }
    }

    // Render the WiFi-creds form into #section-wifi-form on first use.
    // Idempotent — re-installs after the first run are no-ops. See the
    // comment on #section-wifi-form's empty div for why this is deferred
    // (avoids the macOS iCloud Passwords prompt on page load).
    //
    // Attributes on the injected inputs:
    // - autocomplete="off": defeats Chrome's built-in field-fill (the
    //   browser's own offer; not the OS keychain).
    // - data-lpignore / data-1p-ignore: vendor opt-outs for LastPass /
    //   1Password's browser-extension fill icons.
    // - autocomplete="off" on the password (NOT "new-password"):
    //   "new-password" actively signals password managers "credential
    //   being created, offer a generated value", which is wrong here —
    //   the user is typing their existing home-WiFi password, not
    //   creating a new account.
    //
    // The OS keychain prompt (iCloud Passwords) ignores all of the above
    // when it scans the DOM at page load; deferring the form is the only
    // reliable way to keep it quiet until the user is actually mid-install.
    let _wifiFormBuilt = false;
    function buildWifiForm() {
      if (_wifiFormBuilt) return;
      _wifiFormBuilt = true;
      const section = document.getElementById("section-wifi-form");
      section.innerHTML = `
        <div class="install-status">Flash done. Enter your WiFi credentials to provision the device.</div>
        <form id="wifi-form" class="install-form" onsubmit="return false">
          <label for="wifi-ssid">Network name (SSID)</label>
          <input type="text" id="wifi-ssid" autocomplete="off"
                 data-lpignore="true" data-1p-ignore required>
          <label for="wifi-password">Password</label>
          <input type="password" id="wifi-password" autocomplete="off"
                 data-lpignore="true" data-1p-ignore>
          <div class="install-actions">
            <button type="button" class="secondary" id="wifi-skip">Skip</button>
            <button type="submit" class="primary" id="wifi-connect">Connect</button>
          </div>
        </form>
      `;
    }

    // Returns a Promise the orchestrator awaits — resolves when the user
    // submits the WiFi form. The form's Connect button completes via the
    // form's submit event; the Skip button bypasses WiFi (the device
    // falls back to its AP). Skip resolves with empty creds — the
    // orchestrator's provision() with empty SSID will fail fast and the
    // user falls back to the AP flow as documented in Step 2.
    function uiWaitForCreds() {
      return new Promise((resolve) => {
        const form = document.getElementById("wifi-form");
        const ssidEl = document.getElementById("wifi-ssid");
        const passEl = document.getElementById("wifi-password");
        const skipBtn = document.getElementById("wifi-skip");

        const onSubmit = (e) => {
          e.preventDefault();
          cleanup();
          const ssid = ssidEl.value.trim();
          // Save SSID only — password stays out of localStorage. WiFi SSIDs
          // are network-visible anyway (any scanning device sees them);
          // passwords are not.
          try {
            if (ssid) localStorage.setItem("MoonLight.installer.lastSsid", ssid);
          } catch (_) { /* hostile storage */ }
          resolve({ ssid, password: passEl.value });
        };
        const onSkip = () => {
          cleanup();
          resolve({ ssid: "", password: "" });
        };
        const cleanup = () => {
          form.removeEventListener("submit", onSubmit);
          skipBtn.removeEventListener("click", onSkip);
        };
        form.addEventListener("submit", onSubmit);
        skipBtn.addEventListener("click", onSkip);
      });
    }

    // Returns a Promise that resolves with one of:
    //   { action: "ip", url: "<typed value>" }    — user typed an IP + Add
    //   { action: "skip" }                        — user clicked Skip
    //   { action: "retry" }                       — user clicked Try Improv again
    // Shown when the orchestrator's probe-open on the user's pre-picked
    // port fails. The OS port picker is modal and covers the install
    // modal, so any guidance written and immediately followed by
    // requestPort() is invisible — gate the re-prompt behind a Try
    // again click so the message lands before the OS picker covers
    // the page. Resolves when the user clicks Try again.
    function uiWaitForPortRetry() {
      return new Promise((resolve) => {
        const btn = document.getElementById("wrong-port-retry");
        const onClick = () => {
          btn.removeEventListener("click", onClick);
          resolve();
        };
        btn.addEventListener("click", onClick);
      });
    }

    // Symmetric with uiWaitForCreds. Normalisation of the typed value lives
    // in the orchestrator so the host page stays UI-only. The retry action
    // is the orchestrator's signal to re-run Improv `initialize()`; while
    // that's in flight the host re-shows the dialog via showNeedsIpRetrying()
    // (input + buttons disabled, "Trying Improv again…" status visible).
    function uiWaitForIp() {
      return new Promise((resolve) => {
        const form = document.getElementById("needs-ip-form");
        const input = document.getElementById("needs-ip-input");
        const skipBtn = document.getElementById("needs-ip-skip");
        const retryBtn = document.getElementById("needs-ip-retry");

        const onSubmit = (e) => {
          e.preventDefault();
          // Reject whitespace-only input here so the user sees the
          // browser's built-in validation tooltip on the same field
          // instead of a silent re-prompt after the orchestrator
          // discovers an empty normalized URL downstream. `required`
          // on the input catches empty submits but not "   " — trim
          // first, then use setCustomValidity to surface the same UI.
          const trimmed = input.value.trim();
          if (!trimmed) {
            input.setCustomValidity("Enter an IP address or hostname.");
            input.reportValidity();
            input.setCustomValidity("");  // clear so the next valid submit proceeds
            return;
          }
          cleanup();
          resolve({ action: "ip", url: trimmed });
        };
        const onSkip = () => {
          cleanup();
          resolve({ action: "skip" });
        };
        const onRetry = () => {
          cleanup();
          resolve({ action: "retry" });
        };
        const cleanup = () => {
          form.removeEventListener("submit", onSubmit);
          skipBtn.removeEventListener("click", onSkip);
          retryBtn.removeEventListener("click", onRetry);
        };
        form.addEventListener("submit", onSubmit);
        skipBtn.addEventListener("click", onSkip);
        retryBtn.addEventListener("click", onRetry);
      });
    }

    // Toggle the needs-ip dialog into "retry in flight" mode: input + all
    // three buttons disabled, retry status line visible, input field
    // cleared so the user starts fresh if retry fails. The orchestrator
    // calls this before re-running Improv `initialize()`; on completion
    // (success or failure) it returns to the normal `uiWaitForIp()` call,
    // which by re-rendering the dialog implicitly re-enables everything.
    function showNeedsIpRetrying(retrying) {
      const form = document.getElementById("needs-ip-form");
      const input = document.getElementById("needs-ip-input");
      const skipBtn = document.getElementById("needs-ip-skip");
      const retryBtn = document.getElementById("needs-ip-retry");
      const addBtn = document.getElementById("needs-ip-add");
      const status = document.getElementById("needs-ip-retry-status");
      input.disabled = retrying;
      skipBtn.disabled = retrying;
      retryBtn.disabled = retrying;
      addBtn.disabled = retrying;
      status.hidden = !retrying;
      if (retrying) input.value = "";
    }

    function handleSuccess({ url, mdns, board, applyDefaults = true, defaultsApplied = false, viaHttp, alreadyOnline, ethOnlyNoLink }) {
      disarmUnloadGuard();
      // Clear the amber notice styling from any prior install in this session — only the
      // eth-only-no-link branch re-adds it, so every other outcome shows the plain note.
      document.getElementById("done-defaults").classList.remove("install-done-note--notice");
      // Default header: most done outcomes mean the device reported an IP (it's reachable).
      // The eth-only-no-link branch overrides it (the device isn't online until a cable is in).
      document.getElementById("done-status").textContent = "Device is online!";
      // Device-model defaults are applied DURING the install over serial (Improv =
      // REST over serial — APPLY_OP). The success screen just confirms + links;
      // the device is already fully configured by the time it shows.
      if (ethOnlyNoLink) {
        // Ethernet-only firmware flashed, but no cable was connected so the device isn't
        // online yet (and this build has no WiFi to provision). Defaults WERE pushed over
        // serial. Tell the user the one thing they need to do: plug in Ethernet — the device
        // then comes online on its own and the link appears at its IP / <name>.local.
        showSection("done");
        document.getElementById("done-status").textContent = "Flashed — connect Ethernet";
        document.getElementById("done-url").removeAttribute("href");
        document.getElementById("done-url").textContent = "";
        document.getElementById("done-url-mdns").hidden = true;
        const note = document.getElementById("done-defaults");
        // The device-model defaults are pushed over SERIAL during install (no network needed), so on
        // this eth-only path they normally DID apply — report that honestly. The only "next step" is
        // the network cable. Append a defaults caveat only when the push actually didn't run.
        const defaultsNote = defaultsApplied
          ? ` ${board} defaults were applied.`
          : (board && applyDefaults ? ` ${board} defaults weren't applied — apply them later from MoonDeck.` : "");
        note.textContent =
          `Flashed. This is an Ethernet-only firmware — connect a network cable and the device comes online on its own (find it via its IP or <name>.local, or in MoonDeck).${defaultsNote}`;
        note.classList.add("install-done-note--notice");   // amber: flashed OK, action needed (plug in Ethernet)
        note.hidden = false;
        return;
      }
      if (!url) {
        // No device URL (user skipped the IP prompt, or an eth-only/no-Improv device).
        // On that path no serial config push happened. If a model was picked, say so —
        // it can be applied later from MoonDeck on the LAN — else just close.
        if (board && applyDefaults) {
          showSection("done");
          // No device address → not online; don't claim "Device is online!" (the
          // default header set above).
          document.getElementById("done-status").textContent = "Flashed";
          document.getElementById("done-url").removeAttribute("href");
          document.getElementById("done-url").textContent = "";
          document.getElementById("done-url-mdns").hidden = true;
          document.getElementById("done-defaults").textContent =
            `Flashed. ${board} defaults weren't applied (no device address) — apply them later from MoonDeck on your network.`;
          document.getElementById("done-defaults").hidden = false;
          return;
        }
        closeModal();
        return;
      }
      showSection("done");
      // Always show the IP link (guaranteed to work). When the boot serial also
      // reported the device's <deviceName>.local name, show it as a second link —
      // it survives a DHCP lease change but only resolves where mDNS works.
      // Only assign href for http(s) URLs (same guard as myDevices.addProvisionedDevice),
      // so a malformed boot-log value can't become a javascript:/data: link.
      const a = document.getElementById("done-url");
      a.textContent = url;
      if (isHttpUrl(url)) a.href = url; else a.removeAttribute("href");
      const aMdns = document.getElementById("done-url-mdns");
      if (mdns) {
        const mdnsUrl = `http://${mdns}/`;
        aMdns.textContent = mdnsUrl;
        if (isHttpUrl(mdnsUrl)) aMdns.href = mdnsUrl; else aMdns.removeAttribute("href");
        aMdns.hidden = false;
      } else {
        aMdns.hidden = true;
      }
      // Report the defaults outcome HONESTLY, from `defaultsApplied` (did the serial
      // push actually run?), not `applyDefaults` (the checkbox intent): applied,
      // wanted-but-couldn't (model picked + ticked but no push happened — e.g. an
      // Improv-less path), or kept-config (unticked).
      const note = document.getElementById("done-defaults");
      if (defaultsApplied) {
        note.textContent = `Applied ${board} defaults.`;
        note.hidden = false;
      } else if (board && applyDefaults) {
        note.textContent = `Flashed, but ${board} defaults weren't applied — apply them from MoonDeck on your network.`;
        note.hidden = false;
      } else if (board) {
        note.textContent = `Kept the device's existing config (device defaults not applied).`;
        note.hidden = false;
      } else {
        note.hidden = true;
      }
      // Store no board unless the defaults actually applied, so the saved entry doesn't
      // claim a model the device wasn't configured to.
      myDevices.addProvisionedDevice(url, defaultsApplied ? board : "");
    }

    // Every chip MoonLight ships is browser-flashable as of esptool-js 0.7.0, which added the
    // ESP32-S31 target and chip-id detection (GET_SECURITY_INFO) so the S31's magic collision with
    // the classic ESP32 can no longer mis-identify it. The set stays because a NEW chip lands here
    // before esptool-js knows it, and an empty set is the honest way to say "none, today".
    // The CLI (esptool.py) remains the fallback the error path points at.
    const WEB_FLASH_UNSUPPORTED_CHIPS = new Set();

    // Map a firmware key to its chip family ("esp32s31" → "ESP32-S31", "esp32s3-n16r8"
    // → "ESP32-S3", "esp32p4rev1-eth" → "ESP32-P4", "esp32*" → "ESP32") — the same prefix
    // vocabulary build_esp32's TARGET_TO_FAMILY uses. Used as the chip fallback when no
    // board is picked (OTA / generic flash). "" for an unrecognised key.
    function firmwareToChip(firmware) {
      const m = String(firmware || "").match(/^esp32(s31|s3|s2|p4|c\d+|h\d+)/);
      return m ? (m[1] === "s31" ? "ESP32-S31" : "ESP32-" + m[1].toUpperCase()) : (firmware ? "ESP32" : "");
    }

    function handleError(stage, error) {
      disarmUnloadGuard();
      console.error("[install]", stage, error);
      showSection("error");
      // A connect-flash failure on a chip esptool-js can't identify reads as a
      // bare "Timeout". If the picked board is one of those chips, say so and
      // point at the CLI rather than leaving the user staring at a timeout.
      // Prefer the picked board's chip; fall back to the chip the flashing
      // firmware name implies (e.g. "esp32s31" → "ESP32-S31") when no board was
      // picked, so the guidance still fires on the OTA / generic-flash path.
      const chip = installPicker.getSelectedBoardChip() || firmwareToChip(_flashingFirmware);
      const webUnsupported = stage === "connect-flash"
        && WEB_FLASH_UNSUPPORTED_CHIPS.has(chip);
      document.getElementById("error-message").textContent = webUnsupported
        ? `Browser flashing isn't supported for ${chip} yet — the browser flasher `
          + `(esptool-js) has no chip definition for it. Flash with esptool.py instead `
          + `(which does support ${chip}); no dev environment needed:\n\n`
          + `1. Install esptool:   pipx install esptool   (or: pip install esptool)\n`
          + `2. Download the 4 firmware-esp32s31-*.bin files from the release below.\n`
          + `3. Flash (replace <your-port>, e.g. /dev/ttyUSB0 or COM3):\n\n`
          + `   esptool --chip esp32s31 --port <your-port> --baud 460800 write-flash \\\n`
          + `     0x2000  firmware-esp32s31-<ver>-bootloader.bin \\\n`
          + `     0x8000  partition-table-16mb.bin \\\n`
          + `     0xe000  shared-ota-data.bin \\\n`
          + `     0x10000 firmware-esp32s31-<ver>.bin\n\n`
          + `Then re-open this page to provision WiFi, or use the device's own UI. `
          + `Once running, the device updates itself over-the-air — no esptool needed again.`
        : `Stage: ${stage}\n${error && error.message ? error.message : error}`;
    }

    // --- Pre-pick port -------------------------------------------------
    // Two-option dropdown: the currently-picked port (or a "Pick a port…"
    // placeholder), and a "Pick another port…" sentinel that opens the
    // browser's Web Serial picker. We never list multiple previously-
    // granted ports — Web Serial doesn't expose OS device names, so a list
    // of "Port 1 / Port 2 / Port 3" is just confusing. The native picker
    // is the only place where the user can match a port to a physical
    // device (it shows the OS device name there).
    //
    // We also don't pre-select from `navigator.serial.getPorts()` on page
    // load. A surviving grant is not a guarantee the port is still openable
    // — if the device re-enumerated (reboot, replug, host sleep), `open()`
    // fails with "Failed to open serial port" mid-install. Cheaper UX to
    // always have the user re-pick this session than to chase a confusing
    // mid-flash error. installer.start() falls back to its own requestPort()
    // prompt if the user clicks Install without pre-picking.
    //
    // `?autoport=1` opts into the pre-select anyway, for a caller that knows the
    // grant is fresh: a recorded walkthrough, or a rig flashing the same board
    // repeatedly. It can only ever select a port this browser already granted, and
    // the stale-handle risk above is the opt-in's to carry: a failed open surfaces
    // as the same mid-flash error, with the same Try-again path out of it.
    let pickedPort = null;
    // Desktop mode: the user is not flashing a board over USB, they are downloading the build
    // for the computer viewing this page. Same picker, same release list; only the target
    // differs. Kept beside pickedPort because the two are mutually exclusive states of the
    // one "what am I installing onto" question the Port row asks.
    let desktopMode = false;   // this page is for flashing an ESP32; the desktop download is the alternative
    const portSelect = document.getElementById("port-select");
    const PICK_NEW = "__pick_new__"; // sentinel value for the "pick another" option
    const DESKTOP = "__desktop__";   // sentinel value for the "this computer" option
    const IDLE = "__idle__";         // neutral placeholder: nothing chosen yet

    // Name this computer the way the picker names its archive, or null on an OS we do not
    // package, in which case the option is not offered at all rather than offering a
    // download that cannot run.
    function thisComputerLabel() {
        const p = (navigator.platform || "") + " " + (navigator.userAgent || "");
        if (/Win/i.test(p)) return "Windows x64";
        if (/Mac/i.test(p)) return "macOS arm64";
        if (/Linux|X11/i.test(p)) return "Linux x64";
        return null;
    }

    function rebuildPortSelect() {
      portSelect.replaceChildren();
      // No-port state: a single "Pick a port…" option. The select-as-button
      // shape lets the change-event fire when the user re-picks the same
      // option after cancelling the picker (so the picker can be reopened
      // without an extra "Pick another" row to collapse to). Browsers fire
      // `change` when the chosen <option> changes; clicking the lone option
      // when it's already selected doesn't fire — handled by the click
      // listener below.
      // The "this computer" option, offered in every state: a visitor with no board
      // attached is the exact person who wants the desktop build, and they should not
      // have to grant a serial port to discover it exists.
      const desktopLabel = thisComputerLabel();
      const addDesktopOpt = () => {
        if (!desktopLabel) return;
        const opt = document.createElement("option");
        opt.value = DESKTOP;
        opt.textContent = `This computer (${desktopLabel})`;
        portSelect.appendChild(opt);
      };
      if (!pickedPort) {
        // A neutral placeholder sits first and stays selected until the user chooses. It is
        // what makes every pick a real `change` event, including re-picking the same option
        // after cancelling the OS prompt.
        const idleOpt = document.createElement("option");
        idleOpt.value = IDLE;
        idleOpt.textContent = "Choose where to install…";
        portSelect.appendChild(idleOpt);
        // Offer the USB choice only where it can work. Without Web Serial the option would
        // open nothing, so a browser that cannot flash shows only what it can do.
        if ("serial" in navigator) {
          const pickOpt = document.createElement("option");
          pickOpt.value = PICK_NEW;
          pickOpt.textContent = "An ESP32 on a USB port…";
          portSelect.appendChild(pickOpt);
        }
        addDesktopOpt();
        portSelect.value = desktopMode ? DESKTOP : IDLE;
        return;
      }
      // Picked state: name the selected port by the chip Detect read from it
      // ("Port selected — ESP32-S3") instead of a bare "Port selected", so the
      // user can see WHICH device is on the port. Empty until detect() lands (or
      // if it failed); the separate detect-status line carries the matched board.
      const currentOpt = document.createElement("option");
      currentOpt.value = "current";
      const chip = installPicker.getDetectedChip();
      currentOpt.textContent = chip ? `Port selected — ${chip}` : "Port selected";
      portSelect.appendChild(currentOpt);
      const pickOpt = document.createElement("option");
      pickOpt.value = PICK_NEW;
      pickOpt.textContent = "Pick another USB port…";
      portSelect.appendChild(pickOpt);
      addDesktopOpt();
      portSelect.value = desktopMode ? DESKTOP : "current";
    }

    // Reflect the port state in the picker's Install gate after every rebuild
    // (init, post-pick, and any cancel that leaves pickedPort null). Wrapped so
    // both rebuildPortSelect() exits and all call sites stay covered by one hook.
    // Take a port this browser already granted, where the URL asked for it. Runs once,
    // after the first rebuild, so a failure leaves the picker in its normal no-port state.
    async function autoSelectGrantedPort() {
      try {
        if (new URLSearchParams(window.location.search).get("autoport") !== "1") return;
        if (pickedPort || !navigator.serial) return;
        const ports = await navigator.serial.getPorts();
        // Re-checked after the await: the guard above ran before it, so a port the user
        // picked while this was resolving would otherwise be overwritten with ports[0].
        if (!ports.length || pickedPort) return;
        pickedPort = ports[0];
        desktopMode = false;
        syncPortState();
      } catch {
        // No grant, no port, or no permission: the picker stays as it was.
      }
    }

    function syncPortState() {
      rebuildPortSelect();
      installPicker.setDesktopMode(desktopMode);
      installPicker.notifyPortChanged();
      // The serial-only controls have no meaning when the target is this computer: there is
      // no chip to erase and no port to monitor. Hidden rather than disabled, because a
      // greyed "Erase chip first" next to a Mac download invites the wrong question.
      // The board grid picks an ESP32 to flash; in desktop mode the target is this computer,
      // and a stale board selection would also narrow the firmware list to ESP32 variants.
      const boardCard = document.getElementById("board-grid-card");
      if (boardCard) boardCard.hidden = desktopMode;
      const eraseRow = document.getElementById("erase-row");
      // The button's own control-row, so hiding it doesn't leave an empty row behind.
      const monitorRow = document.getElementById("monitor-btn")?.closest(".control-row");
      if (eraseRow) eraseRow.hidden = desktopMode;
      if (monitorRow) monitorRow.hidden = desktopMode;
    }

    syncPortState();
    autoSelectGrantedPort();   // no-op without ?autoport=1, and never blocks the initial render

    let portPromptOpen = false;   // one OS prompt at a time (see the click handler below)
    async function openPortPicker() {
      if (portPromptOpen) return;
      portPromptOpen = true;
      let granted = false;
      try {
        pickedPort = await navigator.serial.requestPort({});
        granted = true;
        // A pending detect() handle is bound to the OLD port — drop it so the
        // next Detect/Install opens the newly-picked port cleanly. Also forget
        // the old port's detected chip so the dropdown doesn't flash it before
        // the fresh detect lands.
        await installer.clearDetected();
        installPicker.clearDetectedChip();
      } catch (_) {
        // User cancelled the picker — keep whatever was picked before.
      } finally {
        portPromptOpen = false;
      }
      syncPortState();
      // Auto-detect right after a fresh grant — the ESP Web Tools / ESPHome
      // model where picking the device detects it immediately, so the board
      // list narrows without a second click. Only on a genuine new grant (not
      // a dropdown re-select), and non-fatal: runDetect routes any failure to
      // the status line. Re-detect = pick another port (same path).
      if (granted) {
        const status = document.getElementById("detect-status");
        // Rebuild the dropdown once detect lands so the option upgrades from
        // "Port selected" to "Port selected — ESP32-S3" (getDetectedChip is set
        // by then). runDetect resolves after the chip read completes.
        installPicker.runDetect((text) => { status.textContent = text; })
          .finally(() => rebuildPortSelect());
      }
    }

    function applyPortChoice(value) {
      if (value === IDLE) return;   // the placeholder is not a choice
      if (value === DESKTOP) { desktopMode = true; syncPortState(); return; }
      // Any other choice is about a board again. openPortPicker ends in its own
      // syncPortState, so only sync here when nothing else will.
      desktopMode = false;
      if (value === PICK_NEW) openPortPicker();
      else syncPortState();
    }

    portSelect.addEventListener("change", () => applyPortChoice(portSelect.value));
    // `change` is the only listener here on purpose. A <select> fires `click` when the list
    // OPENS, so using that to catch a re-pick would launch the OS prompt on top of the list
    // the user just opened and hide every other option behind it. The IDLE placeholder is
    // what makes a re-pick work instead: after a cancelled prompt the row returns to it, so
    // choosing the same option again is a real value change.

    // --- Serial monitor -------------------------------------------------
    // Live read-only viewer of the picked port at 115200 baud. Web Serial
    // owns the port exclusively, so the monitor mutex'es with the
    // install/erase flows in both directions (see openModal /
    // closeModal for the button-disable side, and the closeMonitor
    // calls in onInstall / the Erase handler for the port-release side).
    const _monitor = { port: null, reader: null, closing: false };
    let _flashingFirmware = null;   // the firmware key onInstall is flashing, for handleError's chip fallback
    const monitorBackdrop = document.getElementById("monitor-backdrop");
    const monitorOutput = document.getElementById("monitor-output");
    const monitorStatus = document.getElementById("monitor-status");
    const monitorBtn = document.getElementById("monitor-btn");

    function setMonitorStatus(text) { monitorStatus.textContent = text; }

    function appendMonitor(text) {
      // Autoscroll only when the user is at the bottom — otherwise they're
      // scrolled up reading something and we shouldn't yank them down.
      const atBottom = monitorOutput.scrollTop + monitorOutput.clientHeight
                       >= monitorOutput.scrollHeight - 4;
      monitorOutput.textContent += text;
      if (atBottom) monitorOutput.scrollTop = monitorOutput.scrollHeight;
    }

    async function openMonitor() {
      if (_monitor.port) return;  // already open (Monitor clicked twice fast)
      // Browser-support gate: Safari / Firefox have no Web Serial API.
      // The page-load banner (#browser-warning) already tells the user,
      // but the Monitor button is still rendered and clickable; without
      // this guard we'd hit `navigator.serial.requestPort()` and the
      // resulting TypeError would be swallowed by openPortPicker's
      // catch, leaving the user with a non-responsive button. Show the
      // existing banner explicitly so a click here surfaces the cause.
      if (!("serial" in navigator)) {
        document.getElementById("browser-warning").style.display = "block";
        document.getElementById("browser-warning").scrollIntoView({behavior: "smooth"});
        return;
      }
      if (!pickedPort) {
        // Trigger the port picker; openMonitor was called from a user
        // gesture (Monitor button click), so requestPort() is allowed.
        await openPortPicker();
        if (!pickedPort) return;  // user cancelled
      }
      monitorOutput.textContent = "";
      setMonitorStatus("Opening port at 115200…");
      monitorBackdrop.classList.add("open");
      try {
        await pickedPort.open({ baudRate: 115200 });
      } catch (e) {
        // Already-open errors get caught here — the orchestrator opens
        // the port at flash time; if a prior install left it open we just
        // proceed (the reader still works on an open port).
        if (!String(e.message || "").includes("already open")) {
          setMonitorStatus(`Failed to open: ${e.message || e}`);
          return;
        }
      }
      _monitor.port = pickedPort;
      setMonitorStatus("Reading (115200 8N1)…");
      readLoop().catch(e => {
        // Read errors land here once — surface and shut down. Don't loop
        // forever on a dead port.
        if (!_monitor.closing) setMonitorStatus(`Read error: ${e.message || e}`);
      });
    }

    async function readLoop() {
      const decoder = new TextDecoder("utf-8", { fatal: false });
      _monitor.reader = _monitor.port.readable.getReader();
      try {
        while (true) {
          const { value, done } = await _monitor.reader.read();
          if (done) break;
          if (value && value.length) appendMonitor(decoder.decode(value, { stream: true }));
        }
      } finally {
        try { _monitor.reader.releaseLock(); } catch (_) { /* already released */ }
        _monitor.reader = null;
      }
    }

    async function closeMonitor() {
      if (_monitor.closing) return;
      _monitor.closing = true;
      monitorBackdrop.classList.remove("open");
      try {
        if (_monitor.reader) {
          // cancel() makes the pending read() return {done:true}; the read
          // loop then exits and releases the lock in its finally.
          await _monitor.reader.cancel();
        }
      } catch (_) { /* ignore */ }
      try {
        if (_monitor.port) await _monitor.port.close();
      } catch (_) { /* ignore */ }
      _monitor.port = null;
      _monitor.closing = false;
    }

    // Pulse RTS low → high to trigger the device's auto-reset circuit
    // (DTR/RTS combo mirrors what esptool does pre-flash; for a bare
    // reset, toggling RTS alone is enough on most USB-Serial bridges).
    // setSignals() requires the port to be open — we are, since the
    // monitor is running. After the pulse the device reboots and its
    // boot log starts streaming through the read loop already in place.
    async function monitorReset() {
      if (!_monitor.port) return;
      try {
        await _monitor.port.setSignals({ dataTerminalReady: false, requestToSend: true });
        await new Promise(r => setTimeout(r, 100));
        await _monitor.port.setSignals({ dataTerminalReady: false, requestToSend: false });
        appendMonitor("\n--- reset ---\n");
      } catch (e) {
        setMonitorStatus(`Reset failed: ${e.message || e}`);
      }
    }

    monitorBtn.addEventListener("click", openMonitor);
    document.getElementById("monitor-close").addEventListener("click", closeMonitor);
    document.getElementById("monitor-clear").addEventListener("click", () => {
      monitorOutput.textContent = "";
    });
    document.getElementById("monitor-reset").addEventListener("click", monitorReset);
    // Esc closes the monitor (same affordance the install modal has via
    // closeModal — keeps the two dialogs symmetric).
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && monitorBackdrop.classList.contains("open")) {
        closeMonitor();
      }
    });

    // --- Picker wiring -------------------------------------------------
    // Preview-only: preview_installer.py writes local-firmwares.json next to this
    // page when it stages local ESP32 builds. It lets the picker offer a firmware
    // that isn't in the published release's assets yet (a brand-new variant being
    // tested before it ships). Absent in production (the deploy doesn't write it),
    // so the fetch 404s and we pass null — the published assets drive the list.
    const extraFirmwaresByTag = await fetch("./local-firmwares.json")
      .then(r => r.ok ? r.json() : null).catch(() => null);

    const _pickerReady = installPicker.init({
      container: document.getElementById("picker-mount"),
      ownFirmwareKey: null,  // web installer flashes any firmware variant
      installRowExtras: document.getElementById("erase-row"),
      boardSupport,  // board catalog + chip detection (mooninstaller-only module)
      extraFirmwaresByTag,
      // Gate Install on a picked USB port — the web installer requires the user
      // to choose the port in the dropdown before flashing. (notifyPortChanged()
      // below re-evaluates the button whenever pickedPort changes.)
      // Desktop mode has nothing to open, so the port gate does not apply: the button is a
      // download and is enabled as soon as an archive for this OS exists.
      hasPort: () => desktopMode || !!pickedPort,
      onDetect: async () => {
        // The monitor (if open) holds the port — release it before esptool
        // claims it, same as the install path. detect() keeps the port open
        // afterwards so the following Install reuses it.
        if (_monitor.port) await closeMonitor();
        return await installer.detect({ port: pickedPort, onLog: appendLog });
      },
      onInstall: async (firmware, manifestUrl, binaryUrl, entry) => {
        // A desktop archive is handed to the browser to download; there is no chip to flash
        // and no port to hold. Anchor-click rather than location.href so the Content-Disposition
        // from GitHub is honoured and the page is not navigated away mid-session.
        if (entry && entry.isDesktop) {
          const a = document.createElement("a");
          a.href = binaryUrl;
          a.download = "";
          document.body.appendChild(a);
          a.click();
          a.remove();
          return;
        }
        // Remember the firmware being flashed so handleError can fall back to it
        // for the unsupported-chip guidance when no board was picked (the on-device
        // OTA path, or a generic flash) — getSelectedBoardChip() is empty there.
        _flashingFirmware = firmware;
        // If the monitor is open it holds the port — release it before
        // esptool tries to claim it. The reverse lock (monitor button
        // disabled for the install's duration) lives in openModal /
        // closeModal so the user can't reopen the monitor mid-flash.
        if (_monitor.port) await closeMonitor();
        const localUrl = toLocalUrl(manifestUrl);
        const board = installPicker.getSelectedBoard();
        const txPower = installPicker.getSelectedBoardTxPower();
        openModal(board ? `Installing MoonLight on ${board}` : `Installing ${firmware}`);
        showSection("connecting");
        document.getElementById("connecting-detail").textContent = "";
        const eraseBefore = document.getElementById("erase-before-flash").checked;
        // Apply the device-model's catalog defaults (modules + controls) after flashing.
        // Default-ticked-with-erase (see the change listener below): a clean slate wants
        // defaults; re-flashing a configured device should NOT silently re-inject (the
        // catalog's replaceChildren would delete the user's effects). Untick to flash the
        // firmware while keeping the device's current config. txPower (the brown-out cap)
        // is a hardware trait, not a "default", so it still applies regardless — only the
        // module/control inject is gated.
        const applyDefaults = document.getElementById("apply-device-defaults").checked;
        // Ethernet-only firmware: WiFi compiled out (firmwares.json `eth_only`). Keyed off the
        // name like isCompatible's `-eth*` rule — a `-eth` variant is eth-only UNLESS it's the
        // `-eth-wifi` co-processor build. The orchestrator uses this to skip WiFi provisioning
        // (the build has no WIFI_SETTINGS RPC) and tell the user to connect Ethernet instead.
        const ethOnly = /-eth$/.test(firmware);
        await installer.start({
          // pickedPort may be null — orchestrator falls back to requestPort.
          port: pickedPort,
          manifestUrl: localUrl,
          board,         // names the install title + identifies the catalog entry
          applyDefaults, // gates the APPLY_OP config push (not txPower, sent earlier)
          txPower,
          eraseBefore,
          ethOnly,
          onProgress: handleProgress,
          uiWaitForCreds,
          uiWaitForIp,
          uiShowNeedsIpRetrying: showNeedsIpRetrying,
          uiWaitForPortRetry,
          onSuccess: handleSuccess,
          onError: handleError,
          onLog: appendLog,
        });
      },
    });

    // Auto-tie "Apply device defaults" to "Erase chip first": ticking erase (a clean
    // slate) ticks defaults; unticking erase unticks it. The user can still override
    // either box independently afterwards — the tie only fires on an erase toggle. Both
    // start unticked (erase's default), so a plain re-flash keeps the device's config.
    (() => {
      const eraseBox = document.getElementById("erase-before-flash");
      const defaultsBox = document.getElementById("apply-device-defaults");
      if (eraseBox && defaultsBox) {
        eraseBox.addEventListener("change", () => { defaultsBox.checked = eraseBox.checked; });
      }
    })();

    myDevices.init({
      container: document.getElementById("devices-mount"),
      onErase: async (device) => {
        if (!confirm(
            `Erase ${device.name}? This wipes WiFi credentials and all ` +
            `module state. Back up its config first, see "Back up a ` +
            `device's config first" on this page. You can flash a fresh ` +
            `firmware afterwards.`)) {
          return;
        }
        // Same port-lock concern as install: release the monitor before
        // esptool tries to claim the port.
        if (_monitor.port) await closeMonitor();
        openModal(`Erasing ${device.name}`);
        showSection("connecting");
        document.getElementById("connecting-detail").textContent = "";
        await installer.eraseOnly({
          port: pickedPort,
          onProgress: handleProgress,
          uiWaitForPortRetry,
          onSuccess: () => {
            disarmUnloadGuard();   // erase finished — drop the tab-close warning
            showSection("done");
            // Reset any header left by a prior install in this session — erase is
            // not "Device is online!".
            document.getElementById("done-status").textContent = "Erase complete";
            const a = document.getElementById("done-url");
            a.removeAttribute("href");
            a.textContent = "Erase complete — flash a fresh firmware to use the device again.";
            document.getElementById("done-url-mdns").hidden = true;  // no device URL after erase
            document.getElementById("done-defaults").hidden = true;  // nothing applied on erase
          },
          onError: handleError,
          onLog: appendLog,
        });
      },
    });

    // Show the unsupported-browser banner when Web Serial isn't available.
    if (!("serial" in navigator)) {
      document.getElementById("browser-warning").style.display = "block";
      // Flashing needs Web Serial; downloading the desktop build does not. On a browser
      // without it, open on the half that works instead of on a port row that cannot.
      // After _pickerReady: setDesktopMode reaches the picker's state only once init()
      // has mounted it, so calling it here synchronously would silently do nothing.
      if (thisComputerLabel()) { desktopMode = true; syncPortState(); }
    }

    // --- picture board grid --------------------------------
    // Renders deviceModels.json as a visual card grid; on select it drives the shared
    // picker's own (hidden) #rp-board <select> via a dispatched change event, so
    // the existing release/firmware narrowing + flash flow runs unchanged. The
    // shared install-picker.js is NOT modified.
    (async function boardGrid() {
      let boards = [];
      try {
        const res = await fetch("./deviceModels.json");   // same catalog as the picker
        boards = await res.json();
      } catch (e) {
        document.getElementById("board-grid").textContent = "Could not load deviceModels.json: " + e;
        return;
      }
      await _pickerReady;   // ensure the picker has mounted its (hidden) #rp-board

      const gridEl    = document.getElementById("board-grid");
      const searchEl  = document.getElementById("board-search");
      const summaryEl = document.getElementById("board-summary");
      const labelEl   = document.getElementById("board-summary-label");
      const thumbEl   = document.getElementById("board-summary-thumb");
      const expandEl  = document.getElementById("board-expand");
      const clearEl   = document.getElementById("board-clear");
      let selected = installPicker.getSelectedBoard() || "";   // honour a restored pick

      function ledDriver(b) {
        const d = (b.modules || []).find(m => /LedDriver$/.test(m.type || ""));
        return d ? d.type.replace(/Driver$/, "") : null;
      }
      // A supported capability is "active" (configured) when deviceModels.json has a module
      // backing it. The capability→module link is implicit in the data, so this map
      // names it in one place (bespoke, but it just reads the modules[] already there —
      // no duplicated `active` field to drift). A capability with no entry here, or no
      // matching module, stays merely "supported". Each predicate gets the whole module
      // object so it can inspect controls — needed to tell Ethernet from WiFi: both ride
      // NetworkModule, but Ethernet is only ACTUALLY wired when the board's NetworkModule
      // carries an ethType control set to a real PHY (not absent / "None"/0). WiFi is
      // active wherever NetworkModule exists (the radio is always available); a board
      // that lists WiFi as supported but ships no NetworkModule entry stays "supported".
      const ethConfigured = (m) => {
        const c = m.controls;
        if (!c) return false;
        // A named board preset IS the configuration: it carries the PHY and the pins, so a
        // board that picks one lists no ethType of its own. Custom is the escape hatch and
        // names no PHY, so it falls through to the ethType its entry must then carry.
        if (c.ethBoard !== undefined && c.ethBoard !== "Custom") return true;
        const t = c.ethType;
        return t !== undefined && t !== 0 && t !== "0" && t !== "None";
      };
      const CAP_MODULE = {
        LEDs:     m => /LedDriver$/.test(m.type || ""),
        Ethernet: m => m.type === "NetworkModule" && ethConfigured(m),
        WiFi:     m => m.type === "NetworkModule",
        Audio:    m => /^Audio/.test(m.type || ""),
      };
      function capActive(b, cap) {
        const test = CAP_MODULE[cap];
        return !!test && (b.modules || []).some(m => test(m));
      }
      function setExpanded(open) {
        expandEl.hidden = !open;
        summaryEl.setAttribute("aria-expanded", open ? "true" : "false");
        if (open) { searchEl.focus(); }
      }
      // Reflect the current pick in the collapsed summary (label + thumbnail).
      function updateSummary() {
        const b = boards.find(x => x.name === selected);
        if (b) {
          labelEl.textContent = b.name;
          if (b.image) { thumbEl.hidden = false; thumbEl.style.backgroundImage = `url("${b.image}")`; }
          else { thumbEl.hidden = true; }
        } else {
          labelEl.textContent = "Pick a device";
          thumbEl.hidden = true;
        }
      }
      function pickBoard(name) {
        selected = name;
        // Drive the shared picker's hidden <select>: set value + fire change so
        // its listener updates state.selectedBoard and re-filters firmware.
        const rpBoard = document.getElementById("rp-board");
        if (rpBoard) {
          // A Detect narrows #rp-board's options to one family. With "show all
          // boards" the grid can pick a board from ANOTHER family — whose option
          // isn't in the narrowed list, so `value = name` would silently no-op
          // (value stays "") and the firmware list wouldn't narrow. Ensure the
          // option exists first so the assignment takes and selectedBoard is set.
          if (name && !Array.from(rpBoard.options).some(o => o.value === name)) {
            const o = document.createElement("option");
            o.value = name; o.textContent = name;
            rpBoard.appendChild(o);
          }
          rpBoard.value = name;
          rpBoard.dispatchEvent(new Event("change", { bubbles: true }));
        }
        updateSummary();
        setExpanded(false);   // collapse back to the summary after a pick
        render();             // keep the grid's selected-card state in sync for next open
      }
      // After a Detect, the shared picker narrows its hidden #rp-board <select>
      // to the matching-family boards (applyDetectedChip → fillBoardOptions). The
      // grid mirrors that: it shows only boards whose name is a current #rp-board
      // option. Before any detect, #rp-board holds the FULL catalog (plus the
      // "(any board)" / "Other…" pass-through, which has no value), so the grid
      // shows everything — only a detect narrows it. Returns null = no constraint.
      // `showAll` is the user's escape hatch (the "show all boards" toggle) for a
      // wrong/unhelpful detection — when set, the filter is bypassed.
      let showAll = false;
      function narrowedNames() {
        const sel = document.getElementById("rp-board");
        if (!sel) return null;
        const names = Array.from(sel.options).map(o => o.value).filter(Boolean);
        // If the option set equals the full catalog, there's no narrowing.
        return names.length && names.length < boards.length ? new Set(names) : null;
      }
      function allowedNames() {
        return showAll ? null : narrowedNames();
      }
      // The detected family label (from the narrowed boards' shared chip), for the
      // "Detected <family> · show all" notice. null when not narrowed.
      function detectedFamily() {
        const allow = narrowedNames();
        if (!allow) return null;
        const fams = new Set(boards.filter(b => allow.has(b.name)).map(b => b.chip));
        return fams.size === 1 ? [...fams][0] : null;
      }
      function renderFilterNotice() {
        const notice = document.getElementById("board-filter-notice");
        const fam = detectedFamily();
        if (!fam) { notice.hidden = true; notice.replaceChildren(); return; }
        notice.hidden = false;
        notice.replaceChildren();
        if (showAll) {
          notice.append(`Showing all boards. `);
          const a = document.createElement("button");
          a.textContent = `Filter to detected ${fam}`;
          a.onclick = () => { showAll = false; render(); };
          notice.append(a);
        } else {
          notice.append(`Detected ${fam}. `);
          const a = document.createElement("button");
          a.textContent = "Show all boards";
          a.title = "Detection wrong, or your board isn't in this family? Show the full catalog.";
          a.onclick = () => { showAll = true; render(); };
          notice.append(a);
        }
      }
      function render() {
        const q = (searchEl.value || "").toLowerCase();
        const allow = allowedNames();
        const shown = boards.filter(b =>
          (!q || b.name.toLowerCase().includes(q)) &&
          (!allow || allow.has(b.name)));
        renderFilterNotice();
        gridEl.replaceChildren();
        const byChip = {};
        for (const b of shown) (byChip[b.chip] ||= []).push(b);
        for (const chip of Object.keys(byChip).sort()) {
          const lbl = document.createElement("div");
          lbl.className = "bg-chip-label"; lbl.textContent = chip;
          gridEl.appendChild(lbl);
          for (const b of byChip[chip]) gridEl.appendChild(card(b));
        }
        if (!shown.length) {
          const e = document.createElement("p"); e.className = "note"; e.textContent = "No boards match.";
          gridEl.appendChild(e);
        }
      }
      function card(b) {
        const el = document.createElement("div");
        el.className = "bg-card" + (selected === b.name ? " selected" : "");
        // Keyboard-accessible: a focusable option that picks on Enter/Space, the
        // same affordance the click handler gives the mouse.
        el.tabIndex = 0;
        el.setAttribute("role", "option");
        el.setAttribute("aria-selected", selected === b.name ? "true" : "false");
        el.onclick = (ev) => { if (!ev.target.classList.contains("bg-link")) pickBoard(b.name); };
        el.onkeydown = (ev) => {
          if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); pickBoard(b.name); }
        };
        const thumb = document.createElement("div");
        thumb.className = "bg-thumb" + (b.image ? "" : " noimg");
        // the deploy stages a copy of deviceModels.json
        // + the referenced board images alongside this page, so an "image" path of
        // "assets/deviceModels/<slug>.jpg" resolves same-origin from this page.
        if (b.image) thumb.style.backgroundImage = `url("${b.image}")`;
        el.appendChild(thumb);
        const body = document.createElement("div"); body.className = "bg-body";
        const nm = document.createElement("div"); nm.className = "bg-name"; nm.textContent = b.name; body.appendChild(nm);
        const meta = document.createElement("div"); meta.className = "bg-meta";
        meta.textContent = b.chip + (ledDriver(b) ? " · " + ledDriver(b) : "");
        body.appendChild(meta);
        // Capability chips, three states by color (not text): green = active
        // (supported AND a module configured in deviceModels.json), yellow = supported
        // (firmware supports it, not pre-configured), orange = planned (no module
        // yet — the backlog seed). All chips are shown (labels kept short in
        // deviceModels.json so they fit the card).
        const caps = [
          ...(Array.isArray(b.supported) ? b.supported.map(c =>
            capActive(b, c) ? { c, cls: "act", label: "active" }
                            : { c, cls: "sup", label: "supported" }) : []),
          ...(Array.isArray(b.planned) ? b.planned.map(c => ({ c, cls: "plan", label: "planned" })) : []),
        ];
        if (caps.length) {
          const capsEl = document.createElement("div"); capsEl.className = "bg-caps";
          for (const { c, cls, label } of caps) {
            const chip = document.createElement("span");
            chip.className = "bg-cap " + cls;
            chip.textContent = c;   // color conveys active / supported / planned
            chip.title = c + " — " + label;
            capsEl.appendChild(chip);
          }
          body.appendChild(capsEl);
        }
        if (b.url) {
          const a = document.createElement("a"); a.className = "bg-link"; a.href = b.url;
          a.target = "_blank"; a.rel = "noopener"; a.textContent = "product page ↗"; body.appendChild(a);
        }
        // "details" opens the full deviceModels.json entry in a popup. It carries the
        // bg-link class so the card's onclick treats it as a non-select region
        // (same guard the product-page link relies on).
        const det = document.createElement("a");
        det.className = "bg-link bg-details"; det.textContent = "details ⓘ";
        det.setAttribute("role", "button"); det.tabIndex = 0;
        det.onclick = (ev) => { ev.stopPropagation(); showBoardDetails(b); };
        det.onkeydown = (ev) => {
          if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); ev.stopPropagation(); showBoardDetails(b); }
        };
        body.appendChild(det);
        el.appendChild(body);
        return el;
      }
      // Fill + open the board-details popup from a deviceModels.json entry: a readable
      // summary (chip, firmwares, capabilities, modules + their controls) plus a
      // collapsible raw-JSON block for the exact entry. Built with DOM nodes (not
      // innerHTML) so board-supplied strings can't inject markup.
      function showBoardDetails(b) {
        const dlg = document.getElementById("board-details");
        document.getElementById("bd-title").textContent = b.name || "Device";
        const body = document.getElementById("bd-body");
        body.replaceChildren();
        const row = (key, val) => {
          const r = document.createElement("div"); r.className = "bd-row";
          const k = document.createElement("span"); k.className = "bd-key"; k.textContent = key;
          const v = document.createElement("span"); v.className = "bd-val"; v.textContent = val;
          r.append(k, v); body.appendChild(r);
        };
        // Same shape as row(), but the value is a clickable link. href + textContent
        // only (no innerHTML), so a board-supplied URL can't inject markup. Opens in
        // a new tab; only http(s) links are made clickable (else fall back to text).
        const rowLink = (key, url) => {
          const r = document.createElement("div"); r.className = "bd-row";
          const k = document.createElement("span"); k.className = "bd-key"; k.textContent = key;
          const v = document.createElement("span"); v.className = "bd-val";
          const a = document.createElement("a");
          a.href = url; a.target = "_blank"; a.rel = "noopener"; a.textContent = url;
          v.appendChild(a); r.append(k, v); body.appendChild(r);
        };
        if (b.chip) row("Chip", b.chip);
        if (Array.isArray(b.firmwares)) row("Firmwares", b.firmwares.join(", "));
        if (Array.isArray(b.supported) && b.supported.length) row("Supported", b.supported.join(", "));
        if (Array.isArray(b.planned) && b.planned.length) row("Planned", b.planned.join(", "));
        if (b.url) {
          if (/^https?:\/\//.test(b.url)) rowLink("Product page", b.url);
          else row("Product page", b.url);
        }

        if (Array.isArray(b.modules) && b.modules.length) {
          const h = document.createElement("div"); h.className = "bd-section"; h.textContent = "Modules";
          body.appendChild(h);
          for (const m of b.modules) {
            const mod = document.createElement("div"); mod.className = "bd-mod";
            const nm = document.createElement("div"); nm.className = "bd-mod-name";
            nm.textContent = m.type || "?";
            if (m.id && m.id !== m.type) {
              const idEl = document.createElement("span"); idEl.className = "bd-mod-id";
              idEl.textContent = "  (" + m.id + ")"; nm.appendChild(idEl);
            }
            mod.appendChild(nm);
            const ctrls = m.controls && typeof m.controls === "object" ? m.controls : null;
            if (ctrls) {
              for (const [k, v] of Object.entries(ctrls)) {
                const c = document.createElement("div"); c.className = "bd-ctrl";
                const code = document.createElement("code"); code.textContent = k + " = " + v;
                c.appendChild(code); mod.appendChild(c);
              }
            }
            body.appendChild(mod);
          }
        }

        // Collapsible raw JSON — the exact entry, for developers.
        const raw = document.createElement("details"); raw.className = "bd-raw";
        const sum = document.createElement("summary"); sum.textContent = "Raw JSON";
        const pre = document.createElement("pre"); pre.textContent = JSON.stringify(b, null, 2);
        raw.append(sum, pre); body.appendChild(raw);

        dlg.showModal();
      }
      searchEl.oninput = render;
      summaryEl.addEventListener("click", () => setExpanded(expandEl.hidden));
      clearEl.addEventListener("click", () => pickBoard(""));   // generic / no board

      // Re-render when a Detect narrows the picker: applyDetectedChip swaps out
      // #rp-board's <option>s, so observe its child list. The grid then filters
      // to the detected family (allowedNames), and updateSummary picks up any
      // auto-selected single match.
      // Observe the picker's CONTAINER, not #rp-board itself: the picker re-renders by
      // replacing its whole innerHTML (switching install target does this), so a #rp-board
      // captured once is detached by the first switch and its observer never fires again,
      // silently killing Detect narrowing. The container node is stable for the page's life,
      // and `subtree` catches the option swaps inside whichever #rp-board currently exists.
      const pickerMount = document.getElementById("picker-mount");
      if (pickerMount) {
        new MutationObserver(() => {
          showAll = false;   // a fresh detect is a new context, so re-apply the filter
          // Take the picker's value verbatim: an empty string means it cleared the
          // selection (detected family with multiple matches, so generic mode), and
          // `|| selected` would wrongly keep the stale board in the summary.
          selected = installPicker.getSelectedBoard();
          updateSummary();
          render();
        }).observe(pickerMount, { childList: true, subtree: true });
      }

      updateSummary();   // reflect any restored pick in the collapsed summary
      render();
    })();
