// The gallery and the card's document view: which presets may fill a pad unasked, what a preset needs installed with it, and how a document is shown.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const src = readFileSync(new URL("../../src/ui/app.js", import.meta.url), "utf8");

/// A top-level function's source, by brace matching.
function fnSource(name) {
    const at = src.indexOf(`function ${name}(`);
    assert.notEqual(at, -1, `${name} not found in app.js`);
    const open = src.indexOf("{", at);
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}" && --depth === 0) return src.slice(at, i + 1);
    }
    assert.fail(`unbalanced braces in ${name}`);
}

const g = new Function(`
    const GALLERY_SCRIPT_EXT = [".mle", ".mll", ".mlm", ".mls", ".mlp"];
    ${fnSource("galleryPadReady")}
    ${fnSource("galleryScriptsOf")}
    ${fnSource("galleryFileName")}
    ${fnSource("galleryPresetName")}
    ${fnSource("galleryFillCandidates")}
    ${fnSource("formatJson")}
    return { galleryPadReady, galleryScriptsOf, galleryFileName, galleryPresetName, galleryFillCandidates, formatJson };
`)();

test("a look or a palette may fill a pad unasked, and pins or geometry may not", () => {
    assert.ok(g.galleryPadReady({ Effects: { Layer: {} } }));
    assert.ok(g.galleryPadReady({ slot: 3, Effects: {} }));
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
});

test("a document is shown indented with its keys in the order written", () => {
    const text = '{"Effects":{"2":{"type":"X","s":"a,b:{c}"},"1":{},"list":[1,2]}}';
    const shown = g.formatJson(text);
    assert.ok(shown.indexOf('"2"') < shown.indexOf('"1"'));   // a parse would have moved "1" first
    assert.ok(shown.includes('"s": "a,b:{c}"'));               // punctuation inside a string is left alone
    assert.ok(shown.includes('"1": {}'));
    assert.equal(JSON.stringify(JSON.parse(shown)), JSON.stringify(JSON.parse(text)));
});

test("a pad saves the container its editor shows, not whatever a card saved last", () => {
    const editor = fnSource("openPadEditor");
    const saves = editor.split('sendControl(moduleName, "save", 1)').length - 1;
    const sources = editor.split('sendControl(moduleName, "source", container())').length - 1;
    assert.equal(saves, 2);
    assert.equal(sources, saves);
    assert.ok(!fnSource("buildCaptureToggles").includes('"Layouts", "Effects"'));   // the names come from the device
});
