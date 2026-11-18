// One install flow for a device's firmware: the app, MoonBase or the WiFi coprocessor, from a URL the device fetches or a file the browser pushes.
// The device page's Firmware card runs it behind its full-screen overlay; a map node's popup, on the device page and on MoonDeck alike, runs it
// over its note and bar. What differs between the two is the device's address and the small UI it reports on.

import { fwSleep, pollUntil, errorMessage, installProgress, uploadWithProgress, fmtBytes, fetchWithin } from "./uikit.js";
import { downloadImage } from "./releases.js";

// The images a device installs, by the Firmware card's tab: the tab's label, the name the flow's lines use, the routes for a URL and an upload,
// and whether the device restarts once the image is written.
export const INSTALL_KINDS = {
    app:         { label: "App",         name: "the app",              reboots: true,
                   urlRoute: "/api/firmware/url",                    uploadRoute: "/api/firmware/upload" },
    moonbase:    { label: "MoonBase",    name: "MoonBase",             reboots: false,
                   urlRoute: "/api/firmware/moonbase-update-url",    uploadRoute: "/api/firmware/moonbase-update" },
    coprocessor: { label: "WiFi coproc", name: "the WiFi coprocessor", reboots: true,
                   urlRoute: "/api/firmware/coprocessor-update-url", uploadRoute: "/api/firmware/coprocessor-update" },
};

export const MOONBASE_SILENT_MSG = "MoonBase did not answer. If its WiFi fell back, join the MoonBase access point and open http://4.3.2.1";

const WATCH_MS = 300000;   // five minutes for the device to finish, three more for each retry after an interrupted MoonBase install

// MoonBase's status line from the device at `base`, or null when MoonBase is not what answers: the app 404s /moonbase.
export function moonbaseStatus(base) {
    return fetchWithin(`${base}/moonbase`, 2500, (r) => (r.ok ? r.text() : null));
}

// What the device answers: {app: phase} while the app runs, its Firmware module's status; {moonbase: phase} while MoonBase runs; null while it
// is away, as through a restart. Each ask is bounded, so a device that is off the network leaves the deadlines standing.
async function probeDevice(base) {
    const fw = await fetchWithin(`${base}/api/modules/Firmware`, 2500, (r) => (r.ok ? r.json() : false));
    // A phase is published at status severity; at rest the module may still say something, a MoonBase or radio chip behind the app, as a warning.
    if (fw) return { app: fw.severity === "warning" ? "" : fw.status || "" };
    if (fw === null) return null;   // no answer at all; a refusal is MoonBase, which serves no modules
    const text = await moonbaseStatus(base);
    return text === null ? null : { moonbase: text };
}

// Whether MoonBase answers within `ms`, as it does once a restart into it has run.
export async function waitForMoonBase(base, ms) {
    return !!(await pollUntil(async () => (await moonbaseStatus(base)) !== null, (up) => up, ms, 2000));
}

// How a restart goes: "back" once the device dropped off and answers again, "up" when it answered throughout for `upMs`, null when it is away
// past `ms`. Only whether it answers counts, an opaque reply included, so a device that sends no CORS header (Tasmota) is followed as well.
// A fixed wait reloaded a Teensy still copying its image onto the browser's connection-refused page.
export async function waitForRestart(base, upMs, ms) {
    const start = Date.now();
    let dropped = false;
    while (Date.now() - start < ms) {
        await fwSleep(1000);
        const up = await fetchWithin(`${base}/`, 2500, () => true, { mode: "no-cors" });
        if (!up) dropped = true;
        else if (dropped) return "back";
        else if (Date.now() - start > upMs) return "up";
    }
    return null;
}

// A device's version and firmware, and its summary, /api/system, which carries them; a device on older firmware has them only in its state,
// on the Firmware and System cards. Null when it does not answer.
export async function peerFirmware(base) {
    const sys = await fetchWithin(`${base}/api/system`, 4000, (r) => (r.ok ? r.json() : {}));
    if (!sys) return null;
    if (sys.version) return { version: sys.version, firmware: sys.firmware, summary: sys };
    const st = await fetchWithin(`${base}/api/state`, 4000, (r) => r.json());
    const all = [];
    const walk = (mods) => { for (const m of mods || []) { all.push(m); walk(m.children); } };
    walk(st?.modules);
    const value = (type, name) => (all.find(m => m.type === type)?.controls || []).find(c => c.name === name)?.value;
    const version = value("FirmwareUpdateModule", "appVersion");
    return version ? { version, firmware: value("SystemModule", "firmware") } : null;
}

// Whether the device reports the install's target: exactly the version asked for, since a board whose app installs through MoonBase answers with
// MoonBase's version meanwhile, `+` its build; or the summary's mismatch flag cleared.
function reached(target, fw) {
    if (!fw) return false;
    return target.version ? fw.version === target.version : !!fw.summary && !fw.summary[target.flag];
}

