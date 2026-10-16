// MoonWalk on a device: a process is done when the device's own state shows it, and what comes next is the first process not done whose dependencies are.
// Run: `node --test test/js/moonwalk-device.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { doneOnDevice, nextOf, choicesOf, applies, stepHolds, targetOf, advance } from "../../docs/assets/moonwalk.js";

const model = {
    stages: [{ id: "install", processes: ["install"] }, { id: "light", processes: ["grid", "effect", "status-only"] }],
    processes: {
        install: { actor: "user", steps: [] },
        grid: { actor: "user", dependsOn: ["install"], steps: [{ id: "w", then: [{ module: "GridLayout", control: "width", equals: 16 }] }] },
        effect: { actor: "user", dependsOn: ["grid"], steps: [{ id: "a", then: [{ module: "RainbowEffect", exists: true }] }] },
        "status-only": { actor: "contributor", steps: [{ id: "s", then: [{ status: 200 }] }] },
    },
};
const device = (width, extra = []) => ({ modules: [{ name: "Layouts", type: "Layouts", children: [{ name: "Grid", type: "GridLayout", controls: [{ name: "width", value: width }] }] }, ...extra] });

test("a process is done when every expectation the device's state answers holds", () => {
    assert.deepEqual([...doneOnDevice(model, device(16))], ["grid"]);
});

test("a step names a module by type, so any module of that type can meet it, whatever its name", () => {
    const two = { modules: [{ name: "Small", type: "GridLayout", controls: [{ name: "width", value: 8 }] }, { name: "Wall", type: "GridLayout", controls: [{ name: "width", value: 16 }] }] };
    assert.equal(doneOnDevice(model, two).has("grid"), true);
    assert.equal(doneOnDevice(model, { modules: [{ name: "GridLayout", type: "RingLayout", controls: [{ name: "width", value: 16 }] }] }).has("grid"), false);   // a name equal to the type is no match
});

test("one expectation that does not hold leaves the process to do", () => {
    assert.equal(doneOnDevice(model, device(8)).has("grid"), false);
});

test("a module that exists is an expectation the state answers", () => {
    assert.equal(doneOnDevice(model, device(16, [{ name: "Rainbow", type: "RainbowEffect" }])).has("effect"), true);
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

test("a range holds within its inclusive bounds, either of which may be open", () => {
    const low = { id: "b", then: [{ module: "GridLayout", control: "width", minimum: 1, maximum: 20 }] };
    assert.equal(stepHolds(low, device(20)), true);    // the bound itself
    assert.equal(stepHolds(low, device(5)), true);
    assert.equal(stepHolds(low, device(0)), false);
    assert.equal(stepHolds(low, device(21)), false);
    assert.equal(stepHolds({ id: "c", then: [{ module: "GridLayout", control: "width", maximum: 20 }] }, device(0)), true);
});

test("a list control counts as its number of rows", () => {
    const wifi = { stages: [{ id: "install", processes: ["wifi"] }], processes: { wifi: { steps: [{ id: "pick", then: [{ module: "WiFiModule", control: "known", notEquals: 0 }] }] } } };
    const known = (rows) => ({ modules: [{ name: "WiFi", type: "WiFiModule", controls: [{ name: "known", value: rows }] }] });
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
    assert.equal(applies({ id: "after", given: ["install"] }, chosen), true);   // a process precondition is no choice
});

test("a host vouching for a process keeps it with the reader's own marks", async () => {
    const store = new Map([["mm_moonwalk_done", JSON.stringify(["first-look"])]]);
    globalThis.localStorage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, v) };
    const { markDone } = await import("../../docs/assets/moonwalk.js");
    markDone(["install"]);
    assert.deepEqual(JSON.parse(store.get("mm_moonwalk_done")).sort(), ["first-look", "install"]);
    delete globalThis.localStorage;
});

test("a contributor's process is done when every script it runs last passed", async () => {
    const { doneByRuns } = await import("../../docs/assets/moonwalk.js");
    const model = { processes: {
        loop: { steps: [{ id: "b", when: { run: { script: "moondeck/build/build_desktop.py" } } }, { id: "t", when: { run: { script: "moondeck/build/build_desktop.py", args: "--tests" } } }] },
        talk: { steps: [{ id: "m", when: { manual: "Agree on the plan." } }] },
    } };
    const runs = (testsPass) => [
        { script: "moondeck/build/build_desktop.py", args: "", passed: true },
        { script: "moondeck/build/build_desktop.py", args: "--tests", passed: testsPass },
    ];
    assert.deepEqual([...doneByRuns(model, runs(true))], ["loop"]);   // a process with no scripts is never done by runs
    assert.deepEqual([...doneByRuns(model, runs(false))], []);        // the same script with other arguments is another run
});

test("a step can name several options, and shows for any of them", () => {
    const step = { id: "download", given: [{ input: "platform", in: ["Windows", "macOS", "Linux"] }] };
    assert.equal(applies(step, { platform: "macOS" }), true);
    assert.equal(applies(step, { platform: "ESP32" }), false);
});

const steps = [
    { id: "open", when: { open: { module: "Layouts" } } },
    { id: "width", when: { set: { module: "GridLayout", control: "width", value: 16 } }, then: [{ module: "GridLayout", control: "width", equals: 16 }] },
    { id: "look", when: { manual: "Look at the lights." } },
];

test("a followed step points at the module type its action names, and the control it sets", () => {
    assert.deepEqual(targetOf(steps[1]), { type: "GridLayout", control: "width" });
    assert.deepEqual(targetOf(steps[0]), { type: "Layouts", control: undefined });
    assert.equal(targetOf(steps[2]), null);   // a manual step points at nothing in the app
});

test("a step the state cannot answer is the reader's to mark, so it is neither done nor not done", () => {
    assert.equal(stepHolds(steps[0], device(16)), undefined);
    assert.equal(stepHolds(steps[1], device(16)), true);
    assert.equal(stepHolds(steps[1], device(8)), false);
});

test("the bar moves past every step the state shows done and stops at the first it does not", () => {
    assert.equal(advance(steps, 1, device(8)), 1);    // the width is not set yet
    assert.equal(advance(steps, 1, device(16)), 2);   // set: on to looking, which waits for the reader
    assert.equal(advance(steps, 0, device(16)), 0);   // opening a card is the reader's Done, even with the width already set
});
