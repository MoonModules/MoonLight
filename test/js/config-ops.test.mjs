// Installer config-ops contract — the APPLY_OP plan that applies a device-model's catalog
// entry must CLEAR every parent it adds into before adding, so the "apply device defaults"
// path produces the same tree whether or not the flash was erased first.
//
// Why this matters: the device-side `add` is idempotent on the module name (an existing
// id returns AlreadyExists and is skipped). On a non-erased device the persisted tree
// already holds the entry's modules, so without a clearChildren pre-pass the re-add is a
// no-op and a stale or structurally-different module lingers — the bug where defaults
// "only applied if I also ticked Erase flash". This test pins that every add-parent (and
// every replaceChildren container) is cleared, and that clears precede adds precede sets.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { planConfigOps } from "../../mooninstaller/config-ops.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const catalog = JSON.parse(readFileSync(join(ROOT, "mooninstaller", "deviceModels.json"), "utf8"));

// Indices of the first op of each kind, so we can assert global ordering.
const firstIndex = (ops, pred) => ops.findIndex(pred);
const lastIndex = (ops, pred) => ops.reduce((acc, o, i) => (pred(o) ? i : acc), -1);

test("every add-parent is cleared, unless the parent is itself added fresh", () => {
    assert.ok(Array.isArray(catalog) && catalog.length > 0, "deviceModels.json empty");
    for (const entry of catalog) {
        const ops = planConfigOps(entry);
        const added = new Set(ops.filter(o => o.op === "add").map(o => o.id));
        const addParents = new Set(ops.filter(o => o.op === "add").map(o => o.parent));
        const cleared = new Set(ops.filter(o => o.op === "clearChildren").map(o => o.parent));
        for (const p of addParents) {
            // A parent the entry itself adds (e.g. Layer, added under Layers) starts empty,
            // so it needs no clear; only a pre-existing parent must be cleared so a stale
            // same-named child can't survive the AlreadyExists-skip on the re-add.
            if (added.has(p)) {
                assert.ok(!cleared.has(p),
                    `entry "${entry.name}": clears "${p}" but also adds it fresh — the clear is a no-op.`);
            } else {
                assert.ok(cleared.has(p),
                    `entry "${entry.name}": adds into pre-existing "${p}" but never clears it — a ` +
                    `non-erased device would keep the stale "${p}" child (AlreadyExists skips the re-add).`);
            }
        }
    }
});

test("all clearChildren ops come before all add/set ops", () => {
    for (const entry of catalog) {
        const ops = planConfigOps(entry);
        if (!ops.some(o => o.op === "clearChildren")) continue;
        const firstNonClear = firstIndex(ops, o => o.op !== "clearChildren");
        if (firstNonClear === -1) continue;   // clears-only plan — nothing for them to precede
        const lastClear = lastIndex(ops, o => o.op === "clearChildren");
        assert.ok(lastClear < firstNonClear,
            `entry "${entry.name}": a clearChildren runs after an add/set — clearing must be a ` +
            `pre-pass, or it would wipe a module the entry just added.`);
    }
});

test("a module's add precedes its own set ops", () => {
    for (const entry of catalog) {
        const ops = planConfigOps(entry);
        for (const m of (entry.modules || [])) {
            if (!m || !m.parent_id || !m.type || !m.controls) continue;
            const addAt = firstIndex(ops, o => o.op === "add" && o.id === m.id);
            const firstSet = firstIndex(ops, o => o.op === "set" && o.module === m.id);
            if (firstSet === -1) continue;
            assert.ok(addAt !== -1 && addAt < firstSet,
                `entry "${entry.name}" module "${m.id}": a set precedes its add (or no add) — ` +
                `the device would reject the set with ModuleNotFound.`);
        }
    }
});

test("S3 testbench entry adds Grid + Layer fresh and clears the pre-existing containers", () => {
    const s3 = catalog.find(b => b && b.name === "MM testbench S3");
    assert.ok(s3, "MM testbench S3 entry missing from deviceModels.json");
    const ops = planConfigOps(s3);
    const added = new Set(ops.filter(o => o.op === "add").map(o => o.id));
    const cleared = new Set(ops.filter(o => o.op === "clearChildren").map(o => o.parent));
    // The fix for "Layouts/Effects don't get created without erase": the entry now adds
    // Grid and Layer explicitly instead of assuming the boot defaults are present.
    for (const id of ["Grid", "Layer"]) {
        assert.ok(added.has(id), `S3 entry no longer adds "${id}" — a non-erased device without that boot default gets no ${id}`);
    }
    // Pre-existing containers the entry adds children into are cleared so stale children go.
    // Audio is a Service now (added under Services, not System) — System hosts no user-added
    // children, so the entry never touches it and emits no clearChildren for it.
    for (const p of ["Services", "Drivers", "Layouts", "Effects"]) {
        assert.ok(cleared.has(p), `S3 entry does not clear pre-existing "${p}"`);
    }
    assert.ok(!cleared.has("System"), `S3 entry clears "System" — but System has no user-added children to clear`);
    // Layer is added fresh, so it must NOT be cleared (would be a ModuleNotFound no-op).
    assert.ok(!cleared.has("Layer"), `S3 entry clears "Layer" but also adds it fresh — redundant`);
});

test("a deduped parent is cleared exactly once", () => {
    // Drivers hosts two modules in the S3 entry (RmtLed + NetworkSend); it must be cleared
    // once, not once per child (a second clear would wipe the first child just re-added).
    const s3 = catalog.find(b => b && b.name === "MM testbench S3");
    const driversClears = planConfigOps(s3).filter(o => o.op === "clearChildren" && o.parent === "Drivers");
    assert.equal(driversClears.length, 1, "Drivers cleared more than once — clears must be deduped");
});

test("empty / malformed entry yields no ops (robust to any input)", () => {
    assert.deepEqual(planConfigOps(undefined), []);
    assert.deepEqual(planConfigOps({}), []);
    assert.deepEqual(planConfigOps({ modules: null }), []);
    assert.deepEqual(planConfigOps({ modules: [null, 42, {}, { id: "" }] }), []);
});
