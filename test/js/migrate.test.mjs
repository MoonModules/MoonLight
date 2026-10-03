// The backup/restore rename map (src/ui/migrate.js): every schema break MIGRATING.md
// records, applied client-side to a backup before upload. These tests are the functional
// documentation of what a restore rescues, what it flags for review, and what it never touches.
// Run: `node --test test/js/migrate.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { applyMigrations, CONTROL_RENAMES } from "../../src/ui/migrate.js";

test("a user preset that shares a renamed filename is the user's, not migrated", () => {
    const { files, report } = applyMigrations({ "/.config/presets/Layers.json": '{"x":1}' });
    assert.ok(files["/.config/presets/Layers.json"]);          // untouched name...
    assert.equal(files["/.config/presets/Effects.json"], undefined);
    assert.ok(!report.some(r => r.detail.includes("file →")));  // ...and no file rename reported
});

test("a driver's preset control migrates to fixture with its value", () => {
    const cfg = { "0.type": "RmtLedDriver", "0.preset": 2, "0.enabled": true };
    const { files } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.fixture"], 2);
    assert.equal(out["0.preset"], undefined);
    assert.equal(out["0.enabled"], true);
});

test("light presets are fixture profiles: type, list key and the driver's Select all map", () => {
    const lib = { "0.type": "LightPresetsModule", "0.presets": [{ id: 1, name: "GRB" }] };
    const out1 = applyMigrations({ "/.config/Drivers.json": JSON.stringify({ ...lib, "1.type": "RmtLedDriver", "1.lightPreset": 3, "1.presetRef": "RGBW" }) });
    const out = JSON.parse(out1.files["/.config/Drivers.json"]);
    assert.equal(out["0.type"], "FixtureProfilesModule");
    assert.deepEqual(out["0.profiles"], [{ id: 1, name: "GRB" }]);
    assert.equal(out["0.presets"], undefined);
    assert.equal(out["1.fixture"], 3);
    assert.equal(out["1.fixtureRef"], "RGBW");
    assert.equal(out["1.lightPreset"], undefined);
    // A `presets` key on any other module (ControlModule's own) is not touched.
    const other = applyMigrations({ "/.config/Control.json": JSON.stringify({ presets: 1 }) });
    assert.equal(JSON.parse(other.files["/.config/Control.json"]).presets, 1);
});

test("a bundle carrying both old and new names reports the collision, never silent", () => {
    const cfg = { "0.type": "PreviewDriver", "0.fps": 24, "0.targetFps": 30 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.targetFps"], 30);   // last write wins (plain key iterated after the rename)...
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("later one won")));   // ...and says so
});

test("renamed config files land under their new names, reported", () => {
    const { files, report } = applyMigrations({
        "/.config/Layers.json": JSON.stringify({ enabled: true }),
        "/.config/LayoutGroup.json": JSON.stringify({ enabled: false }),
        "/.config/DriverGroup.json": JSON.stringify({ enabled: true }),
    });
    assert.ok(files["/.config/Effects.json"]);
    assert.ok(files["/.config/Layouts.json"]);
    assert.ok(files["/.config/Drivers.json"]);
    assert.equal(files["/.config/Layers.json"], undefined);
    assert.equal(JSON.parse(files["/.config/Layouts.json"]).enabled, false);   // values survive the rename
    assert.ok(report.some(r => r.kind === "renamed" && r.detail.includes("Effects.json")));
});

test("renamed module types map in place, including nested children", () => {
    const cfg = { "0.type": "Layers", "0.0.type": "ParlioLedDriver", "0.0.enabled": true };
    const { files, report } = applyMigrations({ "/.config/Effects.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Effects.json"]);
    assert.equal(out["0.type"], "Effects");
    assert.equal(out["0.0.type"], "ParallelLedDriver");
    // Parlio kept its peripheral value, so the map SETS it rather than asking for review.
    assert.equal(out["0.0.peripheral"], "Parlio");
    assert.ok(report.filter(r => r.kind === "renamed").length >= 2);
});

test("chip-dependent peripherals get a review entry, never a guessed value", () => {
    const cfg = { "0.type": "I80LedDriver", "0.pin0": 16 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.type"], "ParallelLedDriver");
    assert.equal(out["0.peripheral"], undefined);   // not guessed
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("re-picked")));
});

test("a deterministic peripheral move is set: MoonLedDriver becomes ParallelLedDriver on LCD-MM", () => {
    const cfg = { "0.type": "MoonLedDriver", "0.useRing": true };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.type"], "ParallelLedDriver");
    assert.equal(out["0.peripheral"], "LCD-MM");
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("S3/P4 only")));
});

test("control VALUES migrate too: peripheral MoonI80 maps to LCD-MM, i80 flags review", () => {
    const cfg = { "0.type": "ParallelLedDriver", "0.peripheral": "MoonI80",
                  "1.type": "ParallelLedDriver", "1.peripheral": "i80" };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.peripheral"], "LCD-MM");                 // deterministic: mapped
    assert.equal(out["1.peripheral"], "i80");                    // chip-dependent: untouched...
    assert.ok(report.some(r => r.kind === "review" && r.where.includes("1.peripheral")));   // ...but flagged
    assert.ok(report.some(r => r.kind === "renamed" && r.detail.includes("MoonI80 → LCD-MM")));
});

