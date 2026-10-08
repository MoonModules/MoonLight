// A few short choices show as a segmented control, the rest as a dropdown: a UI decision only, so the device sends a select either way.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource, src } from "./app-source.mjs";

const limits = src.match(/const SEGMENTED_MAX = (\d+), SEGMENTED_CHARS = (\d+);/);
assert.ok(limits, "the segmented limits are in app.js");
const choiceIsSegmented = new Function(`const SEGMENTED_MAX = ${limits[1]}, SEGMENTED_CHARS = ${limits[2]}; ${fnSource("choiceIsSegmented")}; return choiceIsSegmented;`)();

test("up to three short choices are segments; a fourth, or labels too long to fit, make a dropdown", () => {
    assert.equal(choiceIsSegmented({ options: ["disc", "ball", "egg"] }), true);
    assert.equal(choiceIsSegmented({ options: ["simulate", "receive network", "local audio"] }), true);
    assert.equal(choiceIsSegmented({ options: ["a", "b", "c", "d"] }), false);
    assert.equal(choiceIsSegmented({ options: ["unicast", "multicast", "multicast + broadcast"] }), false);
});

test("both render paths build a select through one builder, so a list crossing three changes form in place", () => {
    assert.ok(fnSource("createControl").includes("buildChoice("));
    assert.ok(fnSource("updateModuleControls").includes("buildChoice("));
});
