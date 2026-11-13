// Which release image updates a device: firmware-<name>-v<version>.bin for a board and a desktop alike, with the digest GitHub names; a device its
// host or its user updates gets a null address. Runs in node, on the function's own source.
// Run: `node --test test/js/ui-update-asset.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { fnSource } from "./app-source.mjs";
import { isCompatible, parseFirmwaresFromAssets } from "../../src/ui/install-picker.js";

const { updateAsset } = new Function(`${fnSource("updateAsset")}\nreturn { updateAsset };`)();

const release = {
    assets: [
        { name: "firmware-esp32s3-n16r8-v6.2.0.bin", browser_download_url: "https://x/fw.bin", digest: "sha256:aa" },
        { name: "firmware-desktop-linux-arm64-v6.2.0.bin", browser_download_url: "https://x/pi.bin", digest: "sha256:bb" },
    ],
};

test("a board and a desktop each install their own image by the same name", () => {
    assert.deepEqual(updateAsset(release, { firmware: "esp32s3-n16r8", update: "self" }, "6.2.0"), { url: "https://x/fw.bin", sha256: "aa" });
    assert.deepEqual(updateAsset(release, { firmware: "desktop-linux-arm64", update: "self" }, "6.2.0"), { url: "https://x/pi.bin", sha256: "bb" });
});

test("a board on firmware older than the update field installs as before", () => {
    assert.equal(updateAsset(release, { firmware: "esp32s3-n16r8" }, "6.2.0").url, "https://x/fw.bin");
});

test("a container, a program in a read-only folder and a desktop older than its firmware name get a null address", () => {
    assert.equal(updateAsset(release, { firmware: "desktop-linux-arm64", update: "host" }, "6.2.0").url, null);
    assert.equal(updateAsset(release, { firmware: "desktop-linux-arm64", update: "manual" }, "6.2.0").url, null);
    assert.equal(updateAsset(release, { firmware: null }, "6.2.0").url, null);
});

test("a desktop's own program is offered to that desktop only, a board the web installer flashes taking its own firmware", () => {
    assert.equal(isCompatible("desktop-linux-arm64", "desktop-linux-arm64"), true);
    assert.equal(isCompatible("desktop-linux-x64", "desktop-linux-arm64"), false);
    assert.equal(isCompatible(null, "desktop-linux-arm64"), false);
    assert.equal(isCompatible("esp32", "desktop-linux-arm64"), false);
});

test("a release lists a desktop's own program beside its download of the same name, one to install and one to save", () => {
    const parsed = parseFirmwaresFromAssets([
        { name: "firmware-desktop-linux-arm64-v6.2.0.bin", browser_download_url: "https://x/pi.bin", digest: "sha256:bb" },
        { name: "moonlight_6.2.0_arm64.deb", browser_download_url: "https://x/pi.deb" },
    ], "v6.2.0");
    const program = parsed.find(f => f.firmware === "desktop-linux-arm64" && !f.isDesktop);
    const download = parsed.find(f => f.firmware === "desktop-linux-arm64" && f.isDesktop);
    assert.equal(program?.binaryUrl, "https://x/pi.bin");
    assert.equal(program?.sha256, "bb");   // the digest the Firmware card posts with the address, as the map does
    assert.equal(download?.binaryUrl, "https://x/pi.deb");
});
