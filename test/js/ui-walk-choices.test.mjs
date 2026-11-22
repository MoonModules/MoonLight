// MoonWalk opened on a device preselects the choices that follow from where it runs, so a process shows that device's steps first.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

const walkChoices = new Function(`return ${fnSource("walkChoices")}`)();
const sys = (chip, deviceModel) => [{ name: "chip", value: chip }, { name: "deviceModel", value: deviceModel }];

test("a board is an ESP32 or a Teensy, a board target, with its own microphone", () => {
    assert.deepEqual(walkChoices(sys("ESP32-S3", "Waveshare ESP32-S3-RGB-Matrix")),
        { platform: "ESP32", target: "Board", source: "A microphone on a board" });
    assert.equal(walkChoices(sys("i.MX RT1062", "")).platform, "Teensy");
});

test("a device that says nothing about itself preselects nothing", () => {
    assert.deepEqual(walkChoices(undefined), {});
    assert.deepEqual(walkChoices(sys("", "")), {});
});

const host = (firmware, deviceModel) => [{ name: "firmware", value: firmware }, { name: "deviceModel", value: deviceModel }];

test("a host is named by its firmware key, whatever its device model says, and a container by its catalog name", () => {
    assert.deepEqual(walkChoices(host("host-macos-arm64", "Generic macOS")),
        { platform: "macOS", target: "Host", source: "The computer's audio on macOS" });
    assert.equal(walkChoices(host("host-linux-x64", "Docker container")).source, "A PulseAudio server into the container");
    assert.equal(walkChoices(host("host-windows-x64", "")).platform, "Windows");
    assert.equal(walkChoices(host("host-linux-arm64", "Raspberry Pi 5")).platform, "Linux");
    assert.equal(walkChoices(host("host-linux-x64", "Docker container")).platform, "Docker");
    assert.equal(walkChoices(host("esp32s3-opi", "Docker container")).platform, "Docker");
});
