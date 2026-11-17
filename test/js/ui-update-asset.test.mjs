// Which release image updates a device: firmware-<name>-v<version>.bin for a board and a host alike, with the digest GitHub names; a device its
// host or its user updates gets a null address. Runs in node, on the function's own source.
// Run: `node --test test/js/ui-update-asset.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { fnSource } from "./app-source.mjs";
import { isCompatible, parseFirmwaresFromAssets, chipKey, chipFamilyOf, firmwareChip, releaseAsset, firmwareAssetName, moonbaseAssetName, mirrorUrl, latestStable, releaseByTag }
    from "../../src/ui/install-picker.js";
import { isNewer, parse } from "../../src/ui/semver.js";

const { updateAsset, moonbaseAsset } = new Function("releaseAsset", "firmwareAssetName", "moonbaseAssetName", "chipKey",
    `${fnSource("updateAsset")}\n${fnSource("moonbaseAssetKeyFrom")}\n${fnSource("moonbaseAsset")}\nreturn { updateAsset, moonbaseAsset };`)(
    releaseAsset, firmwareAssetName, moonbaseAssetName, chipKey);

const release = {
    assets: [
        { name: "firmware-esp32s3-n16r8-v6.2.0.bin", browser_download_url: "https://x/fw.bin", digest: "sha256:aa" },
        { name: "firmware-host-linux-arm64-v6.2.0.bin", browser_download_url: "https://x/pi.bin", digest: "sha256:bb" },
    ],
};

test("a board and a host each install their own image by the same name", () => {
    assert.deepEqual(updateAsset(release, { firmware: "esp32s3-n16r8", update: "self" }, "6.2.0"), { url: "https://x/fw.bin", sha256: "aa" });
    assert.deepEqual(updateAsset(release, { firmware: "host-linux-arm64", update: "self" }, "6.2.0"), { url: "https://x/pi.bin", sha256: "bb" });
});

test("a board on firmware older than the update field installs as before", () => {
    assert.equal(updateAsset(release, { firmware: "esp32s3-n16r8" }, "6.2.0").url, "https://x/fw.bin");
});

test("a container, a program in a read-only folder and a host older than its firmware name get a null address", () => {
    assert.equal(updateAsset(release, { firmware: "host-linux-arm64", update: "container" }, "6.2.0").url, null);
    assert.equal(updateAsset(release, { firmware: "host-linux-arm64", update: "manual" }, "6.2.0").url, null);
    assert.equal(updateAsset(release, { firmware: null }, "6.2.0").url, null);
});

test("a host's own program is offered to that host only, a board the web installer flashes taking its own firmware", () => {
    assert.equal(isCompatible("host-linux-arm64", "host-linux-arm64"), true);
    assert.equal(isCompatible("host-linux-x64", "host-linux-arm64"), false);
    assert.equal(isCompatible(null, "host-linux-arm64"), false);
    assert.equal(isCompatible("esp32", "host-linux-arm64"), false);
});

test("a release lists a host's own program beside its download of the same name, one to install and one to save", () => {
    const parsed = parseFirmwaresFromAssets([
        { name: "firmware-host-linux-arm64-v6.2.0.bin", browser_download_url: "https://x/pi.bin", digest: "sha256:bb" },
        { name: "moonlight_6.2.0_arm64.deb", browser_download_url: "https://x/pi.deb" },
    ], "v6.2.0");
    const program = parsed.find(f => f.firmware === "host-linux-arm64" && !f.isHost);
    const download = parsed.find(f => f.firmware === "host-linux-arm64" && f.isHost);
    assert.equal(program?.binaryUrl, "https://x/pi.bin");
    assert.equal(program?.sha256, "bb");   // the digest the Firmware card posts with the address, as the map does
    assert.equal(download?.binaryUrl, "https://x/pi.deb");
});

test("a MoonBase behind its app takes the image its app's release carries for the chip the device reports", () => {
    const rel = { assets: [{ name: "shared-moonbase-esp32p4.bin", browser_download_url: "https://x/mb.bin", digest: "sha256:cc" }] };
    assert.deepEqual(moonbaseAsset(rel, "ESP32-P4"), { url: "https://x/mb.bin", sha256: "cc" });
    assert.equal(moonbaseAsset(rel, "ESP32-S3").url, null);   // that release carries none for this chip
    assert.equal(moonbaseAsset(rel, null).url, null);         // a device that names no chip is offered nothing
});

