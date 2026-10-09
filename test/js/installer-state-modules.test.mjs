// The installer reads a catalog entry's `state` document as modules: root members are top-level
// modules (type not written), an object member with a `type` is a child module, and scalars and
// arrays are controls. Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { stateModules } from "../../src/ui/install-picker-devices.js";

test("a root member is a top-level module whose type is not written", () => {
    const mods = stateModules({ state: { System: { deviceModel: "x" } } });
    assert.deepEqual(mods, [{ id: "System", type: null, controls: { deviceModel: "x" } }]);
});

// The type is an opaque string here, so no real module is named.
test("an object member with a type is a child module, its scalars and arrays are controls", () => {
    const mods = stateModules({ state: { Drivers: { $patch: "replace", Out: { type: "AnyDriver", pins: "16", list: [1, 2] } } } });
    assert.deepEqual(mods.map((m) => m.id), ["Drivers", "Out"]);
    assert.deepEqual(mods[0].controls, {}, "$patch is not a control");
    assert.deepEqual(mods[1], { id: "Out", type: "AnyDriver", controls: { pins: "16", list: [1, 2] } });
});

test("a child of a child module is reached depth first", () => {
    const mods = stateModules({ state: { Effects: { Layer: { type: "Layer", Noise: { type: "NoiseEffect", speed: 3 } } } } });
    assert.deepEqual(mods.map((m) => m.id), ["Effects", "Layer", "Noise"]);
});

test("an entry without a state document has no modules", () => {
    for (const entry of [undefined, null, {}, { state: null }, { state: [] }]) assert.deepEqual(stateModules(entry), []);
});
