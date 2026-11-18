// The install flow the Firmware card and a map node's popup share (deviceinstall.js): how an install starts, what it reports while the device
// works, and what ends it. Run without a browser: a scripted device answers `fetch` and an upload, and a virtual clock runs the minutes at once.
// Run: `node --test test/js/device-install.test.mjs`.

import { test, afterEach } from "node:test";
import assert from "node:assert/strict";

import { installFirmware } from "../../src/ui/deviceinstall.js";
import { pollUntil } from "../../src/ui/uikit.js";
import { INSTALLER_URL } from "../../src/ui/releases.js";

const BASE = "http://10.0.0.7";
const real = { fetch: globalThis.fetch, setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout, now: Date.now,
               XMLHttpRequest: globalThis.XMLHttpRequest };
afterEach(() => {
    globalThis.fetch = real.fetch; globalThis.setTimeout = real.setTimeout; globalThis.clearTimeout = real.clearTimeout;
    Date.now = real.now; globalThis.XMLHttpRequest = real.XMLHttpRequest;
});

// A clock that jumps to the next timer whenever nothing else can run, so five minutes of polling pass in a moment.
function virtualClock() {
    let now = 0, id = 0;
    const timers = new Map();
    globalThis.setTimeout = (fn, ms = 0) => { timers.set(++id, { at: now + ms, fn }); return id; };
    globalThis.clearTimeout = (t) => timers.delete(t);
    Date.now = () => now;
    return {
        now: () => now,
        async run(promise) {
            let settled = false;
            promise.then(() => { settled = true; }, () => { settled = true; });   // the caller handles a rejection from the promise returned
            while (!settled) {
                await new Promise((r) => setImmediate(r));
                if (settled) break;
                const [next] = [...timers.entries()].sort((a, b) => a[1].at - b[1].at);
                assert.ok(next, "the flow waits on nothing");
                timers.delete(next[0]);
                now = Math.max(now, next[1].at);
                next[1].fn();
            }
            return promise;
        },
    };
}

const reply = (status, body) => ({ ok: status >= 200 && status < 300, status,
    json: async () => (typeof body === "string" ? JSON.parse(body) : body), text: async () => (typeof body === "string" ? body : JSON.stringify(body)),
    blob: async () => new Blob([body]) });

// A device scripted by `answer(path, init)`: a reply, or null for one that does not answer. Every request is logged.
function device(answer) {
    const log = [];
    globalThis.fetch = async (url, init = {}) => {
        log.push({ url, method: init.method || "GET", body: init.body });
        if (init.signal?.aborted) throw new Error("aborted");
        const got = answer(url.startsWith(BASE) ? url.slice(BASE.length) : url, init);
        if (!got) throw new TypeError("connection refused");
        return got;
    };
    return log;
}

// The flow installing release 1.1.0 from a URL, reported on recorder `r`'s UI.
const installUpdate = (r) => installFirmware(BASE, "app", { url: "https://x/firmware.bin" }, r.ui, { target: { version: "1.1.0" } });

// An XMLHttpRequest stand-in whose upload answers 200, `send(xhr, body)` recording it and reporting its progress.
function fakeUpload(send) {
    globalThis.XMLHttpRequest = class {
        constructor() { this.upload = { on: {}, addEventListener: (t, f) => { this.upload.on[t] = f; } }; this.on = {}; }
        open(_method, route) { this.route = route; }
        setRequestHeader() {}
        addEventListener(t, f) { this.on[t] = f; }
        abort() {}
        send(body) {
            send(this, body);
            queueMicrotask(() => { this.status = 200; this.on.load(); });
        }
    };
}

// A UI that records every call the flow makes on it.
function recorder() {
    const calls = [];
    const ui = {};
    for (const m of ["status", "busy", "progress", "fail", "done", "setUpload"]) ui[m] = (...args) => calls.push([m, ...args]);
    return { ui, calls, of: (m) => calls.filter(c => c[0] === m).map(c => c.slice(1)) };
}

