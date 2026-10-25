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

test("a desktop build is named by its operating system, a container as Docker", () => {
    assert.deepEqual(walkChoices(sys("arm64", "macos-arm64")),
        { platform: "macOS", target: "Desktop", source: "The computer's audio on macOS" });
    assert.equal(walkChoices(sys("x64", "windows-x64")).platform, "Windows");
    assert.equal(walkChoices(sys("arm64", "linux-arm64")).platform, "Linux");
    assert.equal(walkChoices(sys("x64", "docker")).source, "A PulseAudio server into the container");
});

test("a device that says nothing about itself preselects nothing", () => {
    assert.deepEqual(walkChoices(undefined), {});
    assert.deepEqual(walkChoices(sys("", "")), {});
});
