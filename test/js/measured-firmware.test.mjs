// The firmware a board names by what it tells about itself, for an install with no specific device chosen
// (measuredFirmware in src/ui/install-picker-devices.js). Run: `node --test test/js/measured-firmware.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { measuredFirmware, factsFromEsptool, factsFromDevice } from "../../src/ui/install-picker-devices.js";

const fw = (family, description, features, flashSize) => measuredFirmware(family, factsFromEsptool({ description, features, flashSize })).firmware;

test("a classic ESP32 takes the full build above 4 MB and the 4 MB tier at 4 MB", () => {
    assert.equal(fw("ESP32", "ESP32-D0WD-V3 (revision v3.0)", ["Wi-Fi", "BT"], "16MB"), "esp32");
    assert.equal(fw("ESP32", "ESP32-PICO-V3-02 (revision v3.1)", ["Embedded PSRAM 2MB"], "8MB"), "esp32");
    assert.equal(fw("ESP32", "ESP32-D0WD-V3 (revision v3.1)", [], "4MB"), "esp32-lite");
});

test("unread flash names no firmware where the size decides, so a chosen device's own firmware stands", () => {
    const classic = measuredFirmware("ESP32", factsFromEsptool({ description: "ESP32-D0WD-V3 (revision v3.1)", features: [], flashSize: undefined }));
    assert.equal(classic.firmware, null);
    assert.match(classic.note, /flash size unread/);
    assert.equal(firmwareFits("esp32", classic), true);
    const s3 = measuredFirmware("ESP32-S3", factsFromEsptool({ description: "ESP32-S3 (revision v0.2)", features: ["Embedded PSRAM 16MB (AP_1v8)"], flashSize: undefined }));
    assert.equal(s3.firmware, null);
    assert.equal(firmwareFits("esp32s3-opi-oflash", s3), true);
});

test("a revision 0 or 1 classic with PSRAM in the package takes the cache-workaround build", () => {
    assert.equal(fw("ESP32", "ESP32-D0WDR2 (revision v1.0)", ["Embedded PSRAM 2MB"], "4MB"), "esp32-rev1-lite");
    assert.equal(fw("ESP32", "ESP32-D0WDQ6 (revision v1.0)", ["Wi-Fi"], "4MB"), "esp32-lite");   // PSRAM outside the package reads as none
});

test("an S3 is named by its in-package PSRAM and its flash", () => {
    assert.equal(fw("ESP32-S3", "ESP32-S3 (QFN56) (revision v0.2)", ["Embedded PSRAM 8MB (AP_3v3)"], "16MB"), "esp32s3-opi");
    assert.equal(fw("ESP32-S3", "ESP32-S3 (QFN56) (revision v0.2)", ["Embedded PSRAM 2MB (AP_3v3)"], "8MB"), "esp32s3-qspi");
    assert.equal(fw("ESP32-S3", "ESP32-S3 (QFN56) (revision v0.2)", ["Embedded PSRAM 2MB (AP_3v3)"], "4MB"), "esp32s3-qspi-lite");
    assert.equal(fw("ESP32-S3", "ESP32-S3 (revision v0.2)", ["Embedded PSRAM 16MB (AP_1v8)"], "32MB"), "esp32s3-opi-oflash");
});

test("an S3 whose PSRAM sits outside the package names no firmware, so its device is picked", () => {
    const r = measuredFirmware("ESP32-S3", factsFromEsptool({ description: "ESP32-S3 (revision v0.2)", features: ["Wi-Fi"], flashSize: "8MB" }));
    assert.equal(r.firmware, null);
    assert.match(r.note, /pick the device/);
});

test("the P4 is named by its revision line, and the single-firmware chips by their chip", () => {
    assert.equal(fw("ESP32-P4", "ESP32-P4 (revision v1.3)", [], "16MB"), "esp32p4rev1");
    assert.equal(fw("ESP32-P4", "ESP32-P4 (revision v3.0)", [], "16MB"), "esp32p4rev3");
    assert.equal(fw("ESP32-C3", "ESP32-C3 (QFN32) (revision v0.4)", ["Embedded Flash 4MB (XMC)"], "4MB"), "esp32c3");
});

import { firmwareFits } from "../../src/ui/install-picker-devices.js";
const board = (family, description, features, flashSize) => measuredFirmware(family, factsFromEsptool({ description, features, flashSize }));

