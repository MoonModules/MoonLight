// The page helpers the device page and the node popup share: byte and duration formats, a wait, a poll, a bounded fetch, a failed
// reply's message, a press-twice button and an install's progress, read and uploaded. Generic and self-contained.

// Compact byte formatter: the largest of B, KB, MB and GB that keeps the number at 1 or more, with one decimal under 10.
export function fmtBytes(n) {
    if (n < 1024) return n + "B";
    const units = ["KB", "MB", "GB"];
    let k = n / 1024, u = 0;
    while (k >= 1024 && u < units.length - 1) { k /= 1024; u++; }
    return (k < 10 ? k.toFixed(1) : Math.round(k)) + units[u];
}

// A duration in seconds as its days, hours, minutes and seconds, "1d 2h 3m 4s", the leading zero units left out.
export function fmtTime(sec) {
    sec = Math.max(0, Math.floor(Number(sec) || 0));
    const d = Math.floor(sec / 86400); sec -= d * 86400;
    const h = Math.floor(sec / 3600);  sec -= h * 3600;
    const m = Math.floor(sec / 60);    sec -= m * 60;
    const parts = [];
    if (d) parts.push(d + "d");
    if (d || h) parts.push(h + "h");
    if (d || h || m) parts.push(m + "m");
    parts.push(sec + "s");
    return parts.join(" ");
}

// A promise that settles after `ms`, for a wait between two asks of a device.
export const fwSleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Ask `read` every `everyMs` until `done` holds for its answer or `ms` pass; that answer, or null. The one wait for a device the Firmware card
// and the Devices map share.
export async function pollUntil(read, done, ms, everyMs = 3000) {
    const deadline = Date.now() + ms;
    while (Date.now() < deadline) {
        await fwSleep(everyMs);
        const got = await read();
        if (got && done(got)) return got;
    }
    return null;
}

// A reply read by `read` within `ms`, body included, or null: a device that answers and then stalls releases its caller at the time, and a
// device that does not answer leaves the caller's deadline standing. `init` carries the request's other options.
export async function fetchWithin(url, ms, read, init = {}) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), ms);
    try { return await read(await fetch(url, { cache: "no-store", ...init, signal: ctl.signal })); }
    catch (_) { return null; }
    finally { clearTimeout(timer); }
}

// The message for a failed fetch Response: the server's own `{"error": …}` body (JSON, e.g.
// "not enough space (N free)") when present, else a bare `HTTP <status>`. Every /api/* handler
// returns errors as that JSON shape, so this is the one place the extraction lives.
export async function errorMessage(res) {
    try {
        const j = await res.json();
        if (j && j.error) return j.error;
    } catch (_) { /* non-JSON error body: fall through to the status code */ }
    return `HTTP ${res.status}`;
}

// Wire a button as a press-twice confirm: the first click arms it (adds the
// `armed` class, optional armed label/title), a second click runs `onConfirm`.
// Disarms after 3s or when the pointer leaves. Used by the delete and reboot
// buttons: no browser confirm() popup. `armedText` is optional (delete swaps
// 🗑 → ✓; reboot keeps its glyph). The pre-arm title is captured live so a
// title updated elsewhere (e.g. reboot's crashed-state text) restores correctly.
export function armPressTwice(btn, onConfirm, opts = {}) {
    let armed = false;
    let disarmTimer = null;
    let savedText = "";
    let savedTitle = "";
    const disarm = () => {
        // Only restore label/title if we armed: otherwise a mouseleave before the first
        // click would write the initial empty savedText/savedTitle over the button, collapsing a
        // text-sized button (e.g. "🗑 delete") to an empty sliver.
        if (!armed) return;
        armed = false;
        btn.classList.remove("armed");
        if (opts.armedText !== undefined) btn.textContent = savedText;
        btn.title = savedTitle;
        if (disarmTimer) { clearTimeout(disarmTimer); disarmTimer = null; }
    };
    btn.addEventListener("click", () => {
        // Disarm before running the action so a stray second click can't fire
        // it twice (e.g. two /api/reboot requests).
        if (armed) { disarm(); onConfirm(); return; }
        armed = true;
        savedText = btn.textContent;
        savedTitle = btn.title;
        btn.classList.add("armed");
        if (opts.armedText !== undefined) btn.textContent = opts.armedText;
        if (opts.armedTitle !== undefined) btn.title = opts.armedTitle;
        disarmTimer = setTimeout(disarm, 3000);
    });
    btn.addEventListener("mouseleave", disarm);
}

// Byte counts out of a device status line, or null when it carries none.
//
// ONE PARSER for one wire shape. Every image writer reports progress as "<phase>: N of M bytes",
// and the phase word differs by writer (`flashing`, `writing MoonBase`, MoonBase's own
// `downloading`), so matching the numbers rather than the phrase is what lets one reader serve
// them all. Two hand-written regexes had already drifted apart on whether `bytes` was required.
//
// An ERROR line can carry the same shape ("upload ended early (12 of 34 bytes)"), so it is
// rejected here rather than left to the caller's check order.
export function installProgress(text) {
    if (!text || text.startsWith("error")) return null;
    const m = /(\d+) of (\d+) bytes/.exec(text);
    if (!m) return null;
    const read = parseInt(m[1], 10), total = parseInt(m[2], 10);
    return total > 0 ? { read, total } : null;
}

// POST a file and paint a REAL progress bar while it uploads.
//
// fetch() cannot report upload progress at all (its streaming request bodies are not available
// here), so this is the one place an XMLHttpRequest earns its keep: xhr.upload.onprogress is the
// only way to know how far a browser-pushed install has got. The device-fetched installs report
// through the byte counts in their status line instead, so every install has a bar.
export function uploadWithProgress(route, file, ui, signal) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        // The overlay's cancel aborts through `signal`, which an XMLHttpRequest takes as a listener rather than an option.
        if (signal) {
            if (signal.aborted) { reject(new Error("canceled")); return; }
            signal.addEventListener("abort", () => xhr.abort(), { once: true });
        }
        // The install overlay watches for stalls; the File Manager's drop upload shows only progress.
        if (ui.watchStalls) ui.watchStalls();
        xhr.open("POST", route);
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.upload.addEventListener("progress", (e) => {
            if (e.lengthComputable) ui.progress(e.loaded, e.total);
        });
        xhr.addEventListener("load", () => {
            // The device answers before it reboots, so a 2xx is the real success signal. Anything
            // else carries the device's own reason in the JSON body.
            if (xhr.status >= 200 && xhr.status < 300) { resolve(); return; }
            let msg = `HTTP ${xhr.status}`;
            try { msg = JSON.parse(xhr.responseText).error || msg; } catch (_) { msg = xhr.responseText || msg; }   // MoonBase answers in plain text
            reject(new Error(msg));
        });
        xhr.addEventListener("error", () => reject(new Error("the connection dropped")));
        xhr.addEventListener("abort", () => reject(new Error("canceled")));
        xhr.send(file);
    });
}
