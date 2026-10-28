// Installer firmware-list logic — the pure helpers that decide which firmwares the
// release picker offers. These back the preview-installer enhancement that lets a
// locally-built firmware (e.g. a brand-new esp32s31 not yet in any GitHub release)
// appear in the dropdown and flash from local bins, without affecting production.
//
// Two helpers, both pure (data in, data out — no DOM, no fetch), imported from the
// real module so the test exercises shipping code, not a copy:
//
//   parseFirmwaresFromAssets(assets, tag) — derive {firmware, manifestUrl, binaryUrl}
//     from a GitHub release's assets[]; a firmware needs BOTH a manifest-*.json and a
//     firmware-*-v*.bin (a partial upload mid-publish must not appear).
//   mergeFirmwares(published, extras) — merge locally-staged extras into the published
//     list; a same-named local entry overrides the published one (flash the local
//     build), a local-only firmware is added, and an incomplete extra is ignored.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { parseFirmwaresFromAssets, mergeFirmwares } from "../../src/ui/install-picker.js";

// A GitHub-style asset entry: parse keys off `name`, the URL is `browser_download_url`.
const asset = (name) => ({ name, browser_download_url: `https://gh/${name}` });

test("parseFirmwaresFromAssets: a firmware needs BOTH a manifest and an app .bin", () => {
    const assets = [
        asset("manifest-esp32.json"),
        asset("firmware-esp32-v2.0.0.bin"),
        // bootloader / partition-table / ota-data are install fragments, not the app image
        asset("firmware-esp32-v2.0.0-bootloader.bin"),
        asset("firmware-esp32-v2.0.0-partition-table.bin"),
        // a manifest with NO matching app .bin (partial upload) — must be dropped
        asset("manifest-esp32s3-n16r8.json"),
    ];
    const fw = parseFirmwaresFromAssets(assets, "v2.0.0");
    const names = fw.map((f) => f.firmware);
    assert.deepEqual(names, ["esp32"], "only esp32 has both a manifest and an app .bin");
    assert.match(fw[0].manifestUrl, /manifest-esp32\.json$/);
    assert.match(fw[0].binaryUrl, /firmware-esp32-v2\.0\.0\.bin$/);
    assert.ok(!fw[0].binaryUrl.includes("bootloader"), "the app .bin, not a fragment");
});

test("parseFirmwaresFromAssets: no/empty assets yields no firmwares", () => {
    assert.deepEqual(parseFirmwaresFromAssets(null, "latest"), []);
    assert.deepEqual(parseFirmwaresFromAssets([], "latest"), []);
});

test("mergeFirmwares: a local-only firmware (not in the release) is added", () => {
    // The S31 case: GitHub's `latest` has no esp32s31 asset yet, but the preview
    // staged a local build. The local entry must surface so the picker offers it.
    const published = [
        { firmware: "esp32", manifestUrl: "gh-m", binaryUrl: "gh-b" },
    ];
    const extras = [
        { firmware: "esp32s31", manifestUrl: "./local-m", binaryUrl: "./local-b" },
    ];
    const merged = mergeFirmwares(published, extras);
    const names = merged.map((f) => f.firmware).sort();
    assert.deepEqual(names, ["esp32", "esp32s31"]);
    assert.equal(merged.find((f) => f.firmware === "esp32s31").manifestUrl, "./local-m");
});

test("mergeFirmwares: a same-named local entry overrides the published one", () => {
    // Preview overlays local bins on the real `latest` tag — flashing a board whose
    // firmware IS published must use the LOCAL build, not the GitHub one.
    const published = [{ firmware: "esp32", manifestUrl: "gh-m", binaryUrl: "gh-b" }];
    const extras = [{ firmware: "esp32", manifestUrl: "./local-m", binaryUrl: "./local-b" }];
    const merged = mergeFirmwares(published, extras);
    assert.equal(merged.length, 1, "no duplicate — the local entry replaces the published one");
    assert.equal(merged[0].manifestUrl, "./local-m");
    assert.equal(merged[0].binaryUrl, "./local-b");
});

test("mergeFirmwares: an incomplete extra (missing manifest or binary) is ignored", () => {
    const published = [{ firmware: "esp32", manifestUrl: "gh-m", binaryUrl: "gh-b" }];
    const extras = [
        { firmware: "broken1", manifestUrl: "./m" },            // no binaryUrl
        { firmware: "broken2", binaryUrl: "./b" },              // no manifestUrl
        { manifestUrl: "./m", binaryUrl: "./b" },               // no firmware name
        { firmware: "good", manifestUrl: "./m", binaryUrl: "./b" },
    ];
    const names = mergeFirmwares(published, extras).map((f) => f.firmware).sort();
    assert.deepEqual(names, ["esp32", "good"], "only the complete extra is merged in");
});

test("mergeFirmwares: no extras returns the published list unchanged (production path)", () => {
    // In production local-firmwares.json doesn't exist, so extras is null/[] — the
    // picker must behave exactly as before, driven purely by the published assets.
    const published = [{ firmware: "esp32", manifestUrl: "m", binaryUrl: "b" }];
    assert.strictEqual(mergeFirmwares(published, null), published);
    assert.strictEqual(mergeFirmwares(published, []), published);
});

test("parseFirmwaresFromAssets: MoonBase assets are never offered as an OTA image", () => {
    // shared-moonbase-<chip>.bin is the maintenance image a MoonBase manifest references, and
    // shared-ota-data-slot0.bin its boot-slot record. Offering either as a device's OTA target
    // would replace the app with an image that can only install, not run the show.
    const assets = [
        asset("manifest-esp32.json"),
        asset("firmware-esp32-v2.0.0.bin"),
        asset("shared-moonbase-esp32.bin"),
        asset("shared-ota-data-slot0.bin"),
    ];
    const fw = parseFirmwaresFromAssets(assets, "v2.0.0");
    assert.deepEqual(fw.map((f) => f.firmware), ["esp32"]);
    assert.match(fw[0].binaryUrl, /firmware-esp32-v2\.0\.0\.bin$/,
                 "the app image stays the binary, not a MoonBase asset");
});
