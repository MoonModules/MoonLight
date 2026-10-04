// A password reaches the page obfuscated, a Password control's and a list field's alike, and the page decodes it for hold-to-peek.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { src, fnSource } from "./app-source.mjs";

const decodePassword = new Function(`const PW_XOR_KEY = 0x5A; ${fnSource("decodePassword")} return decodePassword;`)();

test("the page decodes what the device's writeObfuscatedPassword sends", () => {
    assert.equal(decodePassword("Ki1r"), "pw1");   // the encoding unit_WiFiModule pins on the device side
    assert.equal(decodePassword(""), "");
    assert.equal(decodePassword(undefined), "");
});

test("a list's password field shows the decoded password, as the control does", () => {
    const at = src.indexOf('} else if (f.type === "password") {');
    assert.notEqual(at, -1);
    assert.ok(src.slice(at, at + 600).includes("decodePassword(f.value)"));
});
