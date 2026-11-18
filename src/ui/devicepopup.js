// A device map node's popup: the device's detail, and what can be done with it from here. A MoonLight device shows its summary,
// identify, its update and its device defaults; a WLED, Tasmota or ESPHome device moves to MoonLight over the network, and a board
// MoonBase holds takes MoonLight's installer. The device's page opens it from its map, and MoonDeck serves the same file for its Live
// tab's map. What differs between the two comes in as hooks on the map's `opts.hooks`: how the detail is drawn, where the device
// serving the list answers, and a section of the page's own, MoonDeck's USB and live-test actions.

import { installPicker, loadReleases, releaseAsset, firmwareAssetName, moonbaseAssetName, migrateAssetName, chipKey } from "./install-picker.js";
import { graphOutputs } from "./devicemap.js";
import { fmtBytes, fmtTime, fwSleep, pollUntil, errorMessage, armPressTwice, fetchWithin } from "./uikit.js";
import { UPDATE_TTL_MS, channelUpdate, releaseOfVersion, moonbaseAssetKeyFrom, INSTALLER_URL } from "./releases.js";
import { installFirmware, peerFirmware, waitForRestart } from "./deviceinstall.js";

// The hooks a page passes, each with a default: the detail as plain rows and the page's own origin as home, so a page passing only the
// map's hooks still shows the whole popup.
const POPUP_HOOKS = {
    detail: plainDetail,             // (panel, fields, opts): draws a node's fields into the popup's detail panel
    home: () => location.origin,     // (): where the device serving the list answers, its own node's address and the ESPHome relay's
    actions: () => {},               // (row, dlg, opts): adds the page's own section to a node's popup, once, as the popup is built
};
const hooks = (opts) => ({ ...POPUP_HOOKS, ...(opts && opts.hooks) });

// A node's scalar fields as labeled rows, each value a `.display` keyed by the name it shows, as the list's renderer marks it, so linkIp
// finds the ip; `ageSec` reads as "last seen".
function plainDetail(panel, fields) {
    panel.replaceChildren();
    for (const [key, value] of Object.entries(fields || {})) {
        const scalarArray = Array.isArray(value) && value.every(e => typeof e !== "object");
        if (value === null || (typeof value === "object" && !scalarArray)) continue;
        const lastSeen = key === "ageSec";
        const name = lastSeen ? "last seen" : key;
        const line = document.createElement("div");
        line.className = "graph-popup-row";
        const label = document.createElement("span");
        label.className = "graph-popup-label";
        label.textContent = name;
        const shown = document.createElement("span");
        shown.className = "display";
        shown.dataset.key = name;
        shown.textContent = lastSeen ? `${fmtTime(value)} ago` : scalarArray ? value.join(", ") : String(value);
        line.append(label, shown);
        panel.appendChild(line);
    }
}

// A node's row in a popup: its detail, drawn by the page's `detail` hook, and for a MoonLight device the firmware it runs, the release
// it could run, and a button that installs it. `opts` is the map's, its hooks among them.
export function openDevicePopup(row, opts) {
    const dlg = document.createElement("dialog");
    dlg.className = "graph-popup";
    const head = document.createElement("form");
    head.method = "dialog";
    head.className = "graph-popup-head";
    const title = document.createElement("span");
    title.textContent = row.name ?? row.ip ?? "";
    const close = document.createElement("button");
    close.value = "close";
    close.title = "close";
    close.textContent = "✕";
    head.append(title, close);
    const detail = document.createElement("div");
    detail.className = "list-detail";
    const home = hooks(opts).home();
    hooks(opts).detail(detail, popupFields(row), opts);
    linkIp(detail, row, home);
    dlg.append(head, detail);
    if (row.type === "MoonLight") dlg.appendChild(buildPeerSummary(row, home));
    if (row.type === "MoonBase") dlg.appendChild(buildMoonBaseInstall(row, home));
    if (row.type === "WLED") dlg.appendChild(buildWledMigrate(row));
    if (row.type === "Tasmota") dlg.appendChild(buildTasmotaMigrate(row));
    if (row.type === "ESPHome") dlg.appendChild(buildEsphomeMigrate(row, home));
    if (row.mac) dlg.appendChild(buildGroupMembership(row, home));
    hooks(opts).actions(row, dlg, opts);
    document.body.appendChild(dlg);
    // A click on the backdrop closes it, except while a move runs in it: its steps run in the browser, and closing would hide them.
    const mayClose = () => !dlg.dataset.busy || confirm("A move is running. Close this popup? It goes on, out of sight, until the app reloads.");
    dlg.addEventListener("click", (e) => { if (e.target === dlg && !dlg.dataset.busy) dlg.close(); });
    dlg.addEventListener("cancel", (e) => { if (!mayClose()) e.preventDefault(); });   // Escape
    close.addEventListener("click", (e) => { if (!mayClose()) e.preventDefault(); });
    dlg.addEventListener("close", () => dlg.remove(), { once: true });
    dlg.showModal();
}

