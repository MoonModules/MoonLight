// The MIDI bridge's wire format, which MidiService parses: "hex hex...".
//
// encodeMidi is a pure function inside app.js, a browser script, so the test lifts its source and
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
const src = app.match(/function encodeMidi\(messages\) \{[\s\S]*?\n\}/);
assert.ok(src, "encodeMidi is in app.js");
const encodeMidi = new Function(`${src[0]}; return encodeMidi;`)();

test("a batch is each message as two-digit hex, separated by spaces", () => {
    assert.equal(encodeMidi([new Uint8Array([0xE0, 0x7F, 0x3F]), new Uint8Array([0x90, 0x68, 0x00])]),
                 "e07f3f 906800");
});
