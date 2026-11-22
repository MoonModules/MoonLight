// The catalog also lists the computers MoonLight runs on, and the web installer offers only what it can flash: an entry whose firmwares are all host builds is left out.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

import { flashable } from "../../src/ui/install-picker-devices.js";

const catalog = JSON.parse(readFileSync(new URL("../../mooninstaller/deviceModels.json", import.meta.url), "utf8"));

test("the installer lists boards and leaves out computers", () => {
    const names = flashable(catalog).map(d => d.name);
    assert.ok(names.includes("LOLIN D32"));
    for (const host of ["Raspberry Pi 5", "Generic Linux", "Generic macOS", "Docker container"]) {
        assert.ok(catalog.some(d => d.name === host), `${host} is in the catalog`);
        assert.ok(!names.includes(host), `${host} is not flashable`);
    }
});

test("an entry with a board firmware among its firmwares stays", () => {
    assert.equal(flashable([{ name: "a", firmwares: ["host-linux-x64"] }, { name: "b", firmwares: ["esp32", "host-linux-x64"] }]).length, 1);
});