// The fields a node's popup lists: its flavor as its type (WLED-MM), and as its url the `<name>.local` address a MoonLight board answers on;
// the ip links to the address the device was found on, so a url saying the same goes. A host serves on a port and answers no mDNS.
function popupFields(row) {
    const shown = Object.assign({}, row);
    if (shown.flavor) { shown.type = shown.flavor; delete shown.flavor; }
    delete shown.url;
    if (row.type === "MoonLight" && !row.port && /^[a-z0-9-]+$/i.test(row.name || "")) shown.url = `http://${row.name}.local/`;
    return shown;
}

// The popup's ip as a link to the device's page.
function linkIp(detail, row, home) {
    const span = detail.querySelector('.display[data-key="ip"]');
    if (!span || !row.ip) return;
    const a = document.createElement("a");
    a.className = "display control-url";
    a.target = "_blank";
    a.rel = "noopener";
    a.href = `${peerBase(row, home)}/`;
    a.textContent = row.ip;
    span.replaceWith(a);
}

// A device's JSON, or null when it does not answer within `ms`: a hung device leaves a popup saying so rather than reading for ever.
async function fetchJsonWithin(url, ms = 4000) { return fetchWithin(url, ms, (r) => r.json()); }

// Where a device's API answers: `home` for the device serving the list, its address for another.
export function peerBase(row, home) {
    if (row.self) return home;
    return row.url ? row.url.replace(/\/$/, "") : `http://${row.ip}${row.port ? `:${row.port}` : ""}`;
}

// The image of release `rel` that installs `version` on a device, its address and the SHA-256 GitHub names for it, firmware-<name>-v<version>.bin for
// a board and a host alike. Null url for a device its host or its user updates, and for one older than its firmware name.
function updateAsset(rel, dev, version) {
    const installs = !!dev.firmware && (!dev.update || dev.update === "self");
    return installs ? releaseAsset(rel, firmwareAssetName(dev.firmware, version)) : { url: null, sha256: null };
}

// The release a device could move to from `releases` and the asset that installs it, by the same checks the update badge makes for this device.
export function peerUpdate(dev, releases) {
    const u = channelUpdate(dev, releases);
    return u ? { label: u.label, version: u.version, ...updateAsset(u.rel, dev, u.version) } : null;
}

// What a device reports, in the shape the update rules read: version, firmware (null while unnamed), how it is updated and its mode.
export function deviceOf(fw) {
    return { version: fw.version, firmware: fw.firmware && fw.firmware !== "unknown" ? fw.firmware : null, update: fw.summary?.update, mode: fw.summary?.mode };
}

// Why an update `u` with no image is not installed from here: a container's host, a firmware too old to name its image, or the user's own hand.
export function noImageNote(u, fw) {
    if (fw.summary?.update === "container") return `${u.label} is available: the container's host updates it, by Watchtower or Podman's auto-update.`;
    if (!fw.firmware || fw.firmware === "unknown") return `${u.label} is available: this device's firmware is too old to name its image, so install it from the device's own page.`;
    return `${u.label} is available: this device is updated by hand, from the release page.`;
}

// The Ethernet boards of WLED's table the migration carries over, by WLED's `eth.type`, as kWledEthernet in WledNetworks.h maps them.
const WLED_ETHERNET_MAPPED = [1, 2, 3, 4, 5, 6, 8, 9, 10];

// What the move does with a WLED's Ethernet: an empty note for a WLED on WiFi, its wiring carried over for a board the migration maps, else a warning
// that it comes back on its saved WiFi or as an access point.
function wledEthernetNote(type) {
    if (!type) return "";
    return WLED_ETHERNET_MAPPED.includes(type)
        ? " On Ethernet: MoonLight takes over its wiring and comes back on the cable."
        : ` On Ethernet board ${type}, which the move does not carry over: it comes back on its saved WiFi, else as an access point to join.`;
}

// The chip a WLED's /json/info names, as MoonLight's files spell it: "esp32-c3" is esp32c3; null for one MoonLight does not build for.
function wledChip(info) {
    const key = chipKey(info?.arch);
    return /^esp32[a-z0-9]*$/.test(key) ? key : null;
}

// The migration image a WLED takes, and the apps the picker offers it: its chip's, and on a P4 from revision 3 that revision line's, which WLED-MM's
// chip model names ("ESP32-P4 rev.301").
function wledFamily(info, chip) {
    const rev = Number(/rev\.(\d+)/.exec(String(info?.e32model || ""))?.[1] || 0);
    return chip === "esp32p4" && rev >= 300 ? "esp32p4rev3" : chip;
}

// The way back from MoonLight, which a move's confirm names: the firmware's own installer over USB, linked from the web installer's page.
function goingBackLink(firmware) {
    const line = document.createElement("div");
    line.className = "graph-popup-note";
    const a = document.createElement("a");
    a.href = `${INSTALLER_URL}#other-firmware`;
    a.target = "_blank";
    a.rel = "noopener";
    a.textContent = `Going back to ${firmware}: its own installer, over USB`;
    line.appendChild(a);
    return line;
}

