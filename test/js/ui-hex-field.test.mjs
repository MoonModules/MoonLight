// A hex number field shows an I2C address as the datasheet and the bus scan write it, and the live patch keeps it that way.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { src, fnSource } from "./app-source.mjs";

const f = new Function(`
    ${fnSource("numberFieldText")}
    ${fnSource("numberFieldValue")}
    return { numberFieldText, numberFieldValue };
`)();

const hexInput = (value = "") => ({ dataset: { hex: "1" }, value });
const decInput = (value = "") => ({ dataset: {}, value });

test("a hex field shows 24 as 0x18, and a decimal field as 24", () => {
    assert.equal(f.numberFieldText(hexInput(), 24), "0x18");
    assert.equal(f.numberFieldText(hexInput(), 5), "0x05");
    assert.equal(f.numberFieldText(decInput(), 24), "24");
});

test("a hex field reads 0x18, 0X18 and 18 alike, and holds no value until it is a whole hex number", () => {
    for (const typed of ["0x18", "0X18", "18", " 18 "]) assert.equal(f.numberFieldValue(hexInput(typed)), 24, typed);
    for (const typed of ["0x", "", "18zz", "0x1g", "zz"]) assert.ok(Number.isNaN(f.numberFieldValue(hexInput(typed))), typed);
    assert.equal(f.numberFieldValue(decInput("18")), 18);
});

test("the live patch formats a number field the way its renderer does", () => {
    const at = src.indexOf("function updateModuleControls(");
    const patch = src.slice(at, src.indexOf('case "bool":', at));
    assert.match(patch, /numberFieldValue\(input\) !== Number\(ctrl\.value\)/);
    assert.match(patch, /input\.value = numberFieldText\(input, ctrl\.value \?\? 0\)/);
});

// A queued write superseded by a keystroke that leaves no number must not reach the device: typing "0" then "0x" would otherwise write 0 to a codec address.
test("an edit that leaves no number cancels the queued write, and blur sends nothing for a malformed field", () => {
    const at = src.indexOf("if (ctrl.numberField && isNumericType) {");
    const field = src.slice(at, src.indexOf("return row;", at));
    assert.match(field, /if \(Number\.isNaN\(v\)\) \{ cancelSend\(key\); return; \}/);
    assert.match(field, /if \(input\.value\.trim\(\)\) return;/);
});