test("a release asset is fetched from the installer's mirror, which serves it cross-origin; any other address as given", () => {
    const gh = "https://github.com/MoonModules/MoonLight/releases/download/v7.0.0/firmware-esp32-v7.0.0.bin";
    assert.equal(mirrorUrl(gh, "https://moonmodules.org/MoonLight/install/"), "https://moonmodules.org/MoonLight/install/releases/v7.0.0/firmware-esp32-v7.0.0.bin");
    assert.equal(mirrorUrl(gh, "./"), "./releases/v7.0.0/firmware-esp32-v7.0.0.bin");
    assert.equal(mirrorUrl("https://example.com/x.bin", "./"), "https://example.com/x.bin");
});

test("one release list answers both channels: the newest stable release, and the moving latest by its tag", () => {
    const list = [{ tag_name: "latest", prerelease: true }, { tag_name: "v7.1.0", draft: true }, { tag_name: "v7.0.0" }, { tag_name: "v6.0.0" }];
    assert.equal(latestStable(list).tag_name, "v7.0.0");   // as GitHub's /releases/latest: neither a prerelease nor a draft
    assert.equal(releaseByTag(list, "latest").tag_name, "latest");
    assert.equal(releaseByTag(list, "v5.0.0"), null);
    assert.equal(latestStable(null), null);                // an offline device's empty list
});

// The one channel rule behind the status bar's badge, a node's popup and its mark on the map.
const { channelUpdate } = new Function("parse", "isNewer", "latestStable", "releaseByTag", "releaseAsset", "firmwareAssetName",
    `const MODE_DEVELOPER = 2;\n${fnSource("followsLatest")}\n${fnSource("channelUpdate")}\nreturn { channelUpdate };`)(
    parse, isNewer, latestStable, releaseByTag, releaseAsset, firmwareAssetName);
const img = (fw, v) => ({ name: `firmware-${fw}-v${v}.bin`, browser_download_url: `https://x/${v}.bin` });
const releases = [
    { tag_name: "latest", name: "7.2.0-dev.5", prerelease: true, assets: [img("esp32", "7.2.0-dev.5")] },
    { tag_name: "v7.1.0", assets: [img("esp32", "7.1.0")] },
];

test("a device on an official release is offered only a newer official release", () => {
    assert.equal(channelUpdate({ version: "7.0.0", firmware: "esp32" }, releases).tag, "v7.1.0");
    assert.equal(channelUpdate({ version: "7.1.0", firmware: "esp32" }, releases), null);   // latest is newer, but not its channel
});

test("a device on a -dev build, or in developer mode, is also offered a newer latest, an official release first", () => {
    assert.equal(channelUpdate({ version: "7.1.0-dev.9", firmware: "esp32" }, releases).tag, "v7.1.0");   // official first
    assert.equal(channelUpdate({ version: "7.2.0-dev.1", firmware: "esp32" }, releases).label, "latest (7.2.0-dev.5)");
    assert.equal(channelUpdate({ version: "7.1.0", firmware: "esp32", mode: 2 }, releases).label, "latest (7.2.0-dev.5)");
    assert.equal(channelUpdate({ version: "7.1.0", firmware: "esp32", mode: 1 }, releases), null);   // expert is no channel of its own
});

test("a release without the device's own image offers nothing", () => {
    assert.equal(channelUpdate({ version: "7.0.0", firmware: "esp32s3-n16r8" }, releases), null);
    assert.equal(channelUpdate({ version: "7.0.0", firmware: null }, releases), null);
});

test("a chip is a family as the catalog and esptool name it and a key as firmware and assets spell it, each turning into the other", () => {
    assert.equal(chipKey("ESP32-S3"), "esp32s3");
    assert.equal(chipFamilyOf("esp32s3"), "ESP32-S3");
    assert.equal(chipFamilyOf("esp32s31"), "ESP32-S31");          // its own family, as the build names it
    assert.equal(chipFamilyOf("esp32"), "ESP32");                  // the classic
    assert.equal(chipFamilyOf(firmwareChip("esp32p4rev1-eth")), "ESP32-P4");
    assert.equal(chipFamilyOf(firmwareChip("host-macos-arm64")), "host");   // not an ESP32: as given
});
