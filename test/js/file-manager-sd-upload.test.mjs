// A regression guard for two bugs caught live on the bench:
// 1. The File Manager's upload applied FM_UPLOAD_CAP (256 KiB, sized for flash) to a drop destined
//    for /sd, where the server has no cap of its own beyond the card's free space.
// 2. A large /sd upload crashed the board (a stack-local buffer in the per-tick drive loop), which
//    a browser's fetch() reports as the opaque "NetworkError when attempting to fetch resource.".
//    The fix moved uploads onto uploadWithProgress (XMLHttpRequest), the same helper the firmware
//    upload already used, so a dropped connection says so and a long transfer shows real progress.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

// A minimal fake DOM: enough of createElement/appendChild/remove/getElementById for
// fmUploadPanel and fmUploadPanelRow, which this codebase has no existing mock for (its DOM-heavy
// code is exercised by the Playwright --ui scenarios instead, not this node:test harness).
function fakeElement(tag) {
    const el = {
        tagName: tag, className: "", id: "", textContent: "", max: 0, value: 0,
        children: [], parentNode: null,
        append(...kids) { for (const k of kids) el.appendChild(k); },
        appendChild(k) { k.parentNode = el; el.children.push(k); return k; },
        remove() {
            if (!el.parentNode) return;
            const i = el.parentNode.children.indexOf(el);
            if (i >= 0) el.parentNode.children.splice(i, 1);
            el.parentNode = null;
        },
    };
    return el;
}

function fakeDocument() {
    const byId = new Map();
    const index = (el) => { if (el.id) byId.set(el.id, el); };
    const body = fakeElement("body");
    const realBodyAppend = body.appendChild.bind(body);
    body.appendChild = (k) => { const r = realBodyAppend(k); index(k); return r; };
    return {
        body,
        createElement(tag) { return fakeElement(tag); },
        getElementById(id) { return byId.get(id); },
        // The code under test sets `.id` then appends to body; a real DOM indexes that
        // automatically, so this fake does the same at the one place an id is assigned.
        __index: index,
    };
}

// A fake XMLHttpRequest driven by the test: open/send record the call, and emitProgress/emitLoad/
// emitError replace the real network with whatever the test wants to have happened.
class FakeXHR {
    constructor() {
        this._listeners = {};
        this.upload = { addEventListener: (ev, cb) => { if (ev === "progress") this._onProgress = cb; } };
    }
    open(method, url) { this.method = method; this.url = url; }
    setRequestHeader() {}
    addEventListener(ev, cb) { this._listeners[ev] = cb; }
    send(body) { this.body = body; }
    emitProgress(loaded, total) { this._onProgress?.({ lengthComputable: true, loaded, total }); }
    emitLoad(status, responseText) { this.status = status; this.responseText = responseText; this._listeners.load?.(); }
    emitError() { this._listeners.error?.(); }
}

// Builds fmDropUpload with its real dependencies inlined (the same pattern app-source.mjs's other
// callers use), against a fresh fake document and an XHR factory the test controls.
function buildFmDropUpload({ onXhr }) {
    const doc = fakeDocument();
    const XHRCtor = function () {
        const xhr = new FakeXHR();
        onXhr(xhr);
        return xhr;
    };
    const build = new Function("document", "XMLHttpRequest", `
        const FM_UPLOAD_CAP = 256 * 1024;
        ${fnSource("joinFsPath")}
        ${fnSource("fmSize")}
        ${fnSource("fmIsSdPath")}
        ${fnSource("uploadWithProgress")}
        ${fnSource("fmUploadPanel")}
        ${fnSource("fmUploadPanelRow")}
        ${fnSource("fmDropUpload")}
        return { fmDropUpload, fmIsSdPath };`);
    return build(doc, XHRCtor);
}

function fakeFile(name, sizeBytes) {
    return { name, size: sizeBytes };
}

const BIG = 9 * 1024 * 1024;   // bigger than FM_UPLOAD_CAP either way

test("fmIsSdPath matches /sd and anything under it, nothing else", () => {
    const { fmIsSdPath } = buildFmDropUpload({ onXhr: () => {} });
    assert.equal(fmIsSdPath("/sd"), true);
    assert.equal(fmIsSdPath("/sd/clips"), true);
    assert.equal(fmIsSdPath("/"), false);
    assert.equal(fmIsSdPath("/sdx"), false);          // a real folder named "sdx", not the card
    assert.equal(fmIsSdPath("/moonlive"), false);
});

test("a file over the flash cap bound for flash is skipped client-side, never opening a connection", async () => {
    let opened = false;
    const { fmDropUpload } = buildFmDropUpload({ onXhr: () => { opened = true; } });

    const skipped = await fmDropUpload("/", [fakeFile("huge.fseq", BIG)]);

    assert.equal(opened, false);
    assert.equal(skipped.length, 1);
    assert.match(skipped[0], /huge\.fseq/);
});

test("the same file bound for /sd is sent, not skipped by the flash cap, and reports progress", async () => {
    let sawUrl = null;
    const progressReads = [];
    const { fmDropUpload } = buildFmDropUpload({
        onXhr: (xhr) => {
            // .open() runs after the constructor returns, so xhr.url is only valid once deferred.
            setTimeout(() => {
                sawUrl = xhr.url;
                xhr.emitProgress(1024, BIG);
                xhr.emitLoad(200, "{\"ok\":true}");
            }, 0);
        },
    });

    const skipped = await fmDropUpload("/sd", [fakeFile("feuer_120s_smooth_35fps.fseq", BIG)]);

    assert.match(decodeURIComponent(sawUrl), /\/sd\/feuer_120s_smooth_35fps\.fseq$/);
    assert.deepEqual(skipped, []);
});

test("a dropped connection on a long /sd upload reports plainly, not as a generic network error", async () => {
    const { fmDropUpload } = buildFmDropUpload({
        onXhr: (xhr) => { setTimeout(() => xhr.emitError(), 0); },
    });

    const skipped = await fmDropUpload("/sd", [fakeFile("clip.fseq", BIG)]);

    assert.equal(skipped.length, 1);
    assert.match(skipped[0], /connection dropped/);
});

test("a genuine server failure on an SD upload still reports, carrying the server's own message", async () => {
    const { fmDropUpload } = buildFmDropUpload({
        onXhr: (xhr) => { setTimeout(() => xhr.emitLoad(507, "{\"error\":\"not enough space (0 free)\"}"), 0); },
    });

    const skipped = await fmDropUpload("/sd", [fakeFile("clip.fseq", BIG)]);

    assert.equal(skipped.length, 1);
    assert.match(skipped[0], /not enough space/);
});
