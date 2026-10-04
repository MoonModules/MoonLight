// The gallery and the card's document view: which presets may fill a pad unasked, what a preset needs installed with it, and how a document is shown.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { src, fnSource } from "./app-source.mjs";

const g = new Function(`
    const GALLERY_SCRIPT_EXT = [".mle", ".mll", ".mlm", ".mls", ".mlp"];
    ${fnSource("galleryPadReady")}
    ${fnSource("galleryScriptsOf")}
    ${fnSource("galleryFileName")}
    ${fnSource("galleryPresetName")}
    ${fnSource("galleryFillCandidates")}
    ${fnSource("galleryUrl")}
    ${fnSource("formatJson")}
    return { galleryPadReady, galleryScriptsOf, galleryFileName, galleryPresetName, galleryFillCandidates, galleryUrl, formatJson };
`)();

test("a look or a palette may fill a pad unasked, and pins or geometry may not", () => {
    assert.ok(g.galleryPadReady({ Effects: { Layer: {} } }));
    assert.ok(g.galleryPadReady({ $slot: 3, Effects: {} }));
    assert.ok(g.galleryPadReady({ Drivers: { palette: "Ocean" } }));
    assert.ok(!g.galleryPadReady({ Drivers: { palette: "Ocean", brightness: 40 } }));
    assert.ok(!g.galleryPadReady({ Layouts: { Grid: {} } }));
    assert.ok(!g.galleryPadReady({ Effects: {}, Drivers: { palette: "Ocean" } }));
    assert.ok(!g.galleryPadReady([1, 2]));
});

test("a preset names the scripts it needs as control values", () => {
    const doc = { Effects: { Layer: { Fluid: { type: "MoonLiveEffect", script: "fluid.mle", speed: 3 } } },
                  Drivers: { paletteScript: "Warm.MLP" } };
    assert.deepEqual([...g.galleryScriptsOf(doc)].sort(), ["Warm.MLP", "fluid.mle"]);
});

test("a gallery file lands under its own name, and a preset name the device accepts", () => {
    assert.equal(g.galleryFileName({ file: "scripts/0007-fluid.mle" }), "fluid.mle");
    assert.equal(g.galleryPresetName({ name: "Ocean 2.0 / warm", file: "presets/0001-x.json" }), "Ocean 2-0 - warm");
    assert.equal(g.galleryPresetName({ name: "x".repeat(40), file: "presets/0001-x.json" }).length, 31);
    assert.equal(g.galleryPresetName({ name: "Lumière ✨", file: "presets/0001-x.json" }), "Lumi-re -");   // printable ASCII only, as the device takes
});

test("a gallery link is kept only when it is http or https", () => {
    assert.equal(g.galleryUrl("https://github.com/x"), "https://github.com/x");
    assert.equal(g.galleryUrl("javascript:alert(1)"), null);
    assert.equal(g.galleryUrl(undefined), null);
});

test("empty pads fill with the most liked presets the device does not hold yet", () => {
    const index = [
        { issue: 1, kind: "Preset", name: "a", votes: 2 },
        { issue: 2, kind: "Effect script", name: "s", votes: 9 },
        { issue: 3, kind: "Preset", name: "b", votes: 5 },
        { issue: 4, kind: "Preset", name: "c", votes: 5 },
        { issue: 5, kind: "Preset", name: "mine", votes: 8 },
    ];
    assert.deepEqual(g.galleryFillCandidates(index, ["mine"]).map(e => e.name), ["b", "c", "a"]);
    // Two entries mapping to one device name: only the more liked one is tried, so neither overwrites the other.
    const twins = [{ issue: 7, kind: "Preset", name: "Ocean", votes: 1 }, { issue: 8, kind: "Preset", name: "Ocean", votes: 4 }];
    assert.deepEqual(g.galleryFillCandidates(twins, []).map(e => e.issue), [8]);
});

