// The header carries the uptime for everyone, and the frame rate and the memory badges from expert
// mode up: a user watching a show has no use for them, and on a phone they crowded the device name
// into three lines.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

const headerStats = new Function(`return ${fnSource("headerStats")}`)();

// A System card's controls as a device reports them.
const ctrls = [
    { name: "uptime", value: "3h 12m" },
    { name: "fps", value: 120 },
    { name: "heap", value: 100 * 1024, total: 300 * 1024 },
    { name: "maxBlock", value: "96KB" },
];

test("in basic mode the header shows the uptime alone", () => {
    assert.equal(headerStats(ctrls, false), "3h 12m");
});

test("from expert mode up the header adds the rate and the memory after the uptime", () => {
    assert.equal(headerStats(ctrls, true), "3h 12m · 🕒 120 fps · 🧠 200KB · 🧱 96KB");
});

test("a desktop's zero block size and a missing control leave their badge out", () => {
    const desktop = [{ name: "uptime", value: "5m" }, { name: "maxBlock", value: "0KB" }];
    assert.equal(headerStats(desktop, true), "5m");
});