// Where the browser takes a release's migration image: the installer's mirror of the release.
function migrateImageUrl(tag, chip) {
    return `${INSTALLER_URL}releases/${tag}/${migrateAssetName(chip)}`;
}

// A WLED device's way to MoonLight over the network, in one go: the install picker for its chip, a confirm that going back needs a cable,
// then the migration image through WLED's update page, MoonLight from MoonBase, and the device's defaults.
function buildWledMigrate(row) {
    const box = document.createElement("div");
    box.className = "graph-popup-firmware";
    const line = document.createElement("div");
    line.textContent = "WLED: reading…";
    box.appendChild(line);
    const base = `http://${row.ip}`;
    (async () => {
        const [info, cfg] = await Promise.all([fetchJsonWithin(`${base}/json/info`), fetchJsonWithin(`${base}/json/cfg`)]);
        if (!info) { line.textContent = "WLED does not answer."; return; }
        const chip = wledChip(info);
        line.textContent = `WLED ${info.ver || ""} on ${info.arch || "an unknown chip"}.${wledEthernetNote(cfg?.eth?.type)}`;
        if (!chip) { line.textContent += " MoonLight does not run on it."; return; }
        // A migration image from this computer, a build or a download, for a release that carries none for this chip.
        const file = document.createElement("label");
        file.className = "graph-popup-file";
        const input = document.createElement("input");
        input.type = "file";
        input.accept = ".bin";
        file.append("Migration image, for a release without one ", input);
        box.appendChild(goingBackLink("WLED"));
        const family = wledFamily(info, chip);
        mountMapPicker(box, base, family, null, file, async (binaryUrl, tag, note) => {
            if (!confirm("Move this WLED device to MoonLight over the network? Going back to WLED needs a USB cable.")) return null;
            await wledToMoonBase(base, input.files[0] || migrateImageUrl(tag, family), tag, chip, note);
            return moonBaseToMoonLight(base, binaryUrl, note);
        });
    })();
    return box;
}

// A board MoonBase holds: its chip from MoonBase's /api/system, and the install picker for it.
function buildMoonBaseInstall(row, home) {
    const box = document.createElement("div");
    box.className = "graph-popup-firmware";
    const line = document.createElement("div");
    line.textContent = "MoonBase: reading…";
    box.appendChild(line);
    const base = peerBase(row, home);
    (async () => {
        const sys = await fetchJsonWithin(`${base}/api/system`);
        if (!sys || !sys.moonbase) { line.textContent = "MoonBase does not answer."; return; }
        line.textContent = `MoonBase ${sys.version} on an ${sys.chip}: MoonLight's installer holds this board.`;
        mountMapPicker(box, base, sys.chip, sys.firmware, null, (binaryUrl, _tag, note) => moonBaseToMoonLight(base, binaryUrl, note));
    })();
    return box;
}

// The web installer's Device row for a map's install: its own helpers and catalog from its Pages site, which serves both cross-origin, so
// none of it ships in the firmware. The module's loadDevices reads the catalog beside the page, the installer's, so this one reads it beside
// the module. Null offline, which leaves the picker Release and Firmware.
let mapDeviceSupport = null;
function loadMapDeviceSupport() {
    mapDeviceSupport ??= import(`${INSTALLER_URL}install-picker-devices.js`)
        .then(m => ({ ...m, loadDevices: async () => (await fetchJsonWithin(`${INSTALLER_URL}deviceModels.json`)) || [] }))
        .catch(() => null);
    return mapDeviceSupport;
}

// The shared install picker on a node's popup, as the web installer shows it: Release, Device and Firmware for `chip`, and Apply device defaults.
// `move(binaryUrl, tag, note)` puts MoonLight on the board and answers its /api/system once MoonLight runs, or null when the user stops;
// the picked device's defaults follow, and the status line says how it ended.
async function mountMapPicker(box, base, chip, ranFirmware, extra, move, blocker = null) {
    const note = document.createElement("div");
    note.className = "graph-popup-note";
    const defaults = document.createElement("label");
    defaults.className = "graph-popup-defaults";
    const tick = document.createElement("input");
    tick.type = "checkbox";
    tick.checked = true;
    defaults.append(tick, " Apply device defaults sets this device model's recommended modules/settings.");
    const extras = document.createElement("div");
    extras.className = "graph-popup-extras";
    if (extra) extras.appendChild(extra);
    extras.append(defaults, note);
    const mount = document.createElement("div");
    mount.className = "install-picker-host";
    box.appendChild(mount);
    const picker = await installPicker.init({
        container: mount, targetChip: chip, ownFirmwareKey: ranFirmware || null,
        deviceSupport: await loadMapDeviceSupport(), installRowExtras: extras, blocker,
        onInstall: async (_firmware, _manifestUrl, binaryUrl, _entry, label, tag) => {
            const dlg = box.closest("dialog");
            if (dlg) dlg.dataset.busy = "1";   // the move runs in the browser: openDevicePopup keeps the popup open meanwhile
            const device = picker?.selectedDeviceEntry();   // the device the firmware was picked for, whatever the selector shows by the end
            try {
                const sys = await move(binaryUrl, tag, note);
                if (!sys) return "Not started.";
                const applied = tick.checked ? await applyDeviceDefaults(base, device) : "";
                setTimeout(() => { if (dlg && dlg.open) dlg.close(); }, POPUP_DONE_MS);   // a success closes the popup once its line was read, as an update's does
                return `✓ MoonLight ${sys.version || label} runs.${applied}`;
            } finally {
                if (dlg) delete dlg.dataset.busy;
            }
        },
    });
    return picker;
}

