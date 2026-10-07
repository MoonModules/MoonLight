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

const changesSrc = app.match(/function deskChanges\(sent, desk\) \{[\s\S]*?\n\}/);
assert.ok(changesSrc, "deskChanges is in app.js");
const deskChanges = new Function(`${changesSrc[0]}; return deskChanges;`)();
const hex = msgs => msgs.map(m => Array.from(m, b => b.toString(16).padStart(2, "0")).join(""));

test("the desk is sent every slot once, then only the slots that changed", () => {
    const sent = [];
    assert.deepEqual(hex(deskChanges(sent, "e07f7f 90187f b03021")), ["e07f7f", "90187f", "b03021"]);
    assert.deepEqual(hex(deskChanges(sent, "e07f7f 90187f b03021")), []);
    assert.deepEqual(hex(deskChanges(sent, "e00000 90187f b03021")), ["e00000"]);
});

test("a slot that is not a channel message never reaches the desk", () => {
    assert.deepEqual(hex(deskChanges([], "000000 f07f7f e07f zz7f7f 90187f")), ["90187f"]);
});

const helloSrc = app.match(/function helloMessages\(hello\) \{[\s\S]*?\n\}/);
assert.ok(helloSrc, "helloMessages is in app.js");
const helloMessages = new Function(`${helloSrc[0]}; return helloMessages;`)();

test("a desk's greeting goes out as whole SysEx and channel messages, and nothing malformed", () => {
    assert.deepEqual(hex(helloMessages("f0477f2960000442010000f7 b03802 f0477f zz 000000")),
                     ["f0477f2960000442010000f7", "b03802"]);
    assert.deepEqual(helloMessages(""), []);
});