test("a chosen device's firmware fits when the board names it, or when it runs on every classic board", () => {
    const classic4 = board("ESP32", "ESP32-D0WD-V3 (revision v3.1)", [], "4MB");   // names esp32-lite
    assert.equal(firmwareFits("esp32-lite", classic4), true);
    assert.equal(firmwareFits("esp32-ethonly", classic4), true);
    assert.equal(firmwareFits("esp32-rev1-lite", classic4), true);
});

test("a chosen device's firmware that differs from the board in a fact does not fit", () => {
    assert.equal(firmwareFits("esp32", board("ESP32", "ESP32-D0WD-V3 (revision v3.1)", [], "4MB")), false);                // 8 MB and up
    assert.equal(firmwareFits("esp32s3-opi", board("ESP32-S3", "ESP32-S3 (revision v0.2)", ["Embedded PSRAM 2MB"], "8MB")), false);   // octal PSRAM
    assert.equal(firmwareFits("esp32p4rev3", board("ESP32-P4", "ESP32-P4 (revision v1.3)", [], "16MB")), false);  // the other revision line
});


test("a running device's own facts name its firmware, PSRAM outside the package included", () => {
    const wrover = factsFromDevice({ revision: "v1.0", package: "D0WDQ6", flashMB: 4, psramMB: 4, psramMode: "quad" });
    assert.equal(measuredFirmware("ESP32", wrover).firmware, "esp32-rev1-lite");
    assert.equal(measuredFirmware("ESP32", factsFromDevice({ revision: "v3.0", flashMB: 16, psramMB: 0, psramMode: "" })).firmware, "esp32");
    assert.equal(measuredFirmware("ESP32-S3", factsFromDevice({ revision: "v0.2", flashMB: 8, psramMB: 8, psramMode: "octal" })).firmware, "esp32s3-opi");
});

import { devicesForBoard } from "../../src/ui/install-picker-devices.js";
const catalog = [
    { name: "DevKitC", chip: "ESP32-S3", firmwares: ["esp32s3-qspi", "esp32s3-opi", "esp32s3-opi-oflash"] },
    { name: "Zero", chip: "ESP32-S3", firmwares: ["esp32s3-qspi-lite"] },
    { name: "Matrix", chip: "ESP32-S3", firmwares: ["esp32s3-opi-oflash"] },
    { name: "Gateway", chip: "ESP32", firmwares: ["esp32-lite"] },
];
const names = (list) => list.map((d) => d.name);

test("a detected board lists only the devices with a firmware its measurement fits", () => {
    const n16r8 = factsFromEsptool({ description: "ESP32-S3 (revision v0.2)", features: ["Embedded PSRAM 8MB (AP_3v3)"], flashSize: "16MB" });
    assert.deepEqual(names(devicesForBoard(catalog, "ESP32-S3", n16r8)), ["DevKitC"]);
});

test("a board whose facts name no firmware lists its chip's whole catalog", () => {
    const unread = factsFromEsptool({ description: "ESP32-S3 (revision v0.2)", features: ["Embedded PSRAM 8MB (AP_3v3)"], flashSize: undefined });
    assert.deepEqual(names(devicesForBoard(catalog, "ESP32-S3", unread)), ["DevKitC", "Zero", "Matrix"]);
    assert.deepEqual(names(devicesForBoard(catalog, "ESP32-S3", null)), ["DevKitC", "Zero", "Matrix"]);
});

test("a measurement no device fits keeps its chip's whole catalog, so the user still picks", () => {
    const n4r2 = factsFromEsptool({ description: "ESP32-S3 (revision v0.2)", features: ["Embedded PSRAM 2MB (AP_3v3)"], flashSize: "8MB" });
    const onlyOctal = catalog.filter((d) => d.name !== "DevKitC" && d.name !== "Zero");
    assert.deepEqual(names(devicesForBoard(onlyOctal, "ESP32-S3", n4r2)), ["Matrix"]);
});

test("in-package PSRAM of unstated size counts, so a revision 1 classic still takes the cache-workaround build", () => {
    assert.equal(fw("ESP32", "ESP32-D0WDR2 (revision v1.0)", ["Embedded PSRAM"], "4MB"), "esp32-rev1-lite");
});