// Install `source` as `kind` on the device at `base`, "" for the page's own device and `http://ip[:port]` for another, and report on `ui` until it
// ended, every outcome included. `source` is {url, sha256} the device fetches, a url of "" naming its own release's matching image, or {file}
// the browser holds. `opts.viaMoonBase`: the board writes its app from MoonBase, restarting into it. `opts.inMoonBase`: the board answers as
// MoonBase already, as a move from another firmware leaves it, so the source goes to MoonBase straight away. `opts.target`: what the device
// reports once it runs the install, {version} or {flag} (see reached), which then alone counts as done.
//
// `ui` takes status(text), busy(text) for a wait of unknown length, progress(read, total), fail(text) and done(text); setUpload(abortController),
// the cancel of a browser transfer, and watchStalls(), which the upload arms, are used where the UI has them.
export async function installFirmware(base, kind, source, ui, opts = {}) {
    const inMoonBase = kind === "app" && !!opts.inMoonBase;
    const job = { base, k: INSTALL_KINDS[kind], source, ui, target: opts.target, inMoonBase, viaMoonBase: inMoonBase || (kind === "app" && !!opts.viaMoonBase) };
    try {
        const pushed = source.file ? await pushFile(job) : await handOver(job);
        await (pushed ? afterUpload(job) : watch(job));
    } catch (err) {
        ui.fail(err.message);
    }
}

// Hand the device the URL; true when the browser pushed the image itself, as for a board lacking TLS, which answers 501 on the app's URL route.
async function handOver(job) {
    const { base, k, source, ui, viaMoonBase } = job;
    if (job.inMoonBase) {
        ui.status("Handing MoonBase the firmware…");
        const res = await toMoonBase(job).catch(() => null);
        if (!res) throw new Error(MOONBASE_SILENT_MSG);
        if (!res.ok) throw new Error(`Refused: ${await res.text()}`);   // MoonBase answers in plain text
        job.accepted = true;
        return false;
    }
    ui.status(viaMoonBase ? "Restarting into MoonBase…" : `Starting the install of ${k.name}…`);
    let res = null;
    try {
        res = await fetch(`${base}${k.urlRoute}`, { method: "POST", headers: { "Content-Type": "application/json" },
                                                    body: JSON.stringify(source.url ? { url: source.url, sha256: source.sha256 || undefined } : {}) });
    } catch (_) { /* a restart can cut the reply off; the watch sees the outcome */ }
    if (res && res.status === 501 && k === INSTALL_KINDS.app && source.url) {
        // A release asset comes from the installer's Pages copy, since GitHub's own refuses a cross-origin read; one controller cancels both transfers.
        const ctl = new AbortController();
        ui.setUpload?.(ctl);
        ui.status("Downloading the image…");
        const blob = await downloadImage(source.url, ctl.signal)
            .catch((err) => { ui.setUpload?.(null); throw new Error(ctl.signal.aborted ? "Install canceled." : `Download failed: ${err.message}`); });
        await upload(job, k.uploadRoute, blob, ctl);
        return true;
    }
    if (res && !res.ok) throw new Error(`Refused: ${await errorMessage(res)}`);
    job.accepted = !!res;   // the device took the URL; a request with no reply may have been lost on the way
    return false;
}

// Push the file the browser holds; true when the device took it. The app of a board with MoonBase goes to MoonBase: restart into it unless it
// answers as MoonBase already, push the file once it answers, and let the watch follow MoonBase writing it.
async function pushFile(job) {
    const { base, k, source, ui, viaMoonBase } = job;
    if (!viaMoonBase) { await upload(job, k.uploadRoute, source.file); return true; }
    if (!job.inMoonBase) {
        ui.status("Restarting into MoonBase…");
        const res = await fetch(`${base}/api/firmware/moonbase`, { method: "POST" }).catch(() => null);   // the restart can cut the reply off
        if (res && !res.ok) throw new Error(`Refused: ${await errorMessage(res)}`);
        if (!await waitForMoonBase(base, 120000)) throw new Error(MOONBASE_SILENT_MSG);
    }
    await fileToMoonBase(job);
    return false;
}

// Push the file to MoonBase, the raw body of its /api/firmware/upload, with progress and behind the cancel; MoonBase restarts into the app once it
// took the file. A dead socket mid-upload (power cut, WiFi drop) is no verdict: the watch sees where the device lands and hands the file over again.
async function fileToMoonBase(job) {
    try {
        await upload(job, "/api/firmware/upload", job.source.file);
        job.accepted = true;
    } catch (err) {
        // The user's cancel drops the connection, which aborts the write: the device stays waiting in MoonBase.
        if (err.message === "Install canceled.") throw new Error("Install canceled: the device is waiting in MoonBase.");
        if (err.cause?.message.startsWith("error")) throw err;   // MoonBase's own refusal
    }
}