test("control renames apply, and semantics changes flag review instead of guessing", () => {
    const cfg = { "0.type": "PreviewDriver", "0.fps": 30, "1.type": "MoonLedDriver", "1.forceRing": 2 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.targetFps"], 30);
    assert.equal(out["0.fps"], undefined);
    assert.equal(out["1.useRing"], 2);              // name maps (scope follows the type rename); value untouched...
    assert.equal(out["1.type"], "ParallelLedDriver");
    assert.ok(report.some(r => r.kind === "review" && r.where.includes("useRing")));   // ...but flagged
});

test("renames are scoped by module type: NetworkSendDriver keeps its own fps", () => {
    // The bench-found bug: fps → targetFps belongs to PreviewDriver alone; other drivers
    // legitimately own an fps control today and a blanket rename corrupts them.
    const cfg = { "0.type": "NetworkSendDriver", "0.fps": 60, "1.type": "PreviewDriver", "1.fps": 24 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(out["0.fps"], 60);                 // untouched
    assert.equal(out["1.targetFps"], 24);           // renamed
    assert.equal(report.length, 1);
});

test("preset payload values rename (the captured container), other content byte-exact", () => {
    const preset = '{"container":"Layers","role":"layer","x":1}';
    const script = "let x = 1; //, untouched, not JSON, not a preset";
    const { files, report } = applyMigrations({
        "/.config/presets/p1.json": preset,
        "/scripts/anim.mle": script,
    });
    assert.equal(files["/.config/presets/p1.json"], '{"container":"Effects","role":"effects","x":1}');
    assert.equal(files["/scripts/anim.mle"], script);   // byte-exact passthrough
    assert.ok(report.some(r => r.detail.includes("preset value")));
});

test("unknown content passes through untouched with an empty report", () => {
    const cfg = JSON.stringify({ modern: true, "0.type": "NoiseEffect", "0.speed": 128 });
    const { files, report } = applyMigrations({ "/.config/Effects.json": cfg });
    assert.deepEqual(JSON.parse(files["/.config/Effects.json"]), JSON.parse(cfg));
    assert.equal(report.length, 0);
});

test("a corrupt config file is restored as-is and flagged for review", () => {
    const { files, report } = applyMigrations({ "/.config/Broken.json": "{not json" });
    assert.equal(files["/.config/Broken.json"], "{not json");
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("not valid JSON")));
});

// Every CONTROL_RENAMES entry is consumed as `cr.name` (renameKeys), so an entry written with any
// other key silently renames nothing: the old key stays, the user's value never reaches the new
// control, and no report line says so. `soundReactive` shipped with `to:` (the shape FILE_RENAMES
// uses) and did exactly that. This checks the table's own shape as well as one worked example,
// because the shape is the part a new entry gets wrong.
test("every control rename declares the key renameKeys reads, and carries its scope", () => {
    for (const [from, r] of Object.entries(CONTROL_RENAMES)) {
        assert.ok(typeof r.name === "string" && r.name.length > 0,
                  `${from}: needs name: (found keys ${Object.keys(r).join(", ")})`);
        assert.ok(Array.isArray(r.onTypes) && r.onTypes.length > 0,
                  `${from}: a bare-name rename needs onTypes, per the scoping rule`);
    }
});

test("a saved soundReactive value comes back under audioReactive", () => {
    const files = {
        "/.config/Effects.json": JSON.stringify({
            "type": "Effects",
            "0.type": "FishTankEffect",
            "0.soundReactive": true,
            "0.speed": 42,
        }),
    };
    const { files: out } = applyMigrations(files);
    const cfg = JSON.parse(out["/.config/Effects.json"]);
    assert.equal(cfg["0.audioReactive"], true, "the value must land on the new name");
    assert.ok(!("0.soundReactive" in cfg), "the old key must be gone");
    assert.equal(cfg["0.speed"], 42, "unrelated controls are untouched");
});

// Scoped, per the rule the table documents: the same word on a module that never declared it is
// somebody else's control and must not be rewritten.
test("soundReactive is left alone on a module outside its scope", () => {
    const files = {
        "/.config/Effects.json": JSON.stringify({
            "type": "Effects",
            "0.type": "NoiseEffect",
            "0.soundReactive": true,
        }),
    };
    const { files: out } = applyMigrations(files);
    const cfg = JSON.parse(out["/.config/Effects.json"]);
    assert.equal(cfg["0.soundReactive"], true);
    assert.ok(!("0.audioReactive" in cfg));
});

test("the receiver lists become hosts, and discovery's WLED switch becomes its addressing", () => {
    const drivers = { "0.type": "NetworkSendDriver", "0.ips": "192.168.1.70-74", "0.lightsPerIp": "100" };
    const network = { "2.type": "DevicesModule", "2.wledCompatible": true, "3.type": "OscModule", "3.feedbackTo": "192.168.1.147" };
    const { files } = applyMigrations({
        "/.config/Drivers.json": JSON.stringify(drivers),
        "/.config/Network.json": JSON.stringify(network),
    });
    const d = JSON.parse(files["/.config/Drivers.json"]);
    assert.equal(d["0.hosts"], "192.168.1.70-74");
    assert.equal(d["0.lightsPerHost"], "100");
    const n = JSON.parse(files["/.config/Network.json"]);
    assert.equal(n["2.addressing"], 1);                // multicast + broadcast
    assert.equal(n["3.hosts"], "192.168.1.147");
});

test("an E1.31 multicast output becomes E1.31, flagged to set its addressing", () => {
    const drivers = { "0.type": "NetworkSendDriver", "0.protocol": 3 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(drivers) });
    assert.equal(JSON.parse(files["/.config/Drivers.json"])["0.protocol"], 1);
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("addressing")));
});