// Upload the migration image, `source` a chosen file or the mirror's address, as WLED's update page does, field `update`, with skipValidation
// for WLED 16's release-name check; the reply is unreadable cross-origin, so the board's own answer says how it went: MoonBase on /api/system.
async function wledToMoonBase(base, source, tag, chip, note) {
    const image = await migrationImage(source, tag, chip, note);
    note.textContent = "Uploading to WLED: keep the power on for the next minutes…";
    const form = new FormData();
    form.append("skipValidation", "1");
    form.append("update", image, "shared-migrate.bin");
    // A plain fetch, opaque across origins: an upload with progress listeners asks a CORS preflight first (the XMLHttpRequest spec), which
    // WLED answers with no CORS header, so the image would never leave; WLED reports no progress of its own either, so this upload has no bar.
    try { await fetch(`${base}/update`, { method: "POST", body: form, mode: "no-cors" }); } catch (_) { /* a restart cuts the reply */ }
    note.textContent = "WLED restarts into MoonBase…";
    if (await waitForSystem(base, (sys) => sys.moonbase, 90000)) return;
    throw new Error("still WLED after a minute and a half: WLED refused the upload. A newer WLED checks the release name until Skip validation is ticked on its update page; an update lock or password refuses it too");
}

// The migration image: a chosen file as it is, else downloaded from the release's mirror.
async function migrationImage(source, tag, chip, note) {
    if (typeof source !== "string") return source;
    note.textContent = "Fetching the migration image…";
    const res = await fetch(source).catch(() => null);
    if (!res || !res.ok) throw new Error(`${tag} carries no migration image for the ${chip}: choose one from this computer, or move it by cable through the web installer`);
    return res.blob();
}

// A Tasmota device's way to MoonLight over the network: its chip from Discovery, the owner's one-time SetOption128 in Tasmota's console, then the install picker.
function buildTasmotaMigrate(row) {
    const box = document.createElement("div");
    box.className = "graph-popup-firmware";
    const line = document.createElement("div");
    line.textContent = `Tasmota ${row.version || ""}${row.chip ? ` on an ${row.chip}` : ""}.`;
    box.appendChild(line);
    if (!row.chip) { line.textContent += " Its chip shows once Discovery has asked its page."; return box; }
    const base = `http://${row.ip}`;
    const extra = document.createElement("div");
    extra.className = "graph-popup-extras";
    const how = document.createElement("div");
    how.className = "graph-popup-file";
    // Until Tasmota answers the map: where to switch it on. The link is Tasmota's main page, since its console serves only a request whose Referer is Tasmota itself until SetOption128 is 1.
    const ask = document.createElement("span");
    const tasmotaLink = document.createElement("a");
    tasmotaLink.href = `${base}/`;
    tasmotaLink.target = "_blank";
    tasmotaLink.rel = "noopener";
    tasmotaLink.textContent = "open Tasmota";
    ask.append("First, once: ", tasmotaLink, ", go to Tools → Console and enter SetOption128 1, which lets the map reach Tasmota's update.");
    const done = document.createElement("span");
    done.textContent = "✓ SetOption128 is on: the map reaches Tasmota's update.";
    done.hidden = true;
    how.append(ask, done);
    const file = document.createElement("label");
    file.className = "graph-popup-file";
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".bin";
    file.append("Migration image, for a release without one ", input);
    extra.append(how, file);
    // Install waits until Tasmota answers the map, which it does once SetOption128 is 1, checked again every few seconds while the popup shows.
    let open = false, picker = null;
    const check = async () => {
        if (!box.isConnected) return;   // the popup closed: the box is in it from the moment it opens
        open = await tasmotaApiOpen(base);
        ask.hidden = open;
        done.hidden = !open;
        picker?.refresh();
        setTimeout(check, 3000);
    };
    box.appendChild(goingBackLink("Tasmota"));
    mountMapPicker(box, base, row.chip, null, extra, async (binaryUrl, tag, note) => {
        if (!confirm("Move this Tasmota device to MoonLight over WiFi? Going back to Tasmota needs a USB cable.")) return null;
        await tasmotaToMoonBase(base, input.files[0] || migrateImageUrl(tag, row.chip), tag, row.chip, note);
        return moonBaseToMoonLight(base, binaryUrl, note);
    }, () => (open ? "" : "Enter SetOption128 1 in Tasmota's Console first")).then((p) => { picker = p; check(); });
    return box;
}

