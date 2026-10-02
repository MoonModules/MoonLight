// The gamepad bridge's wire format, which GamepadService parses: "buttons,leftx,lefty,rightx,righty".
//
// encodePad is a pure function inside app.js, a browser script, so the test lifts its source and
// runs it rather than loading the page.
//
// Run: `node --test "test/js/**/*.test.mjs"`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");
const src = app.match(/function encodePad\(buttons, axes\) \{[\s\S]*?\n\}/);
assert.ok(src, "encodePad is in app.js");
const encodePad = new Function(`${src[0]}; return encodePad;`)();

test("a pad at rest encodes no buttons and every axis at 128", () => {
    assert.equal(encodePad([], [0, 0, 0, 0]), "0,128,128,128,128");
});

test("buttons become bits in the standard layout's order, and the axes span 0 to 255", () => {
    const pressed = Array(17).fill(false);
    pressed[0] = true;    // a
    pressed[12] = true;   // dpup
    assert.equal(encodePad(pressed, [-1, 1, 0, 0]), `${1 | (1 << 12)},0,255,128,128`);
});

test("a value past the stick's range is clamped rather than wrapped", () => {
    assert.equal(encodePad([], [-2, 3, 0, 0]), "0,0,255,128,128");
});
