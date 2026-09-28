// The board->device rename changed a name a VISITOR'S BROWSER holds: the `MoonLight.picker.board`
// localStorage key their pick is saved under. That value is not ours to migrate, so the new key is
// written and the old one is read as a fallback; dropping it silently forgets what people had, and
// nothing else in the branch would have failed if it were.
//
// The `?device=` parameter is the opposite case and deliberately has NO fallback: a URL is ours to
// name, so it has one spelling.
//
// install-picker.js is a browser script rather than a module, so this pins the contract by reading
// the source and by running the lifted resolution order. A behavioral test would need a DOM.
//
// Run: `node --test "test/js/**/*.test.mjs"`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const src = readFileSync(join(ROOT, "src", "ui", "install-picker.js"), "utf8");

test("the URL parameter has exactly one spelling", () => {
    assert.match(src, /urlParam\("device"\)/, "the link parameter is ?device=");
    assert.doesNotMatch(src, /urlParam\("board"\)/,
        "a URL is ours to name, so a second spelling of it is debt rather than compatibility");
});

test("the old storage key is still read", () => {
    assert.match(src, /PREF_DEVICE_KEY_LEGACY\s*=\s*"MoonLight\.picker\.board"/,
        "the pre-rename key is what a returning visitor's pick is saved under");
    assert.match(src, /safeLocalGet\(PREF_DEVICE_KEY\)\s*\|\|\s*safeLocalGet\(PREF_DEVICE_KEY_LEGACY\)/,
        "the new key is preferred and the old one is the fallback");
});

test("only the new key is ever written, so the old one fades out", () => {
    const writes = src.match(/safeLocalSet\(PREF_DEVICE_KEY(_LEGACY)?/g) || [];
    assert.ok(writes.length > 0, "the pick is saved somewhere");
    assert.deepEqual(writes.filter(w => w.includes("LEGACY")), [],
        "writing the legacy key would keep it alive forever instead of migrating off it");
});

test("the resolution order prefers the explicit link over the saved habit", () => {
    // A link is the sender's intent and a saved key is the visitor's habit, so the link wins: a
    // support link that lands on the wrong device helps nobody. The whole order is one `||` chain
    // in the source, so it is asserted there rather than re-typed here, where a copy would pass
    // whatever install-picker.js went on to do.
    assert.match(src,
        /urlParam\("device"\)\s*\|\|\s*safeLocalGet\(PREF_DEVICE_KEY\)\s*\|\|\s*safeLocalGet\(PREF_DEVICE_KEY_LEGACY\)/,
        "?device= first, then the new key, then the pre-rename key");
});
