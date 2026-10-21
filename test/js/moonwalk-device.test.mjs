// MoonWalk on a device: a process is done when the device's own state shows it, and what comes next is the first process not done whose dependencies are.
// Run: `node --test test/js/moonwalk-device.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { doneOnDevice, nextOf, choicesOf, applies, stepHolds, targetOf, advance, commandOf, took, conditionText } from "../../docs/assets/moonwalk.js";

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

test("a contributor's process is done when every step the change needs has a pass that still holds", async () => {
    const { doneBySteps } = await import("../../docs/assets/moonwalk.js");
    const steps = (testsFresh) => ({
        loop: [{ id: "b", triggered: true, fresh: true }, { id: "t", triggered: true, fresh: testsFresh }, { id: "docs", triggered: false, fresh: false }],
        idle: [{ id: "x", triggered: false, fresh: false }],
    });
    assert.deepEqual([...doneBySteps(steps(true))], ["loop"]);   // a step the change does not need waits for nothing; a process needing none is not done by runs
    assert.deepEqual([...doneBySteps(steps(false))], []);
});

test("a step running a subprocess stands for its steps: needed when any is, passed when every needed one is", async () => {
    const { gateOf } = await import("../../docs/assets/moonwalk.js");
    const group = { id: "code", when: { process: "commit-code" } };
    const gates = (buildFresh, testsTriggered) => ({
        commit: [{ id: "specs", process: "commit", triggered: true, fresh: true }, { id: "code", process: "commit-code", triggered: true, fresh: buildFresh }],
        "commit-code": [{ id: "code", process: "commit-code", triggered: true, fresh: buildFresh }, { id: "test", process: "commit-code", triggered: testsTriggered, fresh: false }],
    });
    assert.deepEqual(gateOf(gates(true, false), "commit", group), { triggered: true, fresh: true, running: false });
    assert.equal(gateOf(gates(true, true), "commit", group).fresh, false);   // a needed step without a pass holds the group back
    assert.equal(gateOf(gates(false, false), "commit", group).fresh, false);
    const idle = { commit: [], "commit-code": [{ id: "code", process: "commit-code", triggered: false, fresh: false }] };
    assert.equal(gateOf(idle, "commit", group).triggered, false);   // not needed for this change
    assert.equal(gateOf(gates(true, false), "commit", { id: "specs", when: { run: { script: "x" } } }).fresh, true);   // a script step is its own run
    assert.equal(gateOf(gates(true, false), "commit", { id: "code", when: { run: { script: "x" } } }), undefined);    // a subprocess's step of the same id is not this process's
    assert.equal(gateOf(gates(true, false), "commit", { id: "review", when: { process: "review-finding" } }), undefined);   // a subprocess running no script has no status
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

test("a step's call copies as the line the map carries, and a step that runs nothing copies nothing", () => {
    assert.equal(commandOf({ command: "uv run moondeck/check/check_specs.py", when: { run: { script: "moondeck/check/check_specs.py" } } }), "uv run moondeck/check/check_specs.py");
    assert.equal(commandOf({ when: { manual: "Stage." } }), null);
});

test("a step a change triggers still shows on the site, which knows no change: only a reader's choice hides a step", () => {
    assert.equal(applies({ id: "build", given: [{ changed: ["src/**"] }] }, {}), true);
});

test("a duration rounds before it splits into minutes, so it never reads 60 seconds", () => {
    assert.equal(took(119.6), "2m00s");
    assert.equal(took(59.6), "1m00s");
    assert.equal(took(42.2), "42s");
    assert.equal(took(null), "");
});

test("a condition reads as the words a reader checks the device against", () => {
    assert.equal(conditionText([{ module: "RainbowEffect", exists: true }]), "a RainbowEffect module exists");
    assert.equal(conditionText([{ module: "Drivers", control: "brightness", minimum: 1, maximum: 20 }]), "Drivers brightness is 1 to 20");
    assert.equal(conditionText([{ module: "Drivers", control: "brightness", maximum: 20 }, { module: "MidiService", control: "share", equals: true }]), "Drivers brightness is at most 20 and MidiService share is true");
    assert.equal(conditionText([{ status: 200 }]), "");   // what the state cannot answer has no words
});

test("a select's index also reads as its option's name, so a step that names the option holds", () => {
    const state = { modules: [{ name: "Midi", type: "MidiService", controls: [{ name: "source", type: "select", value: 1, options: ["browser", "USB", "network"] }] }] };
    const step = (e) => ({ then: [{ module: "MidiService", control: "source", ...e }] });
    assert.equal(stepHolds(step({ equals: "USB" }), state), true);
    assert.equal(stepHolds(step({ equals: "network" }), state), false);
    assert.equal(stepHolds(step({ notEquals: "USB" }), state), false);
    assert.equal(stepHolds(step({ equals: 1 }), state), true);   // the index still matches
});