// An ESPHome device's way to MoonLight over the network: ESPHome takes an image only through its own update protocol, which a browser cannot speak,
// so the device serving the list, at `home`, hands it on (`/api/relay/esphome`), the image's MD5 computed here since ESPHome asks for it before the image.
function buildEsphomeMigrate(row, home) {
    const box = document.createElement("div");
    box.className = "graph-popup-firmware";
    const line = document.createElement("div");
    line.textContent = `ESPHome ${row.version || ""}${row.chip ? ` on an ${row.chip}` : ""}.`;
    box.appendChild(line);
    if (!row.chip) { line.textContent += " Its chip shows once its mDNS record has named it."; return box; }
    const file = document.createElement("label");
    file.className = "graph-popup-file";
    const input = document.createElement("input");
    input.type = "file";
    input.accept = ".bin";
    file.append("Migration image, for a release without one ", input);
    const base = `http://${row.ip}`;
    box.appendChild(goingBackLink("ESPHome"));
    mountMapPicker(box, base, row.chip, null, file, async (binaryUrl, tag, note) => {
        if (!confirm("Move this ESPHome device to MoonLight over WiFi? Going back to ESPHome needs a USB cable, or ESPHome's own tools.")) return null;
        await esphomeToMoonBase(home, row.ip, base, input.files[0] || migrateImageUrl(tag, row.chip), tag, row.chip, note);
        return moonBaseToMoonLight(base, binaryUrl, note);
    });
    return box;
}

// The image to the ESPHome device at `ip` through the device at `relay`, which speaks ESPHome's update protocol and answers how it went; then MoonBase.
async function esphomeToMoonBase(relay, ip, base, source, tag, chip, note) {
    const image = await migrationImage(source, tag, chip, note);
    note.textContent = "Handing the image to ESPHome through this device: keep the power on for the next minutes…";
    const md5 = md5Hex(new Uint8Array(await image.arrayBuffer()));
    const res = await fetch(`${relay}/api/relay/esphome?host=${encodeURIComponent(ip)}&md5=${md5}`, {
        method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: image }).catch(() => null);
    if (!res) throw new Error("this device did not answer the relay");
    if (!res.ok) throw new Error(`ESPHome refused the image: ${await errorMessage(res)}`);
    if (await waitForSystem(base, (sys) => sys.moonbase, 90000)) return;
    throw new Error("ESPHome took the image but no MoonBase answered within a minute and a half");
}

// The MD5 of `bytes` as 32 hex digits (RFC 1321), which ESPHome's update asks for before the image; the browser's own crypto has no MD5.
function md5Hex(bytes) {
    const s = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
    const k = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
    const padded = new Uint8Array(((bytes.length + 8) >> 6 << 6) + 64);
    padded.set(bytes);
    padded[bytes.length] = 0x80;
    const view = new DataView(padded.buffer);
    view.setUint32(padded.length - 8, (bytes.length * 8) >>> 0, true);
    view.setUint32(padded.length - 4, Math.floor(bytes.length / 2 ** 29), true);
    let [a0, b0, c0, d0] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
    for (let off = 0; off < padded.length; off += 64) {
        let [a, b, c, d] = [a0, b0, c0, d0];
        for (let i = 0; i < 64; i++) {
            const r = i >> 4;
            const f = r === 0 ? (b & c) | (~b & d) : r === 1 ? (d & b) | (~d & c) : r === 2 ? b ^ c ^ d : c ^ (b | ~d);
            const g = r === 0 ? i : r === 1 ? (5 * i + 1) & 15 : r === 2 ? (3 * i + 5) & 15 : (7 * i) & 15;
            const sum = (a + f + k[i] + view.getUint32(off + g * 4, true)) | 0;
            const sh = s[r * 4 + (i & 3)];
            [a, d, c] = [d, c, b];
            b = (b + ((sum << sh) | (sum >>> (32 - sh)))) | 0;
        }
        [a0, b0, c0, d0] = [(a0 + a) | 0, (b0 + b) | 0, (c0 + c) | 0, (d0 + d) | 0];
    }
    return [a0, b0, c0, d0].map(w => Array.from({ length: 4 }, (_, i) => ((w >>> (8 * i)) & 255).toString(16).padStart(2, "0")).join("")).join("");
}

// Whether Tasmota answers a command from the map: with SetOption128 off it drops the request unanswered, which the browser reports as a network error;
// on, it answers, which a no-cors request sees as an opaque reply.
async function tasmotaApiOpen(base) {
    return !!(await fetchWithin(`${base}/cm?cmnd=SetOption128`, 2500, () => true, { mode: "no-cors" }));
}

