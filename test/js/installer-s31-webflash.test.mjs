// Web-flash guard contract — the installer special-cases chips esptool-js (the browser flasher)
// cannot handle, so a connect-flash failure shows "flash via the CLI" guidance instead of a bare
// timeout. The guard keys on a chip-family string (install.js `WEB_FLASH_UNSUPPORTED_CHIPS`); that
// string must match the `chip` a board in deviceModels.json actually reports, or it never fires.
//
// The set is EMPTY today: esptool-js 0.7.0 added the ESP32-S31 target and chip-id detection
// (GET_SECURITY_INFO), so every chip MoonLight ships is browser-flashable. The guard stays because
// a new chip lands here before esptool-js knows it, and these tests pin the MECHANISM rather than
// any one entry: whatever the set holds must be a chip a catalog board reports, and the guidance
// must stay scoped to the connect-flash stage.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

const installJs = readFileSync(join(ROOT, "mooninstaller", "install.js"), "utf8");
const boards = JSON.parse(
    readFileSync(join(ROOT, "mooninstaller", "deviceModels.json"), "utf8")
);

// The chip families install.js flags as not-browser-flashable. Parsed from the
// source so the test reads the real declaration (no hard-coded copy that could drift).
function unsupportedChips() {
    // `new Set()` (empty) and `new Set([...])` are both valid declarations: the set is expected to
    // be empty whenever esptool-js covers every chip we ship.
    const m = installJs.match(/WEB_FLASH_UNSUPPORTED_CHIPS\s*=\s*new Set\((?:\[([^\]]*)\])?\)/);
    assert.ok(m, "install.js must declare WEB_FLASH_UNSUPPORTED_CHIPS as a Set literal");
    return (m[1] || "").split(",").map((s) => s.trim().replace(/^["']|["']$/g, "")).filter(Boolean);
}

test("the S31 is browser-flashable: esptool-js 0.7.0 knows the chip", () => {
    assert.ok(
        !unsupportedChips().includes("ESP32-S31"),
        "esptool-js 0.7.0 ships the ESP32-S31 target and chip-id detection, so the S31 no longer " +
        "needs the CLI-only guard"
    );
    // The pin is what makes that true: an older esptool-js has no S31 target, and its magic table
    // would mis-identify the S31 as a classic ESP32 and flash the wrong stub.
    const orchestrator = readFileSync(join(ROOT, "mooninstaller", "install-orchestrator.js"), "utf8");
    const pin = orchestrator.match(/ESPTOOL_JS_VERSION\s*=\s*"(\d+)\.(\d+)\.(\d+)"/);
    assert.ok(pin, "install-orchestrator.js must pin ESPTOOL_JS_VERSION");
    const [maj, min] = [Number(pin[1]), Number(pin[2])];
    assert.ok(maj > 0 || min >= 7,
        `esptool-js must be >= 0.7.0 for S31 support, pinned ${pin[0]}`);
    // The import must load the SAME version the footer credit shows.
    assert.ok(orchestrator.includes(`esptool-js@${pin[1]}.${pin[2]}.${pin[3]}/bundle.js`),
        "the unpkg import must match ESPTOOL_JS_VERSION");
});

test("the guard only fires on the connect-flash stage", () => {
    // The message must be scoped to connect-flash — other stages keep their own
    // diagnostics. Pin that the guard condition ANDs the stage check.
    assert.match(
        installJs,
        /stage\s*===\s*["']connect-flash["']\s*&&\s*WEB_FLASH_UNSUPPORTED_CHIPS\.has/,
        "the unsupported-chip guidance must be gated on stage === 'connect-flash'"
    );
});

test("every flagged chip matches a real deviceModels.json board chip", () => {
    // A guard string with no board would be dead code; pin that each flagged chip
    // is one a catalog board actually reports, so getSelectedDeviceChip() can match it.
    const boardChips = new Set(boards.map((b) => b.chip));
    for (const chip of unsupportedChips()) {
        assert.ok(
            boardChips.has(chip),
            `WEB_FLASH_UNSUPPORTED_CHIPS has "${chip}" but no deviceModels.json board reports ` +
            `that chip — the guidance would never fire. Reconcile install.js with the catalog.`
        );
    }
});

test("the S31 board points at the esp32s31 firmware (so the CLI hint names the right build)", () => {
    const s31 = boards.find((b) => b.chip === "ESP32-S31");
    assert.ok(s31, "deviceModels.json must have an ESP32-S31 board");
    assert.ok(
        Array.isArray(s31.firmwares) && s31.firmwares.includes("esp32s31"),
        "the S31 board's firmwares must include esp32s31 (the build flash_esp32.py expects)"
    );
});
