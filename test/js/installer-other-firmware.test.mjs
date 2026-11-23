// Other projects' firmwares on the installer page: other-firmwares.json beside index.html lists them, each either an ESP Web Tools
// manifest inline (factory images at offset 0, one per chip family) or a link to the project's own manifest. The dropdown shows the
// entries with a build for the detected chip and names each one's version, the install takes the connected chip's build before
// anything is erased, and after the flash it tries Improv as ESP Web Tools does, skipping MoonLight's own boot-log steps.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { chipFamily, patchBootloaderFlashSize } from "../../mooninstaller/manifest.js";
import { releaseManifests, otherFirmwareOptions, otherFirmwareVersion, resolveOtherFirmwares } from "../../mooninstaller/other-firmwares.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const installer = join(ROOT, "mooninstaller");
const entries = JSON.parse(readFileSync(join(installer, "other-firmwares.json"), "utf8"));
// The orchestrator imports esptool-js from a CDN, so Node reads it as text for the order of its steps.
const orchestrator = readFileSync(join(installer, "install-orchestrator.js"), "utf8");

test("each other firmware is its project's manifest, or inline factory images at offset 0, one per chip family", () => {
    assert.ok(entries.length > 0, "the installer page offers another project's firmware");
    for (const entry of entries) {
        assert.ok(entry.name && /^https:\/\//.test(entry.home), `${entry.name}: a name and a page about it`);
        if (entry.manifest) {
            assert.match(entry.manifest, /^https:\/\/.+\.json$/, `${entry.name}: the project's own ESP Web Tools manifest`);
            assert.equal(entry.builds, undefined, `${entry.name}: its builds come from its manifest, one home`);
            continue;
        }
        if (entry.assets) {
            assert.match(entry.download, /^https:\/\//, `${entry.name}: a CORS mirror for its release assets`);
            assert.match(entry.boot, /^https:\/\/.+\/$/, `${entry.name}: the boot files of the project's own installer`);
            for (const rule of entry.assets) {
                assert.ok(entry.bootSets[rule.boot], `${entry.name}: ${rule.match} names boot set ${rule.boot}, which is not there`);
                assert.equal(chipFamily(rule.chipFamily), rule.chipFamily, `${entry.name}: ${rule.match} names no chip family the install compares`);
            }
            continue;
        }
        assert.ok(entry.release && /^[\w.-]+\/[\w.-]+$/.test(entry.release.repo), `${entry.name}: the GitHub release that names its version`);
        const families = entry.builds.map((build) => build.chipFamily);
        assert.equal(new Set(families).size, families.length, `${entry.name}: one build per chip family, so the install's pick is unambiguous`);
        for (const build of entry.builds) {
            assert.equal(chipFamily(build.chipFamily), build.chipFamily, `${entry.name}: ${build.chipFamily} is not a family the install compares`);
            assert.equal(build.parts.length, 1, `${entry.name} ${build.chipFamily}: one factory image`);
            assert.equal(build.parts[0].offset, 0, `${entry.name} ${build.chipFamily}: a factory image is written at offset 0`);
            assert.match(build.parts[0].path, /^https:\/\/.+\.factory\.bin$/, `${entry.name} ${build.chipFamily}: the project's own factory image`);
        }
    }
});

test("the dropdown offers what fits the detected board, and before a Detect lists every entry with its chips, none installable", () => {
    const catalog = [
        { name: "A", home: "h", builds: [{ chipFamily: "ESP32" }, { chipFamily: "ESP32-S3" }] },
        { name: "B", home: "h", builds: [{ chipFamily: "ESP32-P4" }] },
        { name: "C", home: "h", builds: [] },   // a project manifest that failed to load
        { ...wled, name: "W", home: "h", releaseData: { tag_name: "v16.0.1", assets: [asset("WLED_16.0.1_ESP32.bin")] } },
        { ...wled, name: "X", home: "h", error: "did not load (GitHub answered 403): reload the page later" },   // a rate-limited release
    ];
    const names = (board) => otherFirmwareOptions(catalog, board).map((o) => o.label);
    assert.deepEqual(names(null), ["A · for ESP32, ESP32-S3: Detect the board first", "B · for ESP32-P4: Detect the board first",
        "C · for its chips: Detect the board first", "W · for ESP32, ESP32-C3, ESP32-S3: Detect the board first",
        "X · did not load (GitHub answered 403): reload the page later"]);
    assert.ok(otherFirmwareOptions(catalog, null).every((o) => !o.source), "before a Detect every entry is a label, none installable");
    assert.deepEqual(names({ family: "ESP32-S3", flashMB: 8, psramMode: "octal" }), ["A", "X · did not load (GitHub answered 403): reload the page later"]);
    assert.deepEqual(names({ family: "ESP32", flashMB: 4, psramMode: "" }), ["A", "W v16.0.1 · ESP32",
        "X · did not load (GitHub answered 403): reload the page later"], "an entry that did not load stays listed with its reason");
    assert.equal(otherFirmwareOptions(catalog, { family: "ESP32", flashMB: 4, psramMode: "" })[1].accessPoint, "WLED-AP",
        "a build names the access point its firmware opens, for the done screen");
    assert.deepEqual(names({ family: "ESP32-H2", flashMB: 4, psramMode: "" }).filter((n) => !n.startsWith("X ")), [], "a chip no entry builds for lists none");
});

const wled = entries.find((e) => e.name === "WLED");
const wledmm = entries.find((e) => e.name === "WLED-MM");
const asset = (name, repo = "wled/WLED", tag = "v16.0.1") => ({ name, browser_download_url: `https://github.com/${repo}/releases/download/${tag}/${name}` });
const WLED_RELEASE = { tag_name: "v16.0.1", assets: ["WLED_16.0.1_ESP32.bin", "WLED_16.0.1_ESP32_Ethernet.bin", "WLED_16.0.1_ESP32_HUB75.bin",
    "WLED_16.0.1_ESP32-S3_8MB_opi.bin", "WLED_16.0.1_ESP32-S3_8MB_qspi.bin", "WLED_16.0.1_ESP32-S3_16MB_opi.bin", "WLED_16.0.1_ESP32-S3_HD-WF2.bin",
    "WLED_16.0.1_ESP32-S3_Waveshare_HUB75.bin", "WLED_16.0.1_ESP32-S3_WROOM-2.bin", "WLED_16.0.1_ESP02.bin.gz"].map((n) => asset(n)) };
const shorts = (entry, release, board) => releaseManifests(entry, release, board).map((m) => m.short);

test("a WLED release offers the builds for the board's chip, flash size and PSRAM mode, and only the ones its installer knows", () => {
    assert.deepEqual(shorts(wled, WLED_RELEASE, { family: "ESP32", flashMB: 16, psramMode: "" }), ["ESP32", "ESP32_Ethernet", "ESP32_HUB75"]);
    assert.deepEqual(shorts(wled, WLED_RELEASE, { family: "ESP32-S3", flashMB: 16, psramMode: "octal" }),
        ["ESP32-S3_8MB_opi", "ESP32-S3_16MB_opi", "ESP32-S3_HD-WF2"], "quad PSRAM, the 32 MB Waveshare and WROOM-2, which WLED's installer does not offer, stay out");
    assert.deepEqual(shorts(wled, WLED_RELEASE, { family: "ESP32-S3", flashMB: 8, psramMode: "quad" }), ["ESP32-S3_8MB_qspi", "ESP32-S3_HD-WF2"]);
});

test("a WLED build gets its installer's bootloader and partition table for the board's flash size, the bootloader re-flagged when built for another", () => {
    const [plain] = releaseManifests(wled, { tag_name: "v16.0.1", assets: [asset("WLED_16.0.1_ESP32.bin")] }, { family: "ESP32", flashMB: 16, psramMode: "" });
    assert.deepEqual(plain.manifest.builds[0].parts, [
        { path: "https://install.wled.me/bin/boot/bootloaders/esp32/bootloader_esp32_8m.bin", offset: 4096, flashSize: "16MB" },
        { path: "https://install.wled.me/bin/boot/partitions/partitions_esp32_16m.bin", offset: 0x8000 },
        { path: "https://download.wled.me/wled/WLED/releases/download/v16.0.1/WLED_16.0.1_ESP32.bin", offset: 0x10000 },
    ]);
    const [native] = releaseManifests(wled, { tag_name: "v16.0.1", assets: [asset("WLED_16.0.1_ESP32-S3_8MB_opi.bin")] }, { family: "ESP32-S3", flashMB: 8, psramMode: "octal" });
    assert.equal(native.manifest.builds[0].parts[0].flashSize, undefined, "an 8 MB build on the 8 MB bootloader is written as it is");
});

test("a board whose flash size did not read gets the smallest partition table, as WLED's installer defaults to 4 MB", () => {
    const [unread] = releaseManifests(wled, { tag_name: "v16.0.1", assets: [asset("WLED_16.0.1_ESP32.bin")] }, { family: "ESP32", flashMB: 0, psramMode: "" });
    const set = wled.bootSets[wled.assets.find((r) => new RegExp(r.match).test("WLED_16.0.1_ESP32.bin")).boot];
    const smallest = Object.keys(set.partitions).sort((a, b) => parseInt(a, 10) - parseInt(b, 10))[0];
    assert.equal(unread.manifest.builds[0].parts[1].path, wled.boot + set.partitions[smallest]);
    assert.equal(smallest, "4MB");
});

test("a WLED-MM build gets its installer's one boot set per chip, and its flash size must fit the board", () => {
    const release = { tag_name: "v14.7.1", assets: ["WLEDMM_14.7.1_esp32_4MB_V4_M.bin", "WLEDMM_14.7.1_esp32_16MB_V4_S.bin", "WLEDMM_14.7.1_esp32s2_4MB_M.bin",
        "WLEDMM_14.7.1_esp32S3_8MB_PSRAM_M.bin", "WLEDMM_14.7.1_esp8266_4MB_M.bin"].map((n) => asset(n, "MoonModules/WLED-MM", "v14.7.1")) };
    assert.deepEqual(shorts(wledmm, release, { family: "ESP32", flashMB: 4, psramMode: "" }), ["esp32_4MB_V4_M"], "the 16 MB build waits for a 16 MB board");
    const [s3] = releaseManifests(wledmm, release, { family: "ESP32-S3", flashMB: 8, psramMode: "octal" });
    assert.deepEqual(s3.manifest.builds[0].parts.map((p) => p.offset), [0, 0x8000, 0x10000]);
    assert.equal(s3.manifest.version, "v14.7.1");
});

test("re-flagging a bootloader sets the flash-size nibble and recomputes the appended digest over the header", async () => {
    // An image with no segments and the append-digest flag set: header, extended header, padding, checksum, then the digest.
    const image = new Uint8Array(24 + 7 + 1 + 32);
    image[0] = 0xe9; image[3] = 0x3f; image[23] = 1;
    const patched = await patchBootloaderFlashSize(image.buffer, "16MB");
    assert.equal(patched[3], 0x4f, "the size nibble is 16 MB, the frequency nibble kept");
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", patched.slice(0, 32)));
    assert.deepEqual(patched.slice(32), digest);
});

test("the label names a manifest's version, a dated build's included, or a release's tag", () => {
    assert.equal(otherFirmwareVersion({ manifest: "https://x/manifest.json" }, { version: "26.9.2" }), "26.9.2");
    assert.equal(otherFirmwareVersion({ manifest: "https://x/manifest.json" }, { version: "14.5.1-dev build 2026-11-22 e77b82c8" }),
        "14.5.1-dev build 2026-11-22 e77b82c8");
    assert.equal(otherFirmwareVersion({ release: { repo: "arendst/Tasmota" } }, { tag_name: "v16.0.0" }), "v16.0.0");
});

test("the install compares families, so a revision or an esptool-js prefix still finds the build", () => {
    assert.equal(chipFamily("ESP32-P4 (revision v1.0)"), "ESP32-P4");
    assert.equal(chipFamily("unknown ESP32-P4 (revision v1.3)"), "ESP32-P4");
    assert.equal(chipFamily("ESP32-S3 (QFN56) (revision v0.2)"), "ESP32-S3");
    assert.equal(chipFamily("ESP32-D0WD-V3"), "ESP32");
});

test("the build is picked before anything is fetched, and another firmware goes from the reset straight to Improv", () => {
    const at = (needle) => {
        const i = orchestrator.indexOf(needle, orchestrator.indexOf("async start("));
        assert.ok(i > 0, `start() lost: ${needle}`);
        return i;
    };
    const pick = at("builds.find(b => b.chipFamily === chipFamily(chipName))");
    assert.ok(at("end of detect()-reuse else branch") < pick && pick < at('trackProgress("fetch-firmware")'),
        "the connected chip's build is chosen before anything is fetched or erased");
    assert.ok(at('trackProgress("connect-improv")') < at("otherFirmware ? { ip: \"\", mdns: \"\" } : await readBootIp("),
        "MoonLight's MM_IP boot line is skipped for another project's firmware");
    assert.ok(at("if (!deviceUrl && !otherFirmware)") > at("deviceUrl = improvClient.nextUrl"),
        "another firmware that joins and names no address (ESPHome's first-use firmware) is a join, not an error");
    assert.ok(at("Date.now() + (otherFirmware ? 10000 : 0)") < at("await initializeImprov()"),
        "another firmware's Improv is asked for until it answers, since reopening the port can reset the board into a boot that takes a second");
});

test("an entry whose manifest or release answers an HTTP error is listed with the status, not as a chipless entry", async () => {
    const real = globalThis.fetch;
    globalThis.fetch = async () => ({ ok: false, status: 404, json: async () => ({ message: "Not Found" }) });
    try {
        const [manifest, release] = await resolveOtherFirmwares([{ name: "M", manifest: "https://example.org/latest/manifest.json" },
                                                                 { name: "R", release: { repo: "o/r" } }]);
        assert.match(manifest.error, /example\.org answered 404/);
        assert.match(release.error, /GitHub answered 404/);
        assert.deepEqual(manifest.builds, []);
    } finally {
        globalThis.fetch = real;
    }
});