// The two steps Tasmota's own update page runs: `/u4?u4=fct` restarts it into safeboot, then the image goes to `/u2`, field `u2`. Safeboot takes an upload only
// after a `fct` it answered itself marked one as coming, and each upload clears the mark, so `fct` goes again before every upload, as Tasmota's page repeats it.
// Tasmota sends no CORS header, so the page reads none of its answers: it watches the board drop off and come back, and confirms by MoonBase answering on /api/system.
async function tasmotaToMoonBase(base, source, tag, chip, note) {
    const image = await migrationImage(source, tag, chip, note);
    note.textContent = "Restarting Tasmota into its safeboot…";
    await fetch(`${base}/u4?u4=fct&api=`, { mode: "no-cors" }).catch(() => null);
    if (await waitForRestart(base, 12000, 30000) === "back") await fwSleep(3000);   // back from the restart, its web server answers before its upload is ready; a board that stayed up was in safeboot already
    // Twice at most: an upload safeboot was not ready for, or cut off, reads here like a refusal, the reply being unreadable.
    for (let attempt = 1; attempt <= 2; attempt++) {
        await fetch(`${base}/u4?u4=fct&api=`, { mode: "no-cors" }).catch(() => null);   // the mark safeboot needs before it takes an upload
        note.textContent = `Uploading to Tasmota's safeboot${attempt > 1 ? ", again" : ""}: keep the power on for the next minutes…`;
        const form = new FormData();
        form.append("u2", image, "shared-migrate.bin");
        try { await fetch(`${base}/u2?fsz=${image.size}`, { method: "POST", body: form, mode: "no-cors" }); } catch (_) { /* a restart cuts the reply */ }
        if (await waitForSystem(base, (sys) => sys.moonbase, 60000)) return;
    }
    throw new Error("still Tasmota after two uploads: its safeboot did not take the image, or SetOption128 is not 1 in its Console");
}

// A move's last step, from a board MoonBase holds: the shared install flow hands MoonBase the firmware's address and follows it until MoonLight
// runs, on the note and a bar. MoonLight's /api/system once it runs; the flow's failure as the error. The picker's status line says how it ended.
async function moonBaseToMoonLight(base, binaryUrl, note) {
    let failure = null;
    await noteInstall(base, "app", { url: binaryUrl }, note, (text, ok) => { note.textContent = ""; if (!ok) failure = text; }, { inMoonBase: true });
    if (failure) throw new Error(failure);
    return (await fetchJsonWithin(`${base}/api/system`)) || {};   // the flow saw the app answer: a summary lost to a blip still reads as done
}

// The board's /api/system every three seconds until `done` holds for it or `ms` pass; the answer, or null.
async function waitForSystem(base, done, ms) {
    return pollUntil(() => fetchJsonWithin(`${base}/api/system`, 2500), done, ms, 3000);
}

// A device model's defaults onto the new MoonLight as one state document, the one the web installer sends over serial; WiFi and Ethernet
// are wired in code, so a model's Network replace keeps them and the board stays reachable.
async function applyDeviceDefaults(base, model) {
    if (!model || !model.state) return "";
    try {
        const res = await fetch(`${base}/api/state`, { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify(model.state) });
        return res.ok ? ` ${model.name}'s defaults applied.` : ` ${model.name}'s defaults refused: ${await errorMessage(res)}.`;
    } catch (_) {
        return ` ${model.name}'s defaults did not reach it.`;
    }
}

// A running MoonLight takes a device model's defaults as a moved one does: the models its firmware runs on, the one it reports picked, pressed
// twice since the defaults replace the modules they name. Offline, where the catalog does not load, the popup leaves the row out.
async function offerDeviceDefaults(summary, base, fw) {
    const support = await loadMapDeviceSupport();
    const fits = ((support && await support.loadDevices()) || []).filter(m => m.state && (m.firmwares || []).includes(fw.firmware));
    if (!fits.length) return;
    const row = document.createElement("div");
    row.className = "graph-popup-defaults";
    const select = document.createElement("select");
    for (const m of fits) {
        const opt = document.createElement("option");
        opt.value = opt.textContent = m.name;
        opt.selected = m.name === fw.summary?.board;
        select.appendChild(opt);
    }
    const btn = document.createElement("button");
    btn.className = "graph-popup-update";
    btn.textContent = "Apply device defaults";
    btn.title = "Set this device model's recommended modules and settings, replacing the modules it names";
    const note = document.createElement("div");
    armPressTwice(btn, async () => {
        btn.disabled = true;
        note.textContent = "Applying…";
        note.textContent = (await applyDeviceDefaults(base, fits.find(m => m.name === select.value))).trim();
        btn.disabled = false;
    }, { armedText: "✓ Apply device defaults" });
    row.append(select, btn);
    summary.append(row, note);
}

// A device's summary lines, each only when the device reports it: its board, identity, health, what
// drives its lights and the effects it runs.
function peerSummaryLines(sys) {
    const lines = [];
    const board = [sys.board, sys.chip].filter(Boolean).join(" · ");
    if (board) lines.push(`Board: ${board}`);
    if (sys.mac) lines.push(`MAC: ${sys.mac}`);
    const health = [sys.uptime !== undefined ? `up ${fmtTime(sys.uptime)}` : "", sys.fps !== undefined ? `${sys.fps} fps` : "",
                    sys.freeHeap ? `${fmtBytes(sys.freeHeap)} free` : "", sys.rssi ? `WiFi ${sys.rssi} dBm` : ""].filter(Boolean);   // 0 heap on a host, 0 signal off WiFi
    if (health.length) lines.push(`Health: ${health.join(" · ")}`);
    const outputs = graphOutputs(sys);
    if (outputs) lines.push(`Lights: ${outputs}`);
    if (Array.isArray(sys.effects) && sys.effects.length) lines.push(`Effects: ${sys.effects.join(", ")}`);
    return lines;
}

