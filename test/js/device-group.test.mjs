// A device group's update (devicegroup.js): the waves it runs in and how a failure stops them, and which members are skipped.
// Runs in node, the install faked. Run: `node --test test/js/device-group.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { GROUP_WAVE_SIZE, planWaves, runWaves, memberState } from "../../src/ui/devicegroup.js";
import { heldGroups, membershipOffers, memberRowId } from "../../src/ui/devicepopup.js";

const member = (name, extra = {}) => ({ name, url: `https://example.org/${name}.bin`, ...extra });
const names = (waves) => waves.map(w => w.map(m => m.name));
const fleet = (n, extra) => Array.from({ length: n }, (_, i) => member(`d${i + 1}`, extra?.(i)));

test("the first wave is one member, the rest follow at most GROUP_WAVE_SIZE at a time", () => {
    const waves = planWaves(fleet(8));
    assert.equal(GROUP_WAVE_SIZE, 3);
    assert.deepEqual(names(waves), [["d1"], ["d2", "d3", "d4"], ["d5", "d6", "d7"], ["d8"]]);
});

test("the page's own device updates last, alone, whatever its place in the group", () => {
    const members = [member("me", { self: true }), ...fleet(3)];
    assert.deepEqual(names(planWaves(members)), [["d1"], ["d2", "d3"], ["me"]]);
    assert.deepEqual(names(planWaves([member("me", { self: true })])), [["me"]]);
});

test("a skipped member or one with no image is in no wave", () => {
    const members = [member("held", { skip: "updated by hand" }), member("box", { url: null }), ...fleet(2)];
    assert.deepEqual(names(planWaves(members)), [["d1"], ["d2"]]);
    assert.deepEqual(planWaves([]), []);
});

test("a failure stops the waves after it, finishes its own wave, and leaves the rest untouched", async () => {
    const started = [];
    const install = async (m) => { started.push(m.name); return m.name === "d3" ? { ok: false, text: "No result after five minutes." } : { ok: true, text: "Updated." }; };
    const out = await runWaves(planWaves(fleet(8)), install);
    assert.deepEqual(started, ["d1", "d2", "d3", "d4"]);
    assert.deepEqual(out.failed.map(f => [f.member.name, f.text]), [["d3", "No result after five minutes."]]);
    assert.deepEqual(out.done.map(m => m.name), ["d1", "d2", "d4"]);
    assert.deepEqual(out.untouched.map(m => m.name), ["d5", "d6", "d7", "d8"]);
});

test("a first member that fails stops everything, the page's own device included", async () => {
    const started = [];
    const members = [member("me", { self: true }), ...fleet(3)];
    const out = await runWaves(planWaves(members), async (m) => { started.push(m.name); return { ok: false, text: "Refused." }; });
    assert.deepEqual(started, ["d1"]);
    assert.deepEqual(out.untouched.map(m => m.name), ["d2", "d3", "me"]);
});

test("a wave's members install together, and every wave starts after the one before ended", async () => {
    let live = 0, peak = 0;
    const order = [];
    const install = async (m) => {
        order.push(`start ${m.name}`);
        peak = Math.max(peak, ++live);
        await new Promise((r) => setImmediate(r));
        live--;
        order.push(`end ${m.name}`);
        return { ok: true, text: "" };
    };
    const out = await runWaves(planWaves(fleet(5)), install);
    assert.equal(peak, GROUP_WAVE_SIZE);
    assert.equal(order.indexOf("start d2"), order.indexOf("end d1") + 1, "the second wave waits for the first");
    assert.equal(out.done.length, 5);
    assert.deepEqual(out.untouched, []);
});

test("skipped members stay out of the install, a clean run reports none failed", async () => {
    const installed = [];
    const members = [member("held", { skip: "not heard" }), ...fleet(2)];
    const out = await runWaves(planWaves(members), async (m) => { installed.push(m.name); return { ok: true, text: "" }; });
    assert.deepEqual(installed, ["d1", "d2"]);
    assert.deepEqual(out.failed, []);
});

test("a member is skipped with its reason: missing, not MoonLight, silent, or with no image", () => {
    const fw = { version: "1.0.0", firmware: "esp32s3" };
    const u = { label: "v1.1.0", version: "1.1.0", url: "https://x/a.bin", sha256: "ab" };
    assert.deepEqual(memberState({ type: "MoonLight", missing: true }, fw, u), { skip: "not heard" });
    assert.deepEqual(memberState({ type: "WLED" }, fw, u), { skip: "not a MoonLight device" });
    assert.deepEqual(memberState({ type: "MoonLight" }, null, null), { skip: "does not answer" });
    assert.deepEqual(memberState({ type: "MoonLight" }, fw, null), { version: "1.0.0" });
    assert.deepEqual(memberState({ type: "MoonLight" }, fw, u), { version: "1.0.0", label: "v1.1.0", url: u.url, sha256: "ab", target: "1.1.0" });
    const held = memberState({ type: "MoonLight" }, { ...fw, summary: { update: "container" } }, { ...u, url: null });
    assert.match(held.skip, /container's host updates it/);
    assert.equal(held.url, undefined);
});


test("a node is offered to join each group its device's state holds, member of it or not, and to leave the ones it is in", () => {
    const state = { modules: [{ name: "Network", children: [] }, { name: "Devices", children: [
        { name: "DesktopGroup", role: "devicegroup", controls: [{ name: "id", value: "a1" }, { name: "groupName", value: "DesktopGroup" },
          { name: "members", value: [{ id: 3, device: "MM-P4-pc", mac: "30:ED:A0:E1:5C:E9" }] }] },
        { name: "Hall 2", role: "devicegroup", controls: [{ name: "id", value: "b2" }, { name: "groupName", value: "Hall" }, { name: "members", value: [] }] },
        { name: "Discovery", type: "DevicesModule", controls: [] }] }] };   // the state's modules, a top-level list as /api/state serves it
    const held = heldGroups(state);
    assert.deepEqual(held.map(g => [g.id, g.name, g.module]), [["a1", "DesktopGroup", "DesktopGroup"], ["b2", "Hall", "Hall 2"]]);   // the module's own name kept
    assert.deepEqual(membershipOffers(held, "30:ed:a0:e1:5c:e9").map(g => g.member), [true, false]);
    assert.deepEqual(membershipOffers(heldGroups(null), "aa:bb:cc:dd:ee:ff"), []);   // a device holding no group offers an empty list
    assert.equal(memberRowId(held[0], "30:ed:a0:e1:5c:e9"), 3);   // leaving finds the row by its MAC, whatever its case
    assert.equal(memberRowId(held[1], "30:ed:a0:e1:5c:e9"), null);
});