test("a URL install the device accepts with 202 shows its byte counts and ends once the device runs exactly the version asked for", async () => {
    const clock = virtualClock();
    let asks = 0;
    const phases = ["", "flashing: 100 of 400 bytes", "flashing: 400 of 400 bytes", null, null, ""];
    const log = device((path) => {
        if (path === "/api/firmware/url") return reply(202, { ok: true });
        if (path === "/api/modules/Firmware") {
            const phase = phases[Math.min(asks++, phases.length - 1)];
            return phase === null ? null : reply(200, { status: phase });
        }
        if (path === "/api/system") return reply(200, { version: asks >= phases.length ? "1.1.0" : "1.0.0", firmware: "esp32-d0" });
        return reply(404, "not found");
    });
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { url: "https://x/firmware.bin", sha256: "ab" }, r.ui, { target: { version: "1.1.0" } }));
    assert.deepEqual(JSON.parse(log[0].body), { url: "https://x/firmware.bin", sha256: "ab" }, "the device is handed the URL and its digest");
    assert.deepEqual(r.of("progress"), [[100, 400], [400, 400]]);
    assert.deepEqual(r.of("fail"), []);
    assert.deepEqual(r.of("done"), [["Updated to 1.1.0."]]);
});

test("only exactly the version asked for ends an install: MoonBase's own version, the same number with `+` its build, does not", async () => {
    const clock = virtualClock();
    let asks = 0;
    device((path) => {
        if (path === "/api/firmware/url") return reply(202, { ok: true, moonbase: true });
        if (path === "/api/modules/Firmware") return ++asks <= 2 ? reply(404, "not found") : reply(200, { status: "" });
        if (path === "/moonbase") return reply(200, "downloading: 50 of 200 bytes");
        if (path === "/api/system") return reply(200, { version: asks < 6 ? "1.1.0+42" : "1.1.0" });
        return null;
    });
    const r = recorder();
    await clock.run(installUpdate(r));
    assert.deepEqual(r.of("progress"), [[50, 200], [50, 200]], "MoonBase's byte counts show while it writes the app");
    assert.equal(asks, 6, "1.1.0+42 did not end it");
    assert.deepEqual(r.of("done"), [["Updated to 1.1.0."]]);
});

test("a board without TLS answers 501, so the browser downloads the image from the installer's mirror and uploads it with progress", async () => {
    const clock = virtualClock();
    const uploads = [];
    fakeUpload((xhr, body) => {
        uploads.push({ route: xhr.route, size: body.size });
        xhr.upload.on.progress({ lengthComputable: true, loaded: 3, total: 6 });
        xhr.upload.on.progress({ lengthComputable: true, loaded: 6, total: 6 });
    });
    let restarted = false, rootAsks = 0;
    const log = device((path) => {
        if (path === "/api/firmware/url") return reply(501, { error: "this device cannot fetch a URL: upload the image" });
        if (path.startsWith(INSTALLER_URL)) return reply(200, "IMAGE!");
        if (path === "/") { rootAsks++; if (rootAsks === 2) { restarted = true; return null; } return reply(200, ""); }
        if (path === "/api/modules/Firmware") return reply(200, { status: "" });
        if (path === "/api/system") return reply(200, { version: restarted ? "1.1.0" : "1.0.0" });
        return null;
    });
    const r = recorder();
    const asset = "https://github.com/MoonModules/MoonLight/releases/download/v1.1.0/firmware-esp32-d0-v1.1.0.bin";
    await clock.run(installFirmware(BASE, "app", { url: asset }, r.ui, { target: { version: "1.1.0" } }));
    assert.ok(log.some(q => q.url.startsWith(INSTALLER_URL) && q.url.endsWith("firmware-esp32-d0-v1.1.0.bin")), "the image comes from the mirror");
    assert.deepEqual(uploads, [{ route: `${BASE}/api/firmware/upload`, size: 6 }]);
    assert.deepEqual(r.of("progress"), [[3, 6], [6, 6]]);
    assert.ok(r.of("setUpload").some(([ctl]) => ctl instanceof AbortController), "the transfer can be canceled");
    assert.deepEqual(r.of("done"), [["Updated to 1.1.0."]]);
});

test("an install the device refuses fails with the device's own reason", async () => {
    const clock = virtualClock();
    device((path) => (path === "/api/firmware/url" ? reply(409, { error: "ota already in progress" }) : null));
    const r = recorder();
    await clock.run(installUpdate(r));
    assert.deepEqual(r.of("fail"), [["Refused: ota already in progress"]]);
    assert.deepEqual(r.of("done"), []);
});