// Blink a device's lights, through its own Drivers card's identify button.
async function identifyPeer(base, note) {
    note.textContent = "Blinking for five seconds…";
    try {
        const res = await fetch(`${base}/api/control`, {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ module: "Drivers", control: "identify", value: 1 }) });
        if (!res.ok) note.textContent = `Refused: ${await errorMessage(res)}`;
    } catch (_) {
        note.textContent = "The device does not answer.";
    }
}

function buildPeerSummary(row, home) {
    const box = document.createElement("div");
    box.className = "graph-popup-firmware";
    const summary = document.createElement("div");
    summary.className = "graph-popup-summary";
    const line = document.createElement("div");
    line.textContent = "Firmware: reading…";
    const note = document.createElement("div");
    box.append(summary, line, note);
    const base = peerBase(row, home);
    (async () => {
        const fw = await peerFirmware(base);
        if (!fw) { line.textContent = "Firmware: the device does not answer."; return; }
        if (fw.summary) {
            for (const text of peerSummaryLines(fw.summary)) {
                const el = document.createElement("div");
                el.textContent = text;
                summary.appendChild(el);
            }
            // A device that reports its outputs has the identify button; both came in one firmware.
            if (Array.isArray(fw.summary.outputs)) {
                const idNote = document.createElement("div");
                const btn = document.createElement("button");
                btn.className = "graph-popup-identify";
                btn.textContent = "Identify";
                btn.title = "Blink this device's lights for five seconds, at their own brightness";
                btn.addEventListener("click", () => identifyPeer(base, idNote));
                summary.append(btn, idNote);
            }
            offerDeviceDefaults(summary, base, fw);
        }
        line.textContent = `Firmware: ${fw.version} · ${fw.firmware || "unknown"}`;
        const update = fw.summary?.update;
        const releases = await loadReleases({ maxAgeMs: UPDATE_TTL_MS });   // null when GitHub does not answer and none are kept
        const u = releases && peerUpdate(deviceOf(fw), releases);
        if (!u) {
            const lead = releases ? "The app is up to date" : "GitHub does not answer, so whether a newer release exists is unknown";
            note.textContent = `${lead}.`;
            offerImageUpdates(box, base, fw, note, releases, lead);
            return;
        }
        if (!u.url) {
            note.textContent = noImageNote(u, fw);
            return;
        }
        note.textContent = `${u.label} is available.`;
        const btn = document.createElement("button");
        btn.className = "graph-popup-update";
        btn.textContent = `Update to ${u.label}`;
        armPressTwice(btn, () => popupInstall(base, "app", { url: u.url, sha256: u.sha256 }, { version: u.version }, note, btn),
                      { armedText: "✓ " + btn.textContent });
        box.appendChild(btn);
    })();
    return box;
}

// The image that brings a device's MoonBase level with its app, shared-moonbase-<chip>.bin of the release its app ships in, with the SHA-256 GitHub
// names for it; a null url when that release carries none for the chip.
function moonbaseAsset(rel, chip) {
    return chip ? releaseAsset(rel, moonbaseAssetName(moonbaseAssetKeyFrom(chip))) : { url: null, sha256: null };
}

// With no app update offered, MoonBase and the WiFi coprocessor are each offered while the device reports them behind its app, pressed twice like the
// app's update, after `lead`, what the note says of the app. The coprocessor takes the image of the device's own release by itself; MoonBase takes
// the one the app's release in `releases` carries for its chip.
function offerImageUpdates(box, base, fw, note, releases, lead) {
    const sys = fw.summary || {};
    const offers = [];
    if (sys.moonbaseMismatch) {
        const a = moonbaseAsset(releaseOfVersion(releases, fw.version), sys.chip);
        if (a.url) offers.push({ label: "MoonBase", kind: "moonbase", source: { url: a.url, sha256: a.sha256 }, flag: "moonbaseMismatch" });
    }
    if (sys.coprocessorMismatch) offers.push({ label: "WiFi coprocessor", kind: "coprocessor", source: { url: "" }, flag: "coprocessorMismatch" });
    if (!offers.length) return;
    note.textContent = `${lead}; ${offers.map(o => o.label).join(" and ")} ${offers.length > 1 ? "are" : "is"} behind the app.`;
    for (const o of offers) {
        const btn = document.createElement("button");
        btn.className = "graph-popup-update";
        btn.textContent = `Update ${o.label}`;
        armPressTwice(btn, () => popupInstall(base, o.kind, o.source, { flag: o.flag }, note, btn), { armedText: "✓ " + btn.textContent });
        box.appendChild(btn);
    }
}

// How long a finished install's line stays before its popup closes, as the Firmware card's overlay shows its line before the page reloads.
export const POPUP_DONE_MS = 2000;

