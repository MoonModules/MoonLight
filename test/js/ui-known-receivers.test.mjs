// A receivers text shows which board each address is, and which address each board name holds, as the device list knows them.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

const knownReceiversText = new Function(`return ${fnSource("knownReceiversText")}`)();

test("each known entry reads with its counterpart, in the list's order", () => {
    assert.equal(knownReceiversText({ "192.168.1.125": "MM-S31", "MM-Eye": "192.168.1.30" }),
        "192.168.1.125 is MM-S31 · MM-Eye is 192.168.1.30");
});

test("a receivers text naming only unknown boards shows an empty list", () => {
    assert.equal(knownReceiversText(undefined), "");
    assert.equal(knownReceiversText({}), "");
});
