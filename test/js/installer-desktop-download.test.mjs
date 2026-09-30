// The desktop-download half of the install picker. The same picker offers two very
// different things: an ESP32 firmware that gets FLASHED onto a board, and a desktop
// archive that gets DOWNLOADED to the computer viewing the page. These tests pin the
// rule that keeps those apart, because mixing them up means offering a .dmg as
// something to write to a microcontroller.
//
// Pure helpers imported from the real module, no DOM and no fetch.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { parseFirmwaresFromAssets, isCompatible } from "../../src/ui/install-picker.js";

const asset = (name) => ({ name, browser_download_url: `https://gh/${name}` });

// A release as the workflow actually publishes it: ESP32 firmware plus one archive per
// desktop platform.
const RELEASE = [
    asset("manifest-esp32s3-n16r8.json"),
    asset("firmware-esp32s3-n16r8-v3.0.0.bin"),
    asset("MoonLight-macos-arm64-v3.0.0.tar.gz"),
    asset("MoonLight-macos-arm64-v3.0.0.dmg"),
    asset("MoonLight-windows-x64-v3.0.0.zip"),
    asset("MoonLight-windows-x64-v3.0.0-setup.exe"),
    asset("MoonLight-linux-x64-v3.0.0.tar.gz"),
    asset("moonlight_3.0.0_amd64.deb"),
];

test("a desktop archive is offered for download even though it has no manifest", () => {
    // An ESP32 firmware needs a manifest to flash. A desktop archive has nothing to flash,
    // so requiring one would drop every desktop build from the list.
    const got = parseFirmwaresFromAssets(RELEASE, "v3.0.0");
    const mac = got.find(f => f.firmware === "desktop-macos-arm64");
    assert.ok(mac, "the macOS archive should be offered");
    assert.equal(mac.manifestUrl, null);
    assert.ok(mac.isDesktop);
});

test("each desktop platform is offered once, however many archives it ships", () => {
    const got = parseFirmwaresFromAssets(RELEASE, "v3.0.0").filter(f => f.isDesktop);
    assert.deepEqual(got.map(f => f.firmware).sort(),
        ["desktop-linux-x64", "desktop-macos-arm64", "desktop-windows-x64"]);
});

test("a platform shipping both an installer and a tarball offers the installer", () => {
    // macOS ships a .dmg to drag, Windows a setup.exe, Linux a .deb — each beside a plain
    // archive for scripting. The user clicking Download wants the one their OS knows how to
    // open. ALL THREE are asserted: this test covered only macOS and Linux while Windows was
    // shipping a setup.exe the picker could not even see, so the one platform with a broken
    // download was the one nothing checked.
    const got = parseFirmwaresFromAssets(RELEASE, "v3.0.0");
    assert.match(got.find(f => f.firmware === "desktop-macos-arm64").binaryUrl, /\.dmg$/);
    assert.match(got.find(f => f.firmware === "desktop-windows-x64").binaryUrl, /-setup\.exe$/);
    assert.match(got.find(f => f.firmware === "desktop-linux-x64").binaryUrl, /\.deb$/);
});

test("a windows installer whose name carries a suffix after the version is still matched", () => {
    // MoonLight-windows-x64-v3.0.0-setup.exe puts `-setup` AFTER the version, where every other
    // asset ends at its extension. A pattern anchored on "version then extension" silently
    // dropped it, and the release page had an installer the install page never offered.
    const got = parseFirmwaresFromAssets(RELEASE, "v3.0.0");
    const win = got.find(f => f.firmware === "desktop-windows-x64");
    assert.ok(win, "windows must be offered at all");
    assert.ok(win.assets.some(a => /-setup\.exe$/.test(a.name)),
              "the installer must appear among the platform's assets");
});

test("a version with dots does not break the platform match", () => {
    // The platform group is anchored on the "-v", not on "everything up to a dot": the
    // version's own dots would otherwise end the match and the archive would vanish.
    const got = parseFirmwaresFromAssets([asset("MoonLight-macos-arm64-v10.20.30-rc1.dmg")], "x");
    assert.equal(got.length, 1);
    assert.equal(got[0].firmware, "desktop-macos-arm64");
});

test("an ESP32 device is never offered a desktop archive to flash", () => {
    // The property this whole split exists for: a .dmg written to a board is a failed
    // flash at best.
    for (const own of ["esp32", "esp32s3-n16r8", "esp32-eth"]) {
        for (const key of ["desktop-macos-arm64", "desktop-windows-x64", "desktop-linux-x64"]) {
            assert.equal(isCompatible(own, key), false, `${own} must reject ${key}`);
        }
    }
});

test("a desktop is never offered an ESP32 firmware as its update", () => {
    // A desktop build has no ESP32 variant to name, so it reports "unknown". Whichever OS
    // the page runs on, no ESP32 image is ever a desktop upgrade.
    for (const key of ["esp32", "esp32s3-n16r8", "esp32p4", "esp32-eth"]) {
        assert.equal(isCompatible("unknown", key), false, `unknown must reject ${key}`);
    }
});

test("a desktop is offered exactly one archive: the one built for its own OS", () => {
    // Which OS this runs on depends on the host, so assert the SHAPE that must hold on any
    // of them: exactly one of the three desktop keys matches, never two, never none. That
    // is what stops a Mac being offered the Windows .zip.
    const keys = ["desktop-macos-arm64", "desktop-windows-x64", "desktop-linux-x64"];
    const matched = keys.filter(k => isCompatible("unknown", k));
    assert.equal(matched.length, 1, `expected exactly one match, got ${matched}`);
});

test("an ESP32 firmware and its ethernet variant remain mutually flashable", () => {
    // Pre-existing rule, re-pinned here because the desktop branch sits in front of it.
    assert.ok(isCompatible("esp32", "esp32-eth"));
    assert.ok(isCompatible("esp32-eth-wifi", "esp32"));
    assert.equal(isCompatible("esp32", "esp32s3-n16r8"), false);
});

// An OLDER release may ship only an archive where the newest ships an installer: v3.0.0 has a
// .tar.gz for macOS and no .dmg at all. The picker offers what exists, which is right — but a
// user who selected a stable release and received a .tar.gz had no way to see why, because the
// option said only "macOS arm64". The form now rides the label.
test("a release with only an archive still offers it, for every platform it has", () => {
    const OLD_RELEASE = [
        asset("MoonLight-macos-arm64-v3.0.0.tar.gz"),
        asset("MoonLight-windows-x64-v3.0.0.zip"),
    ];
    const got = parseFirmwaresFromAssets(OLD_RELEASE, "v3.0.0");
    assert.match(got.find(f => f.firmware === "desktop-macos-arm64").binaryUrl, /\.tar\.gz$/);
    assert.match(got.find(f => f.firmware === "desktop-windows-x64").binaryUrl, /\.zip$/);
});
