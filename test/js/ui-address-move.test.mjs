// An edit that moves the device away from the page asks first and names where it goes; a half-typed address never goes live.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { src, fnSource } from "./app-source.mjs";

// The decision functions over a stubbed page: its host, its modules, and a confirm that records what it was asked.
const page = (hostname, modules) => new Function("hostname", "modules", `
    const location = { hostname };
    const state = { modules };
    const asked = [];
    const allModules = () => modules;
    const confirmAddressMove = (to) => { asked.push(to); return Promise.resolve(to === undefined); };
    ${fnSource("addressMoveTarget")}
    ${fnSource("carryingInterface")}
    ${fnSource("allowAddressEdit")}
    return { addressMoveTarget, carryingInterface, allowAddressEdit, asked };
`)(hostname, modules);

const wired = (mode) => [
    { name: "Network", type: "NetworkModule", controls: [{ name: "mode", value: mode }] },
    { name: "Ethernet", type: "EthernetModule", controls: [{ name: "ipSettings", value: 0 }, { name: "ip", value: "192.168.1.250" }] },
    { name: "WiFi", type: "WiFiModule", controls: [] },
];

test("an address edit names where the device goes: the static address, or a lease from the router", () => {
    const g = page("192.168.1.40", wired("Ethernet"));
    assert.equal(g.addressMoveTarget("ipSettings", 1, "192.168.1.250"), "192.168.1.250");
    assert.equal(g.addressMoveTarget("ipSettings", 1, undefined), undefined);   // no address yet: nothing moves until one is typed
    assert.equal(g.addressMoveTarget("ipSettings", 1, "0.0.0.0"), undefined);
    assert.equal(g.addressMoveTarget("ipSettings", 0, "192.168.1.250"), null);
    assert.equal(g.addressMoveTarget("ip", "10.0.0.9", "192.168.1.250"), "10.0.0.9");
    assert.equal(g.addressMoveTarget("gateway", "10.0.0.1", "192.168.1.250"), undefined);
});

test("the page knows which interface carries it, and the access point is none", () => {
    assert.equal(page("192.168.1.40", wired("Ethernet")).carryingInterface(), "Ethernet");
    assert.equal(page("192.168.1.40", wired("WiFi STA")).carryingInterface(), "WiFi STA");
    assert.equal(page("4.3.2.1", wired("WiFi STA")).carryingInterface(), "");
});

test("the Ethernet card asks before moving the device only while Ethernet carries the page", async () => {
    const carried = page("192.168.1.40", wired("Ethernet"));
    assert.equal(await carried.allowAddressEdit("Ethernet", "ipSettings", 1), false);   // the stub declines
    assert.deepEqual(carried.asked, ["192.168.1.250"]);
    assert.equal(await carried.allowAddressEdit("Ethernet", "gateway", "192.168.1.1"), true);

    const overWifi = page("192.168.1.40", wired("WiFi STA"));
    assert.equal(await overWifi.allowAddressEdit("Ethernet", "ipSettings", 1), true);
    assert.deepEqual(overWifi.asked, []);
    assert.equal(await page("192.168.1.40", wired("Ethernet")).allowAddressEdit("WiFi", "ipSettings", 1), true);
});

test("an address field is sent when it is committed, not as it is typed", () => {
    const ipv4 = src.slice(src.indexOf('case "ipv4": {'), src.indexOf('case "time": {'));
    assert.ok(ipv4.includes('addEventListener("change"'));
    assert.ok(!ipv4.includes("debounceSend"));
});

test("a control marks its own reset button as it writes, on a card and in a row alike", () => {
    const control = fnSource("createControl");
    assert.ok(control.includes('resetBtn.classList.toggle("active", !controlValuesEqual({ ...ctrl, value }, def))'));
    assert.ok(fnSource("appendResetButton").includes("return btn;"));
});
