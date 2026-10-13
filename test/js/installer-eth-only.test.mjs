// Installer eth-only contract — the rule the web installer uses to decide whether a firmware is
// Ethernet-only must agree with mooninstaller/firmwares.json's `eth_only` flag for EVERY firmware.
//
// Why this matters: an Ethernet-only build (esp32-eth, esp32p4-eth) has WiFi compiled out, so it has
// no WIFI_SETTINGS Improv RPC. If the installer mis-classifies it as WiFi-capable and the device has
// no Ethernet link, it sends WIFI_SETTINGS and the firmware replies UNKNOWN_RPC_COMMAND — the exact
// error users hit. index.html derives eth-only from the firmware key with `/-eth$/` (matching the
// existing `-eth*` naming convention used by install-picker's isCompatible) and passes it to the
// orchestrator as `ethOnly`, which then skips WiFi provisioning and tells the user to connect
// Ethernet. This test pins that the name-based rule and the authoritative `eth_only` flag can't
// drift — e.g. a future firmware added to firmwares.json with a name the rule misreads would fail
// here, before it ships a broken installer flow.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const firmwares = JSON.parse(
    readFileSync(join(ROOT, "mooninstaller", "firmwares.json"), "utf8")
).firmwares;

// The single rule the installer applies (index.html onInstall): a firmware key ending in `-eth` is
// Ethernet-only; `-eth-wifi` (the P4 co-processor WiFi build) is NOT, since it doesn't end in `-eth`.
const isEthOnlyByName = (name) => /-eth$/.test(name);

// The rule belongs to the ESP32 flashing path; a Teensy is Ethernet-only by nature and gets its own writer.
test("the installer's /-eth$/ rule matches firmwares.json eth_only for every ESP32 firmware", () => {
    const esp32 = firmwares.filter((f) => f.platform === "esp32");
    assert.ok(esp32.length > 0, "firmwares.json has no ESP32 firmwares");
    for (const f of esp32) {
        assert.equal(
            isEthOnlyByName(f.name),
            !!f.eth_only,
            `firmware "${f.name}": name-rule says eth-only=${isEthOnlyByName(f.name)} but ` +
            `firmwares.json eth_only=${!!f.eth_only}. The installer would make the wrong ` +
            `WiFi-provisioning decision (eth-only builds have no WIFI_SETTINGS RPC → UNKNOWN_RPC). ` +
            `Reconcile the /-eth$/ rule in mooninstaller/index.html with this firmware's name/flag.`
        );
    }
});

test("at least one eth-only and one WiFi-capable firmware ship (the rule distinguishes them)", () => {
    const shipping = firmwares.filter((f) => f.ships);
    assert.ok(shipping.some((f) => f.eth_only), "no shipping eth-only firmware — eth path untested in practice");
    assert.ok(shipping.some((f) => !f.eth_only), "no shipping WiFi-capable firmware");
});