test("a document is shown indented with its keys in the order written", () => {
    const text = '{"Effects":{"2":{"type":"X","s":"a,b:{c}"},"1":{},"list":[1,2]}}';
    const shown = g.formatJson(text);
    assert.ok(shown.indexOf('"2"') < shown.indexOf('"1"'));   // a parse would have moved "1" first
    assert.ok(shown.includes('"s": "a,b:{c}"'));               // punctuation inside a string is left alone
    assert.ok(shown.includes('"1": {}'));
    assert.equal(JSON.stringify(JSON.parse(shown)), JSON.stringify(JSON.parse(text)));
});

test("a pad saves the container its editor shows, through the one save path", () => {
    const editor = fnSource("openPadEditor");
    assert.equal(editor.split("savePresetFrom(container()").length - 1, 2);
    assert.ok(!fnSource("buildCaptureToggles").includes('"Layouts", "Effects"'));   // the names come from the device
});

test("a refused name stops the save, so it never lands under the name before it", async () => {
    const calls = [];
    const save = new Function("sendControl", "presetModuleName",
        `return ${fnSource("savePresetFrom")}`)(async (m, c) => { calls.push(c); return c !== "name"; }, () => "Control");
    assert.equal(await save("Effects", "bad.name", 3), false);
    assert.deepEqual(calls, ["source", "name"]);
});

test("every gallery download refuses a failed response before anything is written or applied", async () => {
    const fetchFile = new Function("fetch", "GALLERY_RAW", `${fnSource("galleryFresh")} return ${fnSource("galleryFetch")}`)(
        async () => ({ ok: false, status: 404, text: async () => "404: Not Found" }), "https://x/");
    await assert.rejects(fetchFile("presets/0001-a.json"), /download failed \(404\)/);
    const tryNow = src.slice(src.indexOf('now.textContent = "try now"'), src.indexOf('now.textContent = "try now"') + 2000);
    assert.ok(tryNow.indexOf("galleryFetch(") > 0 && tryNow.indexOf("galleryFetch(") < tryNow.indexOf('fetch("/api/state"'));
    assert.ok(!src.includes("fetch(GALLERY_RAW + e.file)"));   // no download bypasses it
});

// An entry someone just shared must show for the person they asked to try it, so neither the browser nor GitHub's raw cache may hold the old index.
test("the gallery index and files are fetched past GitHub's cache, and the index at most once per 30 seconds", async () => {
    const at = (secondsAgo, data) => JSON.stringify({ ts: Date.now() - secondsAgo * 1000, data });
    const run = (stored, call) => new Function("stored", `
        const UPDATE_TTL_MS = 60 * 60 * 1000;
        const GALLERY_RAW = "https://raw.example/";
        const inFlightFetches = {};
        const urls = [];
        const safeLocalGet = () => stored;
        const safeLocalSet = () => {};
        const fetch = async (url) => { urls.push(url); return { ok: true, json: async () => ["fresh"], text: async () => "file" }; };
        ${fnSource("cachedJson")}
        ${src.match(/const GALLERY_TTL_MS = [^;]+;/)[0]}
        ${fnSource("galleryFresh")}
        ${fnSource("galleryIndex")}
        ${fnSource("galleryFetch")}
        return (${call})().then(result => ({ result, urls }));
    `)(stored);
    const reused = await run(at(10, ["cached"]), "galleryIndex");
    assert.deepEqual(reused, { result: ["cached"], urls: [] });
    const fresh = await run(at(40, ["cached"]), "galleryIndex");
    assert.deepEqual(fresh.result, ["fresh"]);
    assert.match(fresh.urls[0], /^https:\/\/raw\.example\/index\.json\?t=\d+$/);
    const file = await run(null, "() => galleryFetch('presets/0003-night-harbor.json')");
    assert.match(file.urls[0], /^https:\/\/raw\.example\/presets\/0003-night-harbor\.json\?t=\d+$/);
});
