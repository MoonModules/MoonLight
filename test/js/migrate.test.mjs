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

test("Network's Ethernet controls move onto its Ethernet child, which starts with the shared IP settings", () => {
    const cfg = { mDNS: true, ethBoard: "P4-NANO", ethType: 2, addressing: 1, ip: "192.168.1.9",
                  "0.type": "ImprovProvisioningModule", "1.type": "DevicesModule" };
    const { files, report } = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/NetworkModule.json"]);
    // The backup predates the child, so it gets the next free index rather than displacing one.
    assert.equal(out["2.type"], "EthernetModule");
    assert.equal(out["2.ethBoard"], "P4-NANO");
    assert.equal(out["2.ethType"], 2);
    assert.equal(out.ethBoard, undefined);
    // The wired interface starts with the IP settings the device had; with no network, Network keeps none.
    assert.equal(out["2.ipSettings"], 1);
    assert.equal(out["2.ip"], "192.168.1.9");
    assert.equal(out.addressing, undefined);
    assert.equal(out.mDNS, true);
    assert.ok(report.some(r => r.detail.includes("moved to EthernetModule")));
    // A backup without a network keeps no empty WiFi row, and says what its network keys came to.
    assert.equal(out["3.type"], undefined);
    assert.ok(report.some(r => r.kind === "review" && r.detail.startsWith("dropped: no ssid")));
    // A WiFi-only backup gains no Ethernet entry from its IP settings alone.
    const wifiOnly = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify({ ssid: "home", addressing: 0 }) });
    const w = JSON.parse(wifiOnly.files["/.config/NetworkModule.json"]);
    assert.equal(w["0.type"], "WiFiModule");
    assert.ok(!Object.values(w).includes("EthernetModule"));
    // A backup without Ethernet settings is left exactly as it was.
    const plain = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify({ mDNS: false }) });
    assert.deepEqual(JSON.parse(plain.files["/.config/NetworkModule.json"]), { mDNS: false });
});

test("Network's one WiFi network becomes the first known network of its WiFi child", () => {
    const cfg = { ssid: "home", password: "pw", addressing: 1, ip: "10.0.0.7", txPowerSetting: 8, mDNS: true, ethBoard: "P4-NANO" };
    const { files } = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/NetworkModule.json"]);
    assert.equal(out["1.type"], "WiFiModule");
    assert.deepEqual(out["1.known"], [{ id: 1, ssid: "home", password: "pw", ipSettings: 1, ip: "10.0.0.7" }]);
    // The wired interface starts with the same IP settings, and Network keeps none of its own.
    assert.equal(out["0.ipSettings"], 1);
    assert.equal(out.addressing, undefined);
    assert.equal(out.ip, undefined);
    assert.equal(out["1.txPowerSetting"], 8);
    assert.equal(out.ssid, undefined);
    assert.equal(out.password, undefined);
    assert.equal(out.mDNS, true);
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

test("a flat preset becomes a state document, each module named as the device names it", () => {
    const flat = {
        slot: 3, captures: "Layers",
        "Layers.enabled": true,
        "Layers.0.type": "Layer", "Layers.0.opacity": 200, "Layers.0.enabled": true,
        "Layers.0.0.type": "NoiseEffect", "Layers.0.0.scale": 4, "Layers.0.0.enabled": true,
        "Layers.1.type": "Layer", "Layers.1.enabled": false,
        "Layers.1.0.type": "NoiseEffect", "Layers.1.0.enabled": true,
    };
    const { files, report } = applyMigrations({ "/.config/presets/look.json": JSON.stringify(flat) });
    const doc = JSON.parse(files["/.config/presets/look.json"]);
    assert.deepEqual(doc, {
        $slot: 3,
        Effects: {
            "$patch": "replace", enabled: true,
            Layer: { type: "Layer", "$patch": "replace", opacity: 200, enabled: true,
                     Noise: { type: "NoiseEffect", "$patch": "replace", scale: 4, enabled: true } },
            "Layer-2": { type: "Layer", "$patch": "replace", enabled: false,
                         "Noise-2": { type: "NoiseEffect", "$patch": "replace", enabled: true } },
        },
    });
    // Key order is creation order on the device, so the document keeps the file's.
    assert.deepEqual(Object.keys(doc.Effects), ["$patch", "enabled", "Layer", "Layer-2"]);
    assert.ok(report.some(r => r.detail.includes("state document")));
});

test("a preset that is already a document passes through", () => {
    const doc = '{"Drivers":{"palette":"Ocean"}}';
    const { files } = applyMigrations({ "/.config/presets/ocean.json": doc });
    assert.equal(files["/.config/presets/ocean.json"], doc);
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

test("a legacy network joins a WiFi child's saved known list rather than replacing it", () => {
    const cfg = { ssid: "old", password: "pw", "0.type": "WiFiModule", "0.known": [{ id: 4, ssid: "saved", password: "x" }] };
    const { files, report } = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify(cfg) });
    const out = JSON.parse(files["/.config/NetworkModule.json"]);
    assert.deepEqual(out["0.known"].map(r => [r.id, r.ssid]), [[5, "old"], [4, "saved"]]);
    // The same network already saved is kept as saved, and the report says so.
    const dup = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify({ ssid: "saved", "0.type": "WiFiModule", "0.known": [{ id: 4, ssid: "saved", password: "x" }] }) });
    const kept = JSON.parse(dup.files["/.config/NetworkModule.json"]);
    assert.deepEqual(kept["0.known"], [{ id: 4, ssid: "saved", password: "x" }]);
    assert.ok(dup.report.some(r => r.kind === "review" && r.detail.includes("already has saved")));
});
