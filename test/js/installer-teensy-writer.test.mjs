// The installer's Teensy writer speaks PJRC's HalfKay protocol as teensy_loader_cli does: these tests pin the report layout,
// the blocks it skips, and the images it refuses, so a browser install writes what the loader would.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";

import {
    REPORT_BYTES,
    TEENSY41_CODE_BYTES,
    TEENSY41_SETTINGS_OFFSET,
    bootReport,
    eraseSettingsReports,
    halfkayReports,
    imageProblem,
    isTeensySerial,
    otherTeensyPorts,
    rebootedSerialPort,
} from "../../mooninstaller/teensy-writer.js";

// A flat image as the build writes it: the "FCFB" configuration block first, then code, with erased gaps.
function image(bytes) {
    const img = new Uint8Array(bytes).fill(0xFF);
    img.set([0x46, 0x43, 0x46, 0x42], 0);   // "FCFB"
    return img;
}

test("a report is a 64-byte header carrying the block's offset little-endian, then 1024 image bytes", () => {
    const img = image(0x30000 + 1024);
    img[0x30000] = 0x5A;
    const last = halfkayReports(img).at(-1);
    assert.equal(last.report.length, REPORT_BYTES);
    assert.deepEqual([...last.report.subarray(0, 3)], [0x00, 0x00, 0x03]);
    assert.ok(last.report.subarray(3, 64).every(b => b === 0));
    assert.equal(last.report[64], 0x5A);
});

test("block 0 is always written, since it erases the chip, and a later all-0xFF block is skipped", () => {
    const img = image(4 * 1024);
    img[2 * 1024 + 7] = 0x01;
    assert.deepEqual(halfkayReports(img).map(r => r.offset), [0, 2048]);
});

test("writing the reports onto erased flash reproduces the image byte for byte", () => {
    const img = image(10 * 1024 + 300);   // a short last block, as a real image ends
    for (let i = 4; i < img.length; i += 997) img[i] = i & 0xFF;
    const flash = new Uint8Array(11 * 1024).fill(0xFF);
    for (const { offset, report } of halfkayReports(img)) flash.set(report.subarray(64), offset);
    assert.deepEqual(flash.subarray(0, img.length), img);
    assert.ok(flash.subarray(img.length).every(b => b === 0xFF), "the short last block pads with erased flash");
});

test("the boot report is 0xFF in its three offset bytes and zero elsewhere", () => {
    const r = bootReport();
    assert.equal(r.length, REPORT_BYTES);
    assert.deepEqual([...r.subarray(0, 3)], [0xFF, 0xFF, 0xFF]);
    assert.ok(r.subarray(3).every(b => b === 0));
});

test("an image is refused when it is a HEX file, lacks the FCFB block, or exceeds the program flash", () => {
    assert.match(imageProblem(new TextEncoder().encode(":020000040000FA")), /HEX/);
    assert.match(imageProblem(new Uint8Array([1, 2, 3, 4, 5])), /not a Teensy 4 image/);
    assert.match(imageProblem(image(TEENSY41_CODE_BYTES + 1)), /more than/);
    assert.match(imageProblem(new Uint8Array(0)), /empty/);
    assert.equal(imageProblem(image(4096)), "");
});

test("a Teensy is recognized by PJRC's vendor id and the USB-serial product id", () => {
    assert.ok(isTeensySerial({ usbVendorId: 0x16C0, usbProductId: 0x0483 }));
    assert.ok(!isTeensySerial({ usbVendorId: 0x16C0, usbProductId: 0x0478 }), "the bootloader is HID, not a serial port");
    assert.ok(!isTeensySerial({ usbVendorId: 0x303A, usbProductId: 0x1001 }));
    assert.ok(!isTeensySerial({}));
});

test("an erase zeroes the settings' superblock pair, 8 KB at the bottom of the 1 MB settings area, so the firmware formats it at boot", () => {
    const reports = eraseSettingsReports();
    assert.equal(TEENSY41_SETTINGS_OFFSET, 0x6C0000);
    assert.deepEqual(reports.map(r => r.offset), [0, 1, 2, 3, 4, 5, 6, 7].map(i => 0x6C0000 + i * 1024));
    for (const { offset, report } of reports) {
        assert.deepEqual([...report.subarray(0, 3)], [offset & 0xFF, (offset >> 8) & 0xFF, offset >> 16]);
        assert.ok(report.subarray(64).every(b => b === 0));
        assert.ok(offset + 1024 <= TEENSY41_CODE_BYTES, "inside what the bootloader writes");
    }
});

// Every Teensy reports the same USB ids, and the installed board's port vanishes while it reboots, so a second granted Teensy would be the only match: it must never be the one read or sent defaults.
test("after the reboot the installer finds the installed board, never another granted Teensy", async () => {
    const teensy = () => ({ getInfo: () => ({ usbVendorId: 0x16C0, usbProductId: 0x0483 }) });
    const installing = teensy(), other = teensy(), esp = { getInfo: () => ({ usbVendorId: 0x10C4, usbProductId: 0xEA60 }) };
    const saved = globalThis.navigator;
    try {
        let granted = [installing, other, esp];
        Object.defineProperty(globalThis, "navigator", { value: { serial: { getPorts: async () => granted } }, configurable: true });
        const others = await otherTeensyPorts(installing);
        assert.deepEqual(others, [other]);
        granted = [other, esp];                                         // rebooting: only the other board answers
        assert.equal(await rebootedSerialPort(0, others), null);        // so nothing is taken
        granted = [other, esp, installing];                             // back on the bus
        assert.equal(await rebootedSerialPort(0, others), installing);
    } finally {
        Object.defineProperty(globalThis, "navigator", { value: saved, configurable: true });
    }
});