test("an install that reaches no result within five minutes fails rather than waiting for ever", async () => {
    const clock = virtualClock();
    device((path) => {
        if (path === "/api/firmware/url") return reply(202, { ok: true });
        if (path === "/api/modules/Firmware") return reply(200, { status: "idle" });
        if (path === "/api/system") return reply(200, { version: "1.0.0" });
        return null;
    });
    const r = recorder();
    await clock.run(installUpdate(r));
    assert.equal(r.of("fail").length, 1);
    assert.match(r.of("fail")[0][0], /five minutes/);
    assert.ok(clock.now() >= 300000 && clock.now() <= 301000, `failed at ${clock.now()} ms, within the last one-second ask past five minutes`);
});

test("without a target, a MoonBase install ends when the device's status is back at rest after work was seen, not at the rest before it", async () => {
    const clock = virtualClock();
    let asks = 0;
    const phases = ["idle", "writing MoonBase: 5 of 10 bytes", "idle"];
    device((path) => {
        if (path === "/api/firmware/moonbase-update-url") return reply(202, { ok: true });
        if (path === "/api/modules/Firmware") return reply(200, { status: phases[Math.min(asks++, phases.length - 1)] });
        return null;
    });
    const r = recorder();
    await clock.run(installFirmware("", "moonbase", { url: "https://x/shared-moonbase-esp32.bin" }, r.ui));
    assert.equal(asks, 3, "the first rest did not end it");
    assert.deepEqual(r.of("progress"), [[5, 10]]);
    assert.deepEqual(r.of("done"), [["Installed MoonBase."]]);
});

test("an app install through MoonBase whose restart no ask saw ends once the app answers at rest twenty seconds on", async () => {
    const clock = virtualClock();
    device((path) => {
        if (path === "/api/firmware/url") return reply(202, { ok: true });
        if (path === "/api/modules/Firmware") return reply(200, { status: "" });   // back so quickly that every ask finds the app at rest
        return reply(200, { version: "1.1.0" });
    });
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { url: "https://example.org/fw.bin" }, r.ui, { viaMoonBase: true }));
    assert.equal(r.of("done").length, 1, "done, rather than failing after five minutes");
    assert.ok(clock.now() > 20000 && clock.now() < 30000, `ended at ${clock.now()} ms`);
    assert.equal(r.of("fail").length, 0);
});

test("a device at rest that warns its MoonBase is behind the app counts as at rest, so the install ends rather than waiting for ever", async () => {
    const clock = virtualClock();
    let asks = 0;
    device((path) => {
        if (path === "/api/firmware/url") return reply(202, { ok: true });
        if (path === "/api/modules/Firmware") {
            asks++;
            if (asks < 3) return reply(200, { status: `flashing: ${asks * 200} of 400 bytes`, severity: "status" });
            if (asks < 5) return null;   // restarting
            return reply(200, { status: "MoonBase does not match this app: install the matching one on the MoonBase tab", severity: "warning" });
        }
        return reply(200, { version: asks >= 5 ? "1.1.0" : "1.0.0" });
    });
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { url: "https://example.org/fw.bin" }, r.ui, { target: { version: "1.1.0" } }));
    assert.deepEqual(r.of("done"), [["Updated to 1.1.0."]]);
    assert.ok(clock.now() < 30000, `ended at ${clock.now()} ms`);
});

// A board MoonBase holds, as moonbase_main.cpp answers: the URL route reads its whole body as the address and refuses one that is not https,
// /moonbase says where the install stands, step by step from `phases`, null while the board restarts, and the app follows with its Firmware module.
function boardInMoonBase(phases) {
    let asks = 0, phase = "idle";
    return device((path, init) => {
        if (path === "/api/firmware/url") {
            if (!String(init.body).startsWith("https://")) return reply(400, "error: url must start with https://");
            return reply(202, "starting the install");
        }
        if (path === "/api/modules/Firmware") {   // each probe asks it first
            phase = phases[Math.min(asks++, phases.length - 1)];
            return phase === "app" ? reply(200, { status: "" }) : phase === null ? null : reply(404, "not found");
        }
        if (path === "/moonbase") return reply(200, phase);
        return null;
    });
}

