// A device group's popup: its members with the version each runs and the update its channel offers, and one button that updates them in
// waves. The wave plan and its runner are plain functions, so a test runs them with a fake install; the popup draws each member's line
// and reports the shared install flow (deviceinstall.js) on it. The map opens it from a click on a group's box, on the device's page
// and on MoonDeck alike.

import { loadReleases } from "./install-picker.js";
import { armPressTwice } from "./uikit.js";
import { UPDATE_TTL_MS } from "./releases.js";
import { installFirmware, peerFirmware } from "./deviceinstall.js";
import { peerBase, peerUpdate, deviceOf, noImageNote } from "./devicepopup.js";

// How many members install at once after the first one went through: a swarm finishes in minutes while a bad release stops at one device.
export const GROUP_WAVE_SIZE = 3;

// The waves a group updates in, from its members `{name, self, url, skip}`: the first member that installs alone, the rest GROUP_WAVE_SIZE at a
// time, the page's own device last since its page reloads as it restarts. A member with a `skip` reason or no `url` is in no wave.
export function planWaves(members) {
    const todo = members.filter(m => m.url && !m.skip);
    const others = todo.filter(m => !m.self), own = todo.filter(m => m.self);
    const waves = others.length ? [[others[0]]] : [];
    for (let i = 1; i < others.length; i += GROUP_WAVE_SIZE) waves.push(others.slice(i, i + GROUP_WAVE_SIZE));
    return waves.concat(own.map(m => [m]));
}

// Run `waves` one after another, the members of a wave together through `install(member)`, which answers {ok, text}. The wave a member fails
// in finishes, then the waves after it stay unstarted. Answers {done, failed: [{member, text}], untouched}, each a list of members.
export async function runWaves(waves, install) {
    const done = [], failed = [];
    let at = 0;
    for (; at < waves.length && !failed.length; at++) {
        const results = await Promise.all(waves[at].map(async (member) => ({ member, ...(await install(member)) })));
        for (const r of results) (r.ok ? done : failed).push(r.ok ? r.member : { member: r.member, text: r.text });
    }
    return { done, failed, untouched: waves.slice(at).flat() };
}

// A member's state from what it reports: `fw` (peerFirmware's answer or null) and `u`, the update its channel offers (peerUpdate's answer or null).
// {skip} says why it is not updated here; no url and no skip means it is up to date.
export function memberState(row, fw, u) {
    if (row.missing) return { skip: "not heard" };
    if (row.type !== "MoonLight") return { skip: "not a MoonLight device" };
    if (!fw) return { skip: "does not answer" };
    if (!u) return { version: fw.version };
    if (!u.url) return { version: fw.version, label: u.label, skip: noImageNote(u, fw) };
    return { version: fw.version, label: u.label, url: u.url, sha256: u.sha256, target: u.version };
}

// A member's install through the shared flow, reported on `ui`: {ok, text} once it ended.
async function installMember(m, ui) {
    let result = { ok: false, text: "No result." };
    const ended = (ok) => (text) => { result = { ok, text }; ui[ok ? "done" : "fail"](text); };
    await installFirmware(m.base, "app", { url: m.url, sha256: m.sha256 }, { ...ui, done: ended(true), fail: ended(false) }, { target: { version: m.target } });
    return result;
}

const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
};

// A member's line: its name and version, a status text, and a bar the install reports through.
function memberLine(m) {
    const line = el("div", "graph-popup-member");
    const status = el("div", "graph-popup-member-status");
    const bar = el("progress", "graph-popup-progress");
    bar.hidden = true;
    line.append(el("div", "", `${m.name}${m.version ? ` · ${m.version}` : ""}`), status, bar);
    const hide = () => { bar.hidden = true; };
    const ui = {
        status: (text) => { status.textContent = text; },
        busy: (text) => { status.textContent = text; bar.hidden = false; bar.removeAttribute("value"); },
        progress: (read, total) => { bar.hidden = false; bar.max = total; bar.value = read; },
        fail: (text) => { hide(); status.textContent = text; },
        done: (text) => { hide(); status.textContent = text; },
    };
    status.textContent = m.skip ? `skipped: ${m.skip}` : m.url ? `${m.label} is available` : "up to date";
    return { line, ui };
}

// What the update ended in, for the line under the button.
function summaryText({ done, failed, untouched }) {
    if (!failed.length) return `Updated ${done.length} member${done.length === 1 ? "" : "s"}.`;
    const f = failed[0];
    return `${f.member.name} failed: ${f.text} ${untouched.length} member${untouched.length === 1 ? "" : "s"} left untouched.`;
}

// The group popup: `group` is {id, name, rows}, its members' rows; `opts` is the map's, its hooks among them.
export function openGroupPopup(group, opts) {
    const dlg = el("dialog", "graph-popup");
    const head = el("form", "graph-popup-head");
    head.method = "dialog";
    const close = el("button", "", "✕");
    close.value = "close";
    close.title = "close";
    head.append(el("span", "", group.name || "group"), close);
    const box = el("div", "graph-popup-firmware");
    const note = el("div", "", "Reading the members…");
    box.appendChild(note);
    dlg.append(head, box);
    document.body.appendChild(dlg);
    dlg.addEventListener("click", (e) => { if (e.target === dlg && !dlg.dataset.busy) dlg.close(); });
    dlg.addEventListener("cancel", (e) => { if (dlg.dataset.busy) e.preventDefault(); });
    close.addEventListener("click", (e) => { if (dlg.dataset.busy) e.preventDefault(); });
    dlg.addEventListener("close", () => dlg.remove(), { once: true });
    dlg.showModal();
    fillGroup(group, opts, box, note, dlg);
}

// Read each member and the releases, list the members, and offer the update when one is behind.
async function fillGroup(group, opts, box, note, dlg) {
    const home = opts?.hooks?.home ? opts.hooks.home() : location.origin;
    const releases = await loadReleases({ maxAgeMs: UPDATE_TTL_MS });
    const members = await Promise.all(group.rows.map(async (row) => {
        const base = peerBase(row, home);
        const ask = !row.missing && row.type === "MoonLight";
        const fw = ask ? await peerFirmware(base) : null;
        const u = fw && releases ? peerUpdate(deviceOf(fw), releases) : null;
        return { name: row.name ?? row.ip, self: !!row.self, base, ...memberState(row, fw, u) };
    }));
    const lines = new Map(members.map(m => [m, memberLine(m)]));
    box.append(...[...lines.values()].map(l => l.line));
    const waves = planWaves(members);
    note.textContent = waves.length ? `${waves.flat().length} of ${members.length} members are behind.` : releases ? "Every member is up to date or skipped." : "GitHub does not answer, so whether a newer release exists is unknown.";
    if (!waves.length) return;
    const btn = el("button", "graph-popup-update", "Update group");
    const result = el("div");
    armPressTwice(btn, async () => {
        btn.disabled = true;
        dlg.dataset.busy = "1";   // closable again however the waves end, a thrown install among them
        try {
            result.textContent = summaryText(await runWaves(waves, (m) => installMember(m, lines.get(m).ui)));
        } catch (err) {
            result.textContent = `The update stopped: ${err.message}`;
        } finally {
            delete dlg.dataset.busy;
        }
    }, { armedText: "✓ Update group" });
    box.append(btn, result);
}
