// The Live tab's device map: the device UI's own map (src/ui/devicemap.js), drawn from the Discovery list of one MoonLight, which
// merges its peers' lists in as a board's page does and sits at its center: the desktop app when it runs; else the first MoonLight
// MoonDeck heard announce itself (moondeck/_presence.py); else, with none heard, what MoonDeck heard and the boards on its USB ports.
// The boards on USB join the map either way, a board also on the network marked with its port. A click on a node opens the popup a
// board's page opens (src/ui/devicepopup.js), with MoonDeck's own section added (popup.js): the USB board's Identify, Flash local build
// and Monitor, the live-test selection and the network install of a local build. A node whose channel offers a newer release is marked as
// on a board's page. The Live tab opens its pane first.

import { mountDeviceMap, fetchPeerGraph } from "/ui/devicemap.js";
import { openDevicePopup } from "/ui/devicepopup.js";
import { UPDATE_TTL_MS, channelUpdate } from "/ui/releases.js";
import { loadReleases } from "/ui/install-picker.js";
import { moondeckActions } from "./popup.js";

const DESKTOP = "localhost:8080";   // the desktop app MoonDeck's Host tab runs
const REFRESH_MS = 2000;
const DISCOVERY = { moduleName: "Discovery", ctrlName: "devices" };   // the module and list a peer's map reads too

const pane = document.getElementById("pane-map");
const container = document.getElementById("device-map");
const status = document.getElementById("map-status");

// A device's own page, at the port its row names when that is not 80.
const pageOf = (row) => `http://${row.ip}${row.port && Number(row.port) !== 80 ? `:${row.port}` : ""}`;

let listHome = null;   // the MoonLight the map's list came from, which a node's popup reads as the device serving the list

const opts = {
    graph: true,
    ...DISCOVERY,
    hooks: {
        openNode: (row, mapOpts) => openDevicePopup(row, mapOpts),
        home: () => listHome ?? location.origin,
        update: (row, releases) => channelUpdate(row, releases),
        releases: () => loadReleases({ maxAgeMs: UPDATE_TTL_MS }),
        fit: fitBox,
        actions: moondeckActions,
    },
};

// The box takes the pane's height below the status line and the filters, so the map fills the pane.
function fitBox() {
    const box = container.querySelector(":scope > .graph-box");
    if (!box) return;
    const top = box.getBoundingClientRect().top - pane.getBoundingClientRect().top;
    box.style.height = `${Math.max(200, pane.clientHeight - top - 8)}px`;
}
new ResizeObserver(fitBox).observe(pane);

// The USB boards joined to `rows`: a board whose MAC a row carries, in any form the board reports itself by (moondeck/_usb.py device_macs),
// marks that row with its port; the others join as nodes of their own.
function withUsb(rows, usb) {
    const byMac = new Map(usb.flatMap(b => (b.macs || []).map(m => [m, b])));
    const out = rows.map(r => (r.mac && byMac.has(r.mac.toLowerCase()) ? { ...r, usb: byMac.get(r.mac.toLowerCase()).usb } : r));
    const onNetwork = new Set(out.filter(r => r.usb).map(r => r.usb));
    return out.concat(usb.filter(b => !onNetwork.has(b.usb)));
}

let heardSource = null;   // the heard MoonLight the map read last, asked first next time
let busy = false;
async function refresh() {
    if (busy || !pane.classList.contains("active")) return;
    busy = true;
    try {
        const presence = await fetch("/api/presence").then(r => r.json()).catch(() => ({ rows: [], usb: [] }));
        let g = await fetchPeerGraph(`http://${DESKTOP}`, DISCOVERY);
        let from = "the desktop app", page = `http://${DESKTOP}`;   // what the status says, and where the list came from
        if (!(g.state === "ok" && g.rows.length)) {
            const heard = presence.rows.filter(r => r.type === "MoonLight" && r.ip !== "127.0.0.1");
            heard.sort((a, b) => (pageOf(b) === heardSource) - (pageOf(a) === heardSource));   // the one read last first, so the center stays put
            for (const row of heard) {
                g = await fetchPeerGraph(pageOf(row), DISCOVERY);
                if (g.state === "ok" && g.rows.length) { from = `${row.name} (${row.ip}), as no desktop app runs`; page = heardSource = pageOf(row); break; }
            }
        }
        if (g.state === "ok" && g.rows.length) {
            status.textContent = `${g.rows.length} devices, from ${from}`;
            listHome = page;
            mountDeviceMap(container, withUsb(g.rows, presence.usb), [], opts);
        } else if (presence.rows.length || presence.usb.length) {
            listHome = null;
            status.textContent = `no MoonLight answers: what MoonDeck heard${presence.listening ? "" : " (its presence port is taken)"} and its USB boards`;
            mountDeviceMap(container, withUsb(presence.rows, presence.usb), [], opts);
        } else {
            container.replaceChildren();
            status.textContent = "no desktop app runs, no MoonLight announces itself, and no board is on USB";
        }
    } finally {
        busy = false;
    }
}

// The map draws the moment its pane shows, by a tab click or by the Live tab opening it, then every two seconds while it shows.
new MutationObserver(() => { if (pane.classList.contains("active")) refresh(); }).observe(pane, { attributes: true, attributeFilter: ["class"] });
refresh();
setInterval(refresh, REFRESH_MS);