// Hand MoonBase the URL as the whole plain-text body of its /api/firmware/url, where the app takes JSON; MoonBase restarts into the app on success.
function toMoonBase({ base, source }) {
    return fetch(`${base}/api/firmware/url`, { method: "POST", body: source.url });
}

// Upload `blob` to `route` with its progress on `ui`, behind the cancel `ctl`.
async function upload({ base, ui }, route, blob, ctl = new AbortController()) {
    ui.setUpload?.(ctl);
    ui.status(`Installing ${fmtBytes(blob.size)}…`);
    try {
        await uploadWithProgress(`${base}${route}`, blob, ui, ctl.signal);
    } catch (err) {
        throw new Error(ctl.signal.aborted ? "Install canceled." : `Install failed: ${err.message}`, { cause: err });
    } finally {
        ui.setUpload?.(null);
    }
}

// The device took the pushed image: a rebooting install waits for the restart, and a target is then confirmed by the watch.
async function afterUpload(job) {
    const { base, k, ui } = job;
    if (k.reboots) {
        ui.busy("Rebooting…");
        if (!await waitForRestart(base, 15000, 120000)) throw new Error("The device did not come back within two minutes: check it, then reload.");
    }
    if (job.target) await watch(job);
    else ui.done(`Installed ${k.name}.`);
}

// The app answering this long after the device took an install through MoonBase counts as done though no ask saw the board away: a restart
// quicker than the one-second asks leaves no silence to see. A request with no reply has to show work first, since it may have been lost on the way.
const MOONBASE_GRACE_MS = 20000;
const RESTING_ASK_EVERY = 3;   // a target's firmware is read every third ask while the device rests before any work shows, every ask after

// Follow the device through the install to its end, its phase and byte counts on `ui`, an error as the failure. Done is the target once the device
// reports it; else rest after work was seen, or answering again after the restart a rebooting install ends in. Work only counts once seen:
// the first ask can land before the install has written anything, and rest or silence then means not started yet.
async function watch(job) {
    const { base, k, ui, target, viaMoonBase } = job;
    const started = Date.now();
    let deadline = started + WATCH_MS;
    let sawWork = false, away = false, silent = 0, retries = 0, asks = 0;
    const showPhase = (text, plain) => {
        const dl = installProgress(text);
        if (dl) { ui.status(`Installing ${fmtBytes(dl.total)}…`); ui.progress(dl.read, dl.total); }
        else ui.status(plain);
    };
    ui.status(`Installing ${k.name}…`);
    while (Date.now() < deadline) {
        await fwSleep(1000);
        const p = await probeDevice(base);
        if (!p) {
            silent++;
            away = true;
            if (viaMoonBase) sawWork = true;   // MoonBase installs a URL before it serves, so going away is the install's work
            if (sawWork && k.reboots) ui.busy("Rebooting…");
            else if (silent >= 4) ui.status("The device is not answering, waiting for it to come back…");
            continue;
        }
        if ("moonbase" in p) {
            const text = p.moonbase;
            if (text === "canceled") throw new Error("Install canceled: the device is waiting in MoonBase.");
            if (text.startsWith("error")) throw new Error(text);
            if (text === "idle" && away) {
                // MoonBase up and idle once the install started: it was cut off, as a power cut does, and the staged URL is spent.
                // The browser still holds the source, so the one click survives the cut: hand it over again.
                if (retries++ >= 2) throw new Error("The install keeps getting interrupted.");
                deadline += 180000;
                ui.status("The install was interrupted, retrying…");
                if (job.source.file) await fileToMoonBase(job);   // behind the cancel, as the first push
                else await toMoonBase(job).catch(() => null);   // the asks see the outcome
            } else showPhase(text, !text || text === "idle" ? "Preparing the install…" : text);
            away = sawWork = true;   // MoonBase answering means the old app is gone
            silent = 0;
            continue;
        }
        const text = p.app;
        if (text.startsWith("error")) throw new Error(text);
        const rest = text === "" || text === "idle";
        // A bare working phase is work too: a chunked download reports only its phases, checking, erasing and writing, then rest.
        if (!rest) { showPhase(text, `Installing ${k.name}…`); sawWork = true; }
        const ended = sawWork && (rest || (k.reboots && silent > 0)) || (viaMoonBase && job.accepted && rest && Date.now() - started > MOONBASE_GRACE_MS);
        const readTarget = rest && (sawWork || away || ++asks % RESTING_ASK_EVERY === 0);
        if (target ? readTarget && reached(target, await peerFirmware(base)) : ended) {
            ui.done(!target ? `Installed ${k.name}.` : target.version ? `Updated to ${target.version}.` : `Installed ${k.name}: it matches the app.`);
            return;
        }
        silent = 0;
    }
    throw new Error(silent && viaMoonBase ? MOONBASE_SILENT_MSG : "No result after five minutes: the device's page shows where it stands.");
}
