// The install progress bar crosses the modal exactly ONCE.
//
// esptool-js reports progress per FILE: `written`/`total` restart at zero for every image, and an
// install writes several (bootloader, partition table, ota data, app, plus MoonBase where the
// layout has one). Plotted raw, the bar fills and resets once per image, five sweeps for five
// images. flashPercent weights each file by its size against the whole write.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

// The orchestrator imports esptool-js from a CDN, so Node cannot import the module (the ESM loader
// refuses an https: specifier). The other installer tests read it as text for the same reason; here
// the function under test is PURE, so lift its source and evaluate just that. Behaviour is tested,
// not a source-text pattern, and the test breaks if the real implementation changes.
const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const src = readFileSync(join(ROOT, "mooninstaller", "install-orchestrator.js"), "utf8");
const start = src.indexOf("export function flashPercent");
if (start < 0) throw new Error("flashPercent not found in install-orchestrator.js");
const end = src.indexOf("\n}", start) + 2;
const flashPercent = new Function(`${src.slice(start, end).replace("export function", "return function")}`)();

// A classic-ESP32 install with MoonBase: the app dwarfs the rest, so a per-file bar spends four
// quick sweeps on small images and one long one on the app.
const CLASSIC_ESP32 = [23168, 3072, 8192, 2059664, 794672];

/// Replay a whole flash the way esptool-js drives it: every file from 0 to its own total.
function sweep(sizes, steps = 8) {
    const pcts = [];
    sizes.forEach((size, idx) => {
        for (let s = 1; s <= steps; s++) pcts.push(flashPercent(sizes, idx, (size * s) / steps, size));
    });
    return pcts;
}

test("progress never goes backwards across a multi-image flash", () => {
    const pcts = sweep(CLASSIC_ESP32);
    for (let i = 1; i < pcts.length; i++) {
        assert.ok(pcts[i] >= pcts[i - 1],
            `progress went backwards at tick ${i}: ${pcts[i - 1]} then ${pcts[i]}`);
    }
});

test("progress reaches 100 exactly once, at the end of the last image", () => {
    const pcts = sweep(CLASSIC_ESP32);
    assert.equal(pcts[pcts.length - 1], 100);
    // The old bug: 100 appeared once per file. One hit, on the final tick, is the contract.
    assert.equal(pcts.filter(p => p === 100).length, 1);
});

test("a file's completion lands at its share of the total, not at 100", () => {
    // Finishing the bootloader (23168 of 2888768 bytes) is under 1%, not a full bar.
    const afterFirst = flashPercent(CLASSIC_ESP32, 0, CLASSIC_ESP32[0], CLASSIC_ESP32[0]);
    assert.ok(afterFirst < 2, `first image ended the bar at ${afterFirst}%`);
    // Finishing the app (the fourth image) is most of the way, but not done: MoonBase follows.
    const afterApp = flashPercent(CLASSIC_ESP32, 3, CLASSIC_ESP32[3], CLASSIC_ESP32[3]);
    assert.ok(afterApp > 60 && afterApp < 100, `app ended the bar at ${afterApp}%`);
});

test("a single-image flash still sweeps the full bar", () => {
    assert.equal(flashPercent([1000], 0, 0, 1000), 0);
    assert.equal(flashPercent([1000], 0, 500, 1000), 50);
    assert.equal(flashPercent([1000], 0, 1000, 1000), 100);
});

test("a tick with no total does not move the bar past what is already written", () => {
    // esptool-js has been seen to report total 0 on a first tick; that must read as "this file has
    // contributed nothing yet", never as a jump.
    const sizes = [100, 900];
    assert.equal(flashPercent(sizes, 1, 450, 0), 10);   // file 0 done, file 1 not counted
    assert.equal(flashPercent(sizes, 1, 450, 900), 55); // the honest tick
});

test("malformed input reports 0 rather than NaN", () => {
    for (const bad of [undefined, null, [], [0, 0], "nope"]) {
        const pct = flashPercent(bad, 0, 10, 100);
        assert.equal(typeof pct, "number");
        assert.ok(Number.isFinite(pct), `flashPercent(${JSON.stringify(bad)}) was not finite`);
        assert.equal(pct, 0);
    }
});

