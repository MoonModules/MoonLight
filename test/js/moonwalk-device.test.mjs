// MoonWalk on a device: a process is done when the device's own state shows it, and what comes next is the first process not done whose dependencies are.
// Run: `node --test test/js/moonwalk-device.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { doneOnDevice, nextOf, choicesOf, applies } from "../../docs/assets/moonwalk.js";

const model = {
    stages: [{ id: "install", processes: ["install"] }, { id: "light", processes: ["grid", "effect", "status-only"] }],
    processes: {
        install: { actor: "user", steps: [] },
        grid: { actor: "user", dependsOn: ["install"], steps: [{ id: "w", then: [{ module: "Grid", control: "width", equals: 16 }] }] },
        effect: { actor: "user", dependsOn: ["grid"], steps: [{ id: "a", then: [{ module: "Rainbow", exists: true }] }] },
        "status-only": { actor: "contributor", steps: [{ id: "s", then: [{ status: 200 }] }] },
    },
};
const device = (width, extra = []) => ({ modules: [{ name: "Layouts", children: [{ name: "Grid", controls: [{ name: "width", value: width }] }] }, ...extra] });

test("a process is done when every expectation the device's state answers holds", () => {
    assert.deepEqual([...doneOnDevice(model, device(16))], ["grid"]);
});

test("one expectation that does not hold leaves the process to do", () => {
    assert.equal(doneOnDevice(model, device(8)).has("grid"), false);
});

test("a module that exists is an expectation the state answers", () => {
    assert.equal(doneOnDevice(model, device(16, [{ name: "Rainbow" }])).has("effect"), true);
});

test("a process the state cannot answer is never done by the device, only by the reader's mark", () => {
    assert.equal(doneOnDevice(model, device(16)).has("status-only"), false);
    assert.equal(doneOnDevice(model, device(16)).has("install"), false);
});

test("next is a process not done whose dependencies all are, in the journey's order", () => {
    assert.deepEqual(nextOf(model, new Set(["install"])), ["grid", "status-only"]);
    assert.deepEqual(nextOf(model, new Set(["install", "grid"])), ["effect", "status-only"]);
});

test("next can be narrowed to the reader's actor", () => {
    assert.deepEqual(nextOf(model, new Set(["install"]), "user"), ["grid"]);
});

test("a list control counts as its number of rows", () => {
    const wifi = { stages: [{ id: "set-up", processes: ["wifi"] }], processes: { wifi: { steps: [{ id: "pick", then: [{ module: "WiFi", control: "known", notEquals: 0 }] }] } } };
    const known = (rows) => ({ modules: [{ name: "WiFi", controls: [{ name: "known", value: rows }] }] });
    assert.equal(doneOnDevice(wifi, known([])).has("wifi"), false);
    assert.equal(doneOnDevice(wifi, known([{ ssid: "home" }])).has("wifi"), true);
});

test("the choices are what is here and what comes next, without what is done or listed twice", () => {
    const choices = choicesOf(model, new Set(["install"]), "", ["grid", "effect", "install"]);
    assert.deepEqual(choices.here, ["grid", "effect"]);   // install is done, so it is no choice
    assert.deepEqual(choices.next, ["status-only"]);       // grid is next too, but already here
});

test("a step shows for the chosen option, and a step for every option always shows", () => {
    const chosen = { platform: "Windows" };
    assert.equal(applies({ id: "download" }, chosen), true);
    assert.equal(applies({ id: "setup", given: [{ input: "platform", equals: "Windows" }] }, chosen), true);
    assert.equal(applies({ id: "dmg", given: [{ input: "platform", equals: "macOS" }] }, chosen), false);
    assert.equal(applies({ id: "after", given: ["install-desktop"] }, chosen), true);   // a process precondition is no choice
});
