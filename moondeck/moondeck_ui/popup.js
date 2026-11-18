// MoonDeck's own section of a map node's popup, the `actions` hook of src/ui/devicepopup.js: what needs this computer and its source tree.
// A board on this computer's USB is identified, flashed with a local build and monitored, its port found by its MAC when the action runs
// (moondeck/_usb.py), the output in the Log pane as a card's run shows it. A MoonLight device is selected for the live tests, by
// MAC, and takes its firmware's local build over the network.

import { peerFirmware, hasMoonBase } from "/ui/deviceinstall.js";
import { popupInstall } from "/ui/devicepopup.js";

const NATIVE_USB = "esp32 (native-usb)";   // a native-USB board whose descriptor names no chip (moondeck/_usb.py)

function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
}

function button(text, title) {
    const b = el("button", "graph-popup-update", text);
    b.type = "button";
    b.title = title;
    return b;
}

// A JSON POST to MoonDeck, its answer, or {error} when MoonDeck does not answer.
async function post(path, body) {
    try {
        const res = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
        return await res.json();
    } catch (err) {
        return { error: String(err) };
    }
}

// A card's script run for this node, its output in the Log pane, which the popup closes to show.
function runCard(id, params, dlg) {
    dlg.close();
    window.moondeck.runCard(id, params);
}

// The node's section: its USB board's actions, its live-test selection and its network install, each where it applies.
export function moondeckActions(row, dlg) {
    const box = el("div", "graph-popup-firmware");
    const note = el("div");
    const builds = fetch("/api/local-builds").then(r => r.json()).then(j => j.builds || []).catch(() => []);
    const parts = [];
    if (row.usb) parts.push(usbActions(row, note, dlg, builds));
    if (row.type === "MoonLight" && row.mac) parts.push(liveTestToggle(row, note));
    if (row.type === "MoonLight" && row.ip && !row.port) parts.push(networkInstall(row, note, builds));
    if (!parts.length) return;
    box.append(el("strong", "", "MoonDeck"), ...parts, note);
    dlg.appendChild(box);
}

// The board's port as the map last read it, Identify for a board whose MAC is unknown, and for a known one Flash local build and Monitor,
// both on the port its MAC is found on when pressed.
function usbActions(row, note, dlg, builds) {
    const wrap = el("div", "graph-popup-summary");
    const chip = row.chip && row.chip !== NATIVE_USB ? row.chip : "";
    wrap.append(el("div", "", `USB: ${row.usb.replace("/dev/cu.", "")}${chip ? ` · ${chip}` : ""}${row.mac ? "" : " · MAC unknown"}`));
    const actions = el("div", "moondeck-popup-actions");
    const pick = el("select");
    pick.title = "The local build to flash, from build/esp32-<firmware>/";
    if (!row.mac) {
        const identify = button("Identify", "Read this board's chip and MAC with esptool, which resets the board. "
                                            + "The MAC is remembered by its USB adapter, so the board is known on any port from then on.");
        identify.addEventListener("click", async () => {
            identify.disabled = true;
            note.textContent = "Identifying: the board resets…";
            const info = (await post("/api/identify-ports", { ports: [row.usb] })).probed?.[row.usb];
            note.textContent = info?.mac
                ? `${info.chip || "ESP32"} · ${info.mac}. The map joins it to its network node on its next read.`
                : "The board did not answer: a monitor may hold its port.";
            identify.disabled = false;
        });
        actions.append(identify);
    } else {
        const flash = button("Flash local build", "Flash the picked build over USB, on the port this board's MAC is found on now. "
                                                  + "esptool reads the board's MAC first and stops on another board.");
        flash.disabled = true;
        builds.then(async list => {
            // The firmware the board says it runs: its row's, else what the popup's own firmware line reads from the board (peerFirmware), for an
            // older board whose Discovery row and summary omit it.
            const runs = row.firmware || (row.ip ? (await peerFirmware(`http://${row.ip}${row.port ? `:${row.port}` : ""}`))?.firmware : "");
            // That firmware's chip, ahead of the USB descriptor's or the adapter cache's, which an old probe may have left behind; a board whose
            // chip stays unknown is offered no build, so only a build of its own chip can be the default.
            const want = list.find(b => b.firmware === runs)?.chip || chip || "";
            const fit = want ? list.filter(b => b.chip === want) : [];
            for (const b of fit) {
                const opt = el("option", "", b.firmware);
                opt.value = b.firmware;
                opt.selected = b.firmware === runs;
                pick.appendChild(opt);
            }
            flash.disabled = !fit.length;
            if (!fit.length) flash.title = want ? `No local build for ${want}: build one on the ESP32 tab.` : "The board's chip is unknown: flash it from the ESP32 tab.";
        });
        flash.addEventListener("click", () => runCard("flash_esp32", { mac: row.mac, firmware: pick.value, device_model: row.board || "" }, dlg));
        actions.append(pick, flash);
    }
    const monitor = button("Monitor", "The serial monitor on this board's port, in the Log pane; its firmware's build decodes a crash's backtrace.");
    monitor.addEventListener("click", () => runCard("monitor_esp32", {
        ...(row.mac ? { mac: row.mac } : { port: row.usb }),
        firmware: pick.value || (row.firmware && row.firmware !== "unknown" ? row.firmware : ""),
    }, dlg));
    actions.append(monitor);
    wrap.append(actions);
    return wrap;
}

// Whether the live tests run against this device, stored by its MAC in moondeck.json.
function liveTestToggle(row, note) {
    const label = el("label", "moondeck-popup-actions");
    const box = el("input");
    box.type = "checkbox";
    box.disabled = true;
    const mac = row.mac.toLowerCase();
    fetch("/api/state").then(r => r.json()).then(s => { box.checked = (s.liveTests || []).includes(mac); box.disabled = false; }).catch(() => {});
    box.addEventListener("change", async () => {
        const want = box.checked;
        box.disabled = true;   // one save at a time
        const res = await post("/api/live-tests", { mac, selected: want });
        if (res.error) {   // the save did not reach MoonDeck: the box shows the last confirmed value again
            box.checked = !want;
            note.textContent = "The live-test selection was not saved.";
        }
        box.disabled = false;
    });
    label.title = "The Live tab's tests run against every device selected here, found by its MAC when they start";
    label.append(box, el("span", "", "Use in live tests"));
    return label;
}

// Install local build: the board's firmware's local build, which MoonDeck serves as a file, as the popup's own updates install (popupInstall):
// an upload with its bar, through MoonBase where the board writes its app from it. A host updates from its own Firmware card, so only a board's
// row offers it.
function networkInstall(row, note, builds) {
    const btn = button("Install local build", "");
    btn.hidden = true;
    builds.then(list => {
        const build = list.find(b => b.firmware === row.firmware);
        if (!build) return;
        btn.hidden = false;
        btn.title = `Upload the local ${build.firmware} build to this device over the network, no USB`;
    });
    btn.addEventListener("click", async () => {
        btn.disabled = true;
        const base = `http://${row.ip}`;
        note.textContent = "Reading the local build…";
        const file = await fetch(`/api/local-build/${encodeURIComponent(row.firmware)}`).then(r => (r.ok ? r.blob() : null)).catch(() => null);
        if (!file) { note.textContent = `No local ${row.firmware} build: run Build on the ESP32 tab.`; btn.disabled = false; return; }
        const fw = await fetch(`${base}/api/modules/Firmware`).then(r => r.json()).catch(() => null);
        await popupInstall(base, "app", { file }, { viaMoonBase: hasMoonBase(fw) }, note, btn);
    });
    return btn;
}
