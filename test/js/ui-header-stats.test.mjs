// The header carries the uptime for everyone, and the frame rate and the memory badges from expert
// mode up: a user watching a show has no use for them, and on a phone they crowded the device name
// into three lines.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const src = readFileSync(new URL("../../src/ui/app.js", import.meta.url), "utf8");

// The header line's build: from the stats element to the text it sets.
const start = src.indexOf('getElementById("sys-stats")');
const block = src.slice(start, src.indexOf("setText(statsEl", start));

test("the header's rate and memory show from expert mode up, the uptime always", () => {
    assert.match(block, /const tuning = uiMode\(\) >= 1;/);
    for (const badge of ["🕒", "🧠", "🧱"]) {
        const at = block.indexOf(`"${badge} "`);
        assert.notEqual(at, -1, `${badge} not found in the header line`);
        const guard = block.lastIndexOf("if (", at);
        assert.match(block.slice(guard, at), /^if \(tuning &&/, `${badge} shows outside expert mode`);
    }
    assert.match(block, /if \(uptimeCtrl\) parts\.push/);
});
