// Flash-baud contract for the web installer. A board opts into a non-default flash
// baud via deviceModels.json `flashBaud`: up for a verified-fast bridge (the S31's
// 921600, CLI-only), down for a flaky one (a LOLIN-style CH340 pinned to 460800). The
// installer's orchestrator resolves it per board (catalogFlashBaud) and hands it to
// esptool-js; the CLI (flash_esp32.py) resolves the same field. This test pins that the
// wiring stays honest so the two flash paths can't drift from the catalog data.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

const orchestrator = readFileSync(
    join(ROOT, "mooninstaller", "install-orchestrator.js"), "utf8"
);
const boards = JSON.parse(
    readFileSync(join(ROOT, "mooninstaller", "deviceModels.json"), "utf8")
);

// The standard esptool bauds a board may declare, from the catalog's schema, which check_devices.py validates against too.
const schema = JSON.parse(
    readFileSync(join(ROOT, "mooninstaller", "deviceModels.schema.json"), "utf8")
);
const VALID_BAUDS = new Set(schema.items.properties.flashBaud.enum);

test("the orchestrator drives esptool-js from the catalog flashBaud, not a hardcoded rate", () => {
    // The esploader baudrate must come from the resolved variable, so a board's
    // flashBaud actually reaches the flash. A literal `baudrate: 460800` would mean
    // the catalog value is ignored: pin against that regression.
    assert.match(
        orchestrator,
        /const\s+flashBaud\s*=\s*await\s+catalogFlashBaud\(/,
        "start() must resolve flashBaud via catalogFlashBaud(board)"
    );
    assert.match(
        orchestrator,
        /baudrate:\s*flashBaud\b/,
        "the ESPLoader must be constructed with baudrate: flashBaud (not a literal)"
    );
});

test("catalogFlashBaud falls back to 460800 on any lookup failure", () => {
    // The resolver must be best-effort: an unknown board / fetch error / parse error
    // returns the safe default rather than blocking or guessing a fast rate.
    const m = orchestrator.match(/const\s+DEFAULT_FLASH_BAUD\s*=\s*(\d+)/);
    assert.ok(m, "orchestrator must define DEFAULT_FLASH_BAUD");
    assert.equal(Number(m[1]), 460800, "the safe default flash baud must be 460800");
});

test("every board's flashBaud (when set) is a standard esptool rate", () => {
    // A typo'd baud would stringify wrong for esptool / be rejected by the driver.
    // The Python check_devices gate enforces this too; pinning it here keeps the JS
    // consumer honest against the same catalog.
    for (const b of boards) {
        if (b.flashBaud === undefined) continue;
        assert.ok(
            Number.isInteger(b.flashBaud) && VALID_BAUDS.has(b.flashBaud),
            `board "${b.name}" flashBaud must be one of ${[...VALID_BAUDS].sort((x, y) => x - y).join(", ")}, ` +
            `got ${JSON.stringify(b.flashBaud)}`
        );
    }
});
