// The gallery and the card's document view: which presets may fill a pad unasked, what a preset needs installed with it, and how a document is shown.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { src, fnSource } from "./app-source.mjs";

// The "try now" handler alone, from its button to the title set right after it, so a growing handler or a later fetch cannot move the ordering checks.
const tryNowHandler = () => src.slice(src.indexOf('button("try now"'), src.indexOf("now.title", src.indexOf('button("try now"')));

const g = new Function(`
    const GALLERY_SCRIPT_EXT = [".mle", ".mll", ".mlm", ".mls", ".mlp"];
    ${fnSource("galleryAppliesUnasked")}
    ${fnSource("galleryScriptsOf")}
    ${fnSource("galleryFileName")}
    ${fnSource("galleryPresetName")}
    ${fnSource("galleryUrl")}
    ${fnSource("galleryPath")}
    ${fnSource("formatJson")}
    const GALLERY_PER_PAGE = 12;
    ${fnSource("galleryMatches")}
    ${fnSource("gallerySorted")}
    ${fnSource("galleryPage")}
    return { galleryAppliesUnasked, galleryScriptsOf, galleryFileName, galleryPresetName, galleryUrl, formatJson,
             galleryMatches, gallerySorted, galleryPage, galleryPath };
`)();

test("a look or a palette applies unasked, and pins or geometry ask first", () => {
    assert.ok(g.galleryAppliesUnasked({ Effects: { Layer: {} } }));
    assert.ok(g.galleryAppliesUnasked({ $slot: 3, Effects: {} }));
    assert.ok(g.galleryAppliesUnasked({ Drivers: { palette: "Ocean" } }));
    assert.ok(!g.galleryAppliesUnasked({ Drivers: { palette: "Ocean", brightness: 40 } }));
    assert.ok(!g.galleryAppliesUnasked({ Layouts: { Grid: {} } }));
    assert.ok(!g.galleryAppliesUnasked({ Effects: {}, Drivers: { palette: "Ocean" } }));
    assert.ok(!g.galleryAppliesUnasked([1, 2]));
});

test("a preset names the scripts it needs as control values", () => {
    const doc = { Effects: { Layer: { Fluid: { type: "MoonLiveEffect", script: "fluid.mle", speed: 3 } } },
                  Drivers: { paletteScript: "Warm.MLP" } };
    assert.deepEqual([...g.galleryScriptsOf(doc)].sort(), ["Warm.MLP", "fluid.mle"]);
});

test("a preset brings the scripts the device lacks: the gallery's copy first, else the one the firmware ships", async () => {
    const wrote = [], downloaded = [];
    const fetchScripts = new Function("deviceScriptNames", "galleryFetch", "writeDeviceFile", "mlFetchCatalog", "mlDownloadScript", `
        const GALLERY_SCRIPT_EXT = [".mle", ".mll", ".mlm", ".mls", ".mlp"];
        ${fnSource("galleryScriptsOf")}
        ${fnSource("galleryFileName")}
        ${fnSource("mlGroupForExt")}
        return ${fnSource("galleryFetchScripts")}`)(
        async () => new Set(["fluid.mle"]),
        async (file) => `text of ${file}`,
        async (dir, name) => { wrote.push(`${dir}/${name}`); },
        async () => ({ effects: { names: ["comet-trail.mle", "fluid.mle"] } }),
        async (name, group) => { downloaded.push(`${group}/${name}`); });
    const index = [{ kind: "Effect script", file: "scripts/0009-swirl.mle" }];
    const doc = { Effects: { L: { A: { script: "comet-trail.mle" }, B: { script: "swirl.mle" }, C: { script: "fluid.mle" } } } };
    await fetchScripts(doc, index);
    assert.deepEqual(wrote, ["/moonlive/swirl.mle"]);            // from the gallery
    assert.deepEqual(downloaded, ["effects/comet-trail.mle"]);   // shipped, not yet on the device
    // fluid.mle is already there and stays; a script found nowhere refuses the preset, naming it.
    await assert.rejects(fetchScripts({ Effects: { L: { A: { script: "gone.mle" } } } }, index), /gone\.mle is neither in the gallery nor in this firmware/);
    const tryNow = tryNowHandler();
    assert.ok(tryNow.indexOf("galleryFetchScripts(") > 0 && tryNow.indexOf("galleryFetchScripts(") < tryNow.indexOf('fetch("/api/state"'));
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
    const tryNow = tryNowHandler();
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

// A hundred entries, as a grown gallery holds: every fourth a script, votes falling with the issue number.
const hundred = Array.from({ length: 100 }, (_, k) => ({
    issue: k + 1, kind: k % 4 === 3 ? "Effect script" : "Preset", name: `Look ${k + 1}`,
    description: k === 41 ? "a harbor at night" : "", author: k === 7 ? "wompslab" : "ewowi", votes: 100 - k,
}));

test("a hundred entries show twelve to a page, and a page past the end lands on the last", () => {
    const first = g.galleryPage(hundred, 0);
    assert.equal(first.items.length, 12);
    assert.equal(first.pages, 9);
    const last = g.galleryPage(hundred, 99);
    assert.equal(last.page, 8);
    assert.equal(last.items.length, 4);
    assert.deepEqual(g.galleryPage([], 3), { items: [], page: 0, pages: 1 });
});

test("search covers the name, the description and the author, and a kind chip narrows to one kind", () => {
    const find = (q, kind = "") => hundred.filter(e => g.galleryMatches(e, q, kind)).map(e => e.issue);
    assert.deepEqual(find("look 10"), [10, 100]);
    assert.deepEqual(find("HARBOR"), [42]);
    assert.deepEqual(find("wompslab"), [8]);
    assert.equal(find("", "Effect script").length, 25);
    assert.deepEqual(find("look 10", "Effect script"), [100]);
    assert.equal(find("  ").length, 100);   // a blank search hides nothing
});

test("the gallery sorts by most liked or by newest", () => {
    const index = [{ issue: 1, votes: 2 }, { issue: 2, votes: 5 }, { issue: 3, votes: 5 }, { issue: 4 }];
    assert.deepEqual(g.gallerySorted(index, "liked").map(e => e.issue), [2, 3, 1, 4]);
    assert.deepEqual(g.gallerySorted(index, "newest").map(e => e.issue), [4, 3, 2, 1]);
});

test("browsing loads thumbnails only, and the full-size preview waits until an entry opens", () => {
    const view = fnSource("renderGallery");
    const browse = view.slice(view.indexOf("const drawBrowse"), view.indexOf("const draw = "));
    assert.ok(browse.includes("e.thumb"));
    assert.ok(!browse.includes("e.media") && !browse.includes("galleryMedia("));
    const detail = view.slice(view.indexOf("const drawDetail"), view.indexOf("const drawBrowse"));
    assert.ok(detail.includes("galleryMedia("));
});

test("a thumbnail path from the index stays inside the gallery repository", () => {
    assert.equal(g.galleryPath("thumbs/0003.webp"), "thumbs/0003.webp");
    assert.equal(g.galleryPath("../MoonLight/main/x.png"), null);
    assert.equal(g.galleryPath("/etc/passwd"), null);
    assert.equal(g.galleryPath("https://evil.example/x.png"), null);
    assert.equal(g.galleryPath(undefined), null);
});
