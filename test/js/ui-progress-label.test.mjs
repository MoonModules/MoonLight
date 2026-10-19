// A progress gauge reads "used / total", each number sized to its own unit, from the unit the device declares.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

const fmtProgressLabel = new Function(`${fnSource("fmtBytes")} ${fnSource("fmtProgressLabel")} return fmtProgressLabel;`)();

test("a byte gauge picks KB, MB or GB per number", () => {
    assert.equal(fmtProgressLabel({ value: 120 * 1024, total: 1408 * 1024, unit: "B" }), "120KB / 1.4MB");
    assert.equal(fmtProgressLabel({ value: 512, total: 2048 }), "512B / 2.0KB");
});

test("a KiB gauge scales first, so a 32 GB card reads as gigabytes", () => {
    assert.equal(fmtProgressLabel({ value: 1536, total: 31166976, unit: "KiB" }), "1.5MB / 30GB");
});

test("a count gauge shows plain numbers, never a byte suffix", () => {
    assert.equal(fmtProgressLabel({ value: 3000, total: 6300, unit: "count" }), "3000 / 6300");
});

test("a missing value reads as zero rather than NaN", () => {
    assert.equal(fmtProgressLabel({ unit: "B" }), "0B / 0B");
});