// The shared flow reported on `note` and a bar beneath it, the bar gone once the flow ended; `end(text, ok)` hears how it ended.
function noteInstall(base, kind, source, note, end, opts) {
    const bar = document.createElement("progress");   // no value: the sweep until the first byte count
    bar.className = "graph-popup-progress";
    note.insertAdjacentElement("afterend", bar);
    const ended = (ok) => (text) => { bar.remove(); end(text, ok); };
    return installFirmware(base, kind, source, {
        status: (text) => { note.textContent = text; },
        busy: (text) => { note.textContent = text; bar.removeAttribute("value"); },
        progress: (read, total) => { bar.max = total; bar.value = read; },
        fail: ended(false),
        done: ended(true),
    }, opts);
}

// A popup's install: a success closes the popup once its line was read, and a failure frees the button again.
function popupInstall(base, kind, source, target, note, btn) {
    btn.disabled = true;
    return noteInstall(base, kind, source, note, (text, ok) => {
        note.textContent = text;
        if (!ok) { btn.disabled = false; return; }
        const dlg = note.closest("dialog");
        setTimeout(() => { if (dlg && dlg.open) dlg.close(); }, POPUP_DONE_MS);
    }, { target });
}

// --- Group membership: join or leave a group the device serving the list holds -------------------------------------------------------

// The device groups a device's state (`GET /api/state`) holds, each its id, shared name, module name and member rows {id, mac}: a holder edits
// its groups whether or not it is a member itself, so its state, not its own Discovery row, says which. Pure.
export function heldGroups(state) {
    const out = [];
    const walk = (m) => {
        if (Array.isArray(m)) { m.forEach(walk); return; }   // the state's `modules`, the top-level list
        if (!m || typeof m !== "object") return;
        if (m.role === "devicegroup") {   // the module's role, as the UI reads a module rather than naming its type
            const c = Object.fromEntries((m.controls || []).map(x => [x.name, x.value]));
            if (c.id) out.push({ id: c.id, name: c.groupName || m.name, module: m.name, members: Array.isArray(c.members) ? c.members : [] });
        }
        for (const child of m.children || []) walk(child);
    };
    walk(state?.modules);
    return out;
}

const sameMac = (a, b) => String(a || "").toLowerCase() === String(b || "").toLowerCase();

// The groups a node can join or leave from here, each with whether the node with `mac` is a member. Pure.
export function membershipOffers(groups, mac) {
    return (groups || []).map(g => ({ ...g, member: g.members.some(r => sameMac(r.mac, mac)) }));
}

// The row id of the member with `mac` in a held group, or null. Pure.
export function memberRowId(group, mac) {
    const row = (group?.members || []).find(r => sameMac(r.mac, mac));
    return row ? row.id : null;
}

// Add the node's MAC to the group's members on the holder, or remove its row; an error text, or "" when the holder took the edit.
async function editMembership(base, grp, mac, join) {
    const list = `${base}/api/list/${encodeURIComponent(grp.module)}/members`;
    const send = (url, method, body) => fetch(url, { method, headers: { "Content-Type": "application/json" }, body: body && JSON.stringify(body) });
    try {
        if (!join) {
            const id = memberRowId(grp, mac);
            if (id == null) return "";   // already gone
            const res = await send(`${list}/${id}`, "DELETE");
            return res.ok ? "" : `Refused: ${await errorMessage(res)}`;
        }
        const added = await send(list, "POST");
        if (!added.ok) return `Refused: ${await errorMessage(added)}`;
        const { id } = await added.json();
        const set = await send(`${list}/${id}`, "PATCH", { field: "mac", value: mac });
        return set.ok ? "" : `Refused: ${await errorMessage(set)}`;
    } catch (err) {
        return `The device did not answer: ${err.message}`;
    }
}

// The popup's Groups section: a Join or Leave button per group the device serving the list holds, Leave pressed twice. Shown only when
// that device holds a group; a member joining or leaving follows within seconds, as the map's next refresh shows.
function buildGroupMembership(row, home) {
    const box = document.createElement("div");
    box.className = "graph-popup-firmware";
    box.hidden = true;
    (async () => {
        const state = await fetchJsonWithin(`${home}/api/state`).catch(() => null);
        const offers = membershipOffers(heldGroups(state), row.mac);
        if (!offers.length) return;
        const head = document.createElement("strong");
        head.textContent = "Groups";
        const note = document.createElement("div");
        note.className = "graph-popup-note";
        box.append(head);
        for (const grp of offers) box.appendChild(membershipButton(grp, row, home, note));
        box.append(note);
        box.hidden = false;
    })();
    return box;
}

function membershipButton(grp, row, home, note) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "graph-popup-update";
    btn.textContent = `${grp.member ? "Leave" : "Join"} ${grp.name}`;
    const run = async () => {
        btn.disabled = true;
        note.textContent = grp.member ? `Leaving ${grp.name}…` : `Joining ${grp.name}…`;
        const err = await editMembership(home, grp, row.mac, !grp.member);
        note.textContent = err || `${grp.member ? "Left" : "Joined"} ${grp.name}: every member follows within seconds.`;
        if (err) btn.disabled = false;
    };
    if (grp.member) armPressTwice(btn, run, { armedText: `✓ Leave ${grp.name}` });
    else btn.addEventListener("click", run);
    return btn;
}