test("a move's last step starts from MoonBase: the image's address goes to MoonBase as the plain-text body it reads, its byte counts show, and it ends once the app answers", async () => {
    const clock = virtualClock();
    const log = boardInMoonBase(["downloading: 100 of 400 bytes", "downloading: 400 of 400 bytes", "installed, restarting", null, "app"]);
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { url: "https://x/firmware.bin" }, r.ui, { inMoonBase: true }));
    assert.equal(log[0].url, `${BASE}/api/firmware/url`);
    assert.equal(log[0].body, "https://x/firmware.bin", "MoonBase reads the address as the whole body, not JSON");
    assert.ok(!log.some(q => q.url.endsWith("/api/firmware/moonbase")), "a board in MoonBase is not asked to restart into it");
    assert.deepEqual(r.of("progress"), [[100, 400], [400, 400]]);
    assert.deepEqual(r.of("fail"), []);
    assert.deepEqual(r.of("done"), [["Installed the app."]]);
});

test("a move's last step fails with MoonBase's own words when MoonBase reports the install canceled or an error", async () => {
    for (const [phase, failure] of [["canceled", "Install canceled: the device is waiting in MoonBase."],
                                    ["error: the download stalled", "error: the download stalled"]]) {
        const clock = virtualClock();
        boardInMoonBase(["downloading: 100 of 400 bytes", phase]);
        const r = recorder();
        await clock.run(installFirmware(BASE, "app", { url: "https://x/firmware.bin" }, r.ui, { inMoonBase: true }));
        assert.deepEqual(r.of("fail"), [[failure]], phase);
        assert.deepEqual(r.of("done"), [], phase);
    }
});

test("a move's last step that MoonBase refuses fails with MoonBase's plain-text reason", async () => {
    const clock = virtualClock();
    boardInMoonBase(["app"]);
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { url: "http://x/firmware.bin" }, r.ui, { inMoonBase: true }));
    assert.deepEqual(r.of("fail"), [["Refused: error: url must start with https://"]]);
});

test("an app install through MoonBase whose request got no reply ends only once the device is seen at work, not on twenty seconds of rest", async () => {
    const clock = virtualClock();
    device((path) => {
        if (path === "/api/firmware/url") return null;   // the request was lost on the way
        if (path === "/api/modules/Firmware") return reply(200, { status: "" });   // the old app, untouched
        return reply(200, { version: "1.0.0" });
    });
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { url: "https://example.org/fw.bin" }, r.ui, { viaMoonBase: true }));
    assert.deepEqual(r.of("done"), [], "an untouched app is no install");
    assert.match(r.of("fail")[0]?.[0] || "", /five minutes/);
});

test("a file MoonBase lost to a power cut goes again as the first push did, with progress and behind the cancel", async () => {
    const clock = virtualClock();
    const uploads = [];
    fakeUpload((xhr, body) => {
        uploads.push(xhr.route);
        xhr.upload.on.progress({ lengthComputable: true, loaded: body.size, total: body.size });
    });
    boardInMoonBase([null, "idle", "app"]);   // away, then MoonBase idle: the write was cut off
    const r = recorder();
    await clock.run(installFirmware(BASE, "app", { file: new Blob(["IMAGE"]) }, r.ui, { inMoonBase: true }));
    assert.deepEqual(uploads, [`${BASE}/api/firmware/upload`, `${BASE}/api/firmware/upload`], "both pushes are uploads with progress");
    assert.equal(r.of("setUpload").filter(([ctl]) => ctl instanceof AbortController).length, 2, "each push can be canceled");
    assert.deepEqual(r.of("done"), [["Installed the app."]]);
});

test("a poll asks for the last time at its deadline, and stops there", async () => {
    const clock = virtualClock();
    const asked = [];
    const got = await clock.run(pollUntil(async () => { asked.push(clock.now()); return Date.now() > 10000 ? "late" : null; }, () => true, 10000, 3000));
    assert.equal(got, null, "no answer from past the deadline counts");
    assert.deepEqual(asked, [3000, 6000, 9000, 10000]);
});
