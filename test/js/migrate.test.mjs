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

// The document a migrated config file holds: its one root key is the module, its value the module's node.
const docOf = (files, path) => JSON.parse(files[path]);
const rootOf = (files, path) => Object.values(docOf(files, path))[0];
const kids = (node, type) => Object.values(node).filter(v => v && typeof v === "object" && v.type === type);
const NETWORK_LIVE = [{ name: "Network", type: "NetworkModule", controls: [],
                        children: [{ name: "Ethernet", type: "EthernetModule", controls: [] },
                                   { name: "WiFi", type: "WiFiModule", controls: [] }] }];

test("Network's Ethernet controls move onto its Ethernet child, which starts with the shared IP settings", () => {
    const cfg = { enabled: true, mDNS: true, ethBoard: "P4-NANO", ethType: 2, addressing: 1, ip: "192.168.1.9",
                  "0.type": "ImprovProvisioningModule", "1.type": "DevicesModule" };
    const { files, report } = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify(cfg) }, NETWORK_LIVE);
    const doc = docOf(files, "/.config/NetworkModule.json");
    assert.deepEqual(Object.keys(doc), ["Network"]);
    const net = doc.Network;
    // The backup predates the child, so it joins the existing children rather than displacing one.
    assert.equal(net.Ethernet.type, "EthernetModule");
    assert.equal(net.Ethernet.ethBoard, "P4-NANO");
    assert.equal(net.Ethernet.ethType, 2);
    assert.equal(net.ethBoard, undefined);
    // The wired interface starts with the IP settings the device had; with no network, Network keeps none.
    assert.equal(net.Ethernet.ipSettings, 1);
    assert.equal(net.Ethernet.ip, "192.168.1.9");
    assert.equal(net.addressing, undefined);
    assert.equal(net.mDNS, true);
    assert.ok(report.some(r => r.detail.includes("moved to EthernetModule")));
    // A backup without a network keeps no empty WiFi row, and says what its network keys came to.
    assert.equal(net.WiFi, undefined);
    assert.ok(report.some(r => r.kind === "review" && r.detail.startsWith("dropped: no ssid")));
    // A WiFi-only backup gains no Ethernet entry from its IP settings alone.
    const wifiOnly = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify({ enabled: true, ssid: "home", addressing: 0 }) }, NETWORK_LIVE);
    const w = rootOf(wifiOnly.files, "/.config/NetworkModule.json");
    assert.equal(kids(w, "WiFiModule").length, 1);
    assert.equal(kids(w, "EthernetModule").length, 0);
    // A backup without Ethernet settings keeps its controls and gains no child.
    const plain = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify({ enabled: true, mDNS: false }) }, NETWORK_LIVE);
    assert.deepEqual(docOf(plain.files, "/.config/NetworkModule.json"), { Network: { "$patch": "replace", enabled: true, mDNS: false } });
});

test("Network's one WiFi network becomes the first known network of its WiFi child", () => {
    const cfg = { enabled: true, ssid: "home", password: "pw", addressing: 1, ip: "10.0.0.7", txPowerSetting: 8, mDNS: true, ethBoard: "P4-NANO" };
    const { files } = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify(cfg) }, NETWORK_LIVE);
    const net = rootOf(files, "/.config/NetworkModule.json");
    assert.equal(net.WiFi.type, "WiFiModule");
    assert.deepEqual(net.WiFi.known, [{ id: 1, ssid: "home", password: "pw", ipSettings: 1, ip: "10.0.0.7" }]);
    // The wired interface starts with the same IP settings, and Network keeps none of its own.
    assert.equal(net.Ethernet.ipSettings, 1);
    assert.equal(net.addressing, undefined);
    assert.equal(net.ip, undefined);
    assert.equal(net.WiFi.txPowerSetting, 8);
    assert.equal(net.ssid, undefined);
    assert.equal(net.password, undefined);
    assert.equal(net.mDNS, true);
});

test("a driver's preset control migrates to fixture with its value", () => {
    const cfg = { enabled: true, "0.type": "RmtLedDriver", "0.preset": 2, "0.enabled": true };
    const { files } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const [drv] = kids(rootOf(files, "/.config/Drivers.json"), "RmtLedDriver");
    assert.equal(drv.fixture, 2);
    assert.equal(drv.preset, undefined);
    assert.equal(drv.enabled, true);
});

test("light presets are fixture profiles: type, list key and the driver's Select all map", () => {
    const lib = { enabled: true, "0.type": "LightPresetsModule", "0.presets": [{ id: 1, name: "GRB" }] };
    const out1 = applyMigrations({ "/.config/Drivers.json": JSON.stringify({ ...lib, "1.type": "RmtLedDriver", "1.lightPreset": 3, "1.presetRef": "RGBW" }) });
    const root = rootOf(out1.files, "/.config/Drivers.json");
    assert.equal(kids(root, "LightPresetsModule").length, 0);
    const [profiles] = kids(root, "FixtureProfilesModule");
    assert.deepEqual(profiles.profiles, [{ id: 1, name: "GRB" }]);
    assert.equal(profiles.presets, undefined);
    const [drv] = kids(root, "RmtLedDriver");
    assert.equal(drv.fixture, 3);
    assert.equal(drv.fixtureRef, "RGBW");
    assert.equal(drv.lightPreset, undefined);
    // A `presets` key on any other module (ControlModule's own) is not touched.
    const other = applyMigrations({ "/.config/Control.json": JSON.stringify({ enabled: true, presets: 1 }) });
    assert.equal(rootOf(other.files, "/.config/Control.json").presets, 1);
});

test("a bundle carrying both old and new names reports the collision, never silent", () => {
    const cfg = { enabled: true, "0.type": "PreviewDriver", "0.fps": 24, "0.targetFps": 30 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const [drv] = kids(rootOf(files, "/.config/Drivers.json"), "PreviewDriver");
    assert.equal(drv.targetFps, 30);   // last write wins (plain key iterated after the rename)...
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
    assert.equal(rootOf(files, "/.config/Layouts.json").enabled, false);   // values survive the rename
    assert.ok(report.some(r => r.kind === "renamed" && r.detail.includes("Effects.json")));
});

test("renamed module types map in place, including nested children", () => {
    const cfg = { enabled: true, "0.type": "Layers", "0.0.type": "ParlioLedDriver", "0.0.enabled": true };
    const { files, report } = applyMigrations({ "/.config/Effects.json": JSON.stringify(cfg) });
    const [container] = kids(rootOf(files, "/.config/Effects.json"), "Effects");
    const [drv] = kids(container, "ParallelLedDriver");
    assert.ok(drv);
    // Parlio kept its peripheral value, so the map SETS it rather than asking for review.
    assert.equal(drv.peripheral, "Parlio");
    assert.ok(report.filter(r => r.kind === "renamed").length >= 2);
});

test("chip-dependent peripherals get a review entry, never a guessed value", () => {
    const cfg = { enabled: true, "0.type": "I80LedDriver", "0.pin0": 16 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const [drv] = kids(rootOf(files, "/.config/Drivers.json"), "ParallelLedDriver");
    assert.ok(drv);
    assert.equal(drv.peripheral, undefined);   // not guessed
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("re-picked")));
});

test("a deterministic peripheral move is set: MoonLedDriver becomes ParallelLedDriver on LCD-MM", () => {
    const cfg = { enabled: true, "0.type": "MoonLedDriver", "0.useRing": true };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const [drv] = kids(rootOf(files, "/.config/Drivers.json"), "ParallelLedDriver");
    assert.equal(drv.peripheral, "LCD-MM");
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("S3/P4 only")));
});

test("control VALUES migrate too: peripheral MoonI80 maps to LCD-MM, i80 flags review", () => {
    const cfg = { enabled: true, "0.type": "ParallelLedDriver", "0.peripheral": "MoonI80",
                  "1.type": "ParallelLedDriver", "1.peripheral": "i80" };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const [first, second] = kids(rootOf(files, "/.config/Drivers.json"), "ParallelLedDriver");
    assert.equal(first.peripheral, "LCD-MM");                 // deterministic: mapped
    assert.equal(second.peripheral, "i80");                   // chip-dependent: untouched...
    assert.ok(report.some(r => r.kind === "review" && r.where.includes("1.peripheral")));   // ...but flagged
    assert.ok(report.some(r => r.kind === "renamed" && r.detail.includes("MoonI80 → LCD-MM")));
});

test("control renames apply, and semantics changes flag review instead of guessing", () => {
    const cfg = { enabled: true, "0.type": "PreviewDriver", "0.fps": 30, "1.type": "MoonLedDriver", "1.forceRing": 2 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const root = rootOf(files, "/.config/Drivers.json");
    const [preview] = kids(root, "PreviewDriver");
    assert.equal(preview.targetFps, 30);
    assert.equal(preview.fps, undefined);
    const [parallel] = kids(root, "ParallelLedDriver");   // the type rename comes first, and scopes the control rename
    assert.equal(parallel.useRing, 2);                    // name maps; value untouched...
    assert.ok(report.some(r => r.kind === "review" && r.where.includes("useRing")));   // ...but flagged
});

test("renames are scoped by module type: NetworkSendDriver keeps its own fps", () => {
    // The bench-found bug: fps → targetFps belongs to PreviewDriver alone; other drivers
    // legitimately own an fps control today and a blanket rename corrupts them.
    const cfg = { enabled: true, "0.type": "NetworkSendDriver", "0.fps": 60, "1.type": "PreviewDriver", "1.fps": 24 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(cfg) });
    const root = rootOf(files, "/.config/Drivers.json");
    assert.equal(kids(root, "NetworkSendDriver")[0].fps, 60);        // untouched
    assert.equal(kids(root, "PreviewDriver")[0].targetFps, 24);      // renamed
    // The one rename, plus the conversion of the file itself.
    assert.deepEqual(report.filter(r => !r.detail.startsWith("config →")).map(r => r.detail.slice(0, 18)), ["fps → targetFps (2"]);
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
            "enabled": true,
            "0.type": "FishTankEffect",
            "0.soundReactive": true,
            "0.speed": 42,
        }),
    };
    const { files: out } = applyMigrations(files);
    const [fish] = kids(rootOf(out, "/.config/Effects.json"), "FishTankEffect");
    assert.equal(fish.audioReactive, true, "the value must land on the new name");
    assert.ok(!("soundReactive" in fish), "the old key must be gone");
    assert.equal(fish.speed, 42, "unrelated controls are untouched");
});

// Scoped, per the rule the table documents: the same word on a module that never declared it is
// somebody else's control and must not be rewritten.
test("soundReactive is left alone on a module outside its scope", () => {
    const files = {
        "/.config/Effects.json": JSON.stringify({
            "type": "Effects",
            "enabled": true,
            "0.type": "NoiseEffect",
            "0.soundReactive": true,
        }),
    };
    const { files: out } = applyMigrations(files);
    const [noise] = kids(rootOf(out, "/.config/Effects.json"), "NoiseEffect");
    assert.equal(noise.soundReactive, true);
    assert.ok(!("audioReactive" in noise));
});

test("the receiver lists become hosts, and discovery's WLED switch becomes its addressing", () => {
    const drivers = { enabled: true, "0.type": "NetworkSendDriver", "0.ips": "192.168.1.70-74", "0.lightsPerIp": "100" };
    const network = { enabled: true, "0.type": "DevicesModule", "0.wledCompatible": true, "1.type": "OscModule", "1.feedbackTo": "192.168.1.147" };
    const { files } = applyMigrations({
        "/.config/Drivers.json": JSON.stringify(drivers),
        "/.config/Network.json": JSON.stringify(network),
    });
    const [send] = kids(rootOf(files, "/.config/Drivers.json"), "NetworkSendDriver");
    assert.equal(send.hosts, "192.168.1.70-74");
    assert.equal(send.lightsPerHost, "100");
    const net = rootOf(files, "/.config/Network.json");
    assert.equal(kids(net, "DevicesModule")[0].addressing, 1);                 // multicast + broadcast
    assert.equal(kids(net, "OscModule")[0].hosts, "192.168.1.147");
});

test("the MIDI desk's usb switch becomes its source, browser or USB", () => {
    const services = { enabled: true, "0.type": "MidiService", "0.usb": true, "1.type": "MidiService", "1.usb": false };
    const { files } = applyMigrations({ "/.config/Services.json": JSON.stringify(services) });
    const [usb, browser] = kids(rootOf(files, "/.config/Services.json"), "MidiService");
    assert.equal(usb.source, 1);       // USB
    assert.equal(browser.source, 0);   // browser
    assert.ok(!("usb" in usb));
});

test("an E1.31 multicast output becomes E1.31, flagged to set its addressing", () => {
    const drivers = { enabled: true, "0.type": "NetworkSendDriver", "0.protocol": 3 };
    const { files, report } = applyMigrations({ "/.config/Drivers.json": JSON.stringify(drivers) });
    assert.equal(kids(rootOf(files, "/.config/Drivers.json"), "NetworkSendDriver")[0].protocol, 1);
    assert.ok(report.some(r => r.kind === "review" && r.detail.includes("addressing")));
});

test("a legacy network joins a WiFi child's saved known list rather than replacing it", () => {
    const cfg = { enabled: true, ssid: "old", password: "pw", "0.type": "WiFiModule", "0.known": [{ id: 4, ssid: "saved", password: "x" }] };
    const { files } = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify(cfg) });
    const [wifi] = kids(rootOf(files, "/.config/NetworkModule.json"), "WiFiModule");
    assert.deepEqual(wifi.known.map(r => [r.id, r.ssid]), [[5, "old"], [4, "saved"]]);
    // The same network already saved is kept as saved, and the report says so.
    const dup = applyMigrations({ "/.config/NetworkModule.json": JSON.stringify({ enabled: true, ssid: "saved", "0.type": "WiFiModule", "0.known": [{ id: 4, ssid: "saved", password: "x" }] }) });
    const [kept] = kids(rootOf(dup.files, "/.config/NetworkModule.json"), "WiFiModule");
    assert.deepEqual(kept.known, [{ id: 4, ssid: "saved", password: "x" }]);
    assert.ok(dup.report.some(r => r.kind === "review" && r.detail.includes("already has saved")));
});

test("a flat config becomes the module's document, its code-wired child named as the live tree names it", () => {
    const live = [{ name: "MoonCloud", type: "MoonCloudModule", controls: [{ name: "url" }],
                    children: [{ name: "Stats", type: "MoonStatsModule", controls: [{ name: "interval" }] }] }];
    const flat = { enabled: true, url: "https://cloud.example", "0.type": "MoonStatsModule", "0.interval": 5 };
    const { files, report } = applyMigrations({ "/.config/MoonCloudModule.json": JSON.stringify(flat) }, live);
    assert.deepEqual(docOf(files, "/.config/MoonCloudModule.json"), {
        MoonCloud: {
            "$patch": "replace", url: "https://cloud.example", enabled: true,
            Stats: { type: "MoonStatsModule", "$patch": "replace", interval: 5 },
        },
    });
    assert.ok(report.some(r => r.kind === "renamed" && r.detail === "config → state document (2026-10-08)"));
    // Without a live tree the names fall back to the type without its role noun.
    const bare = applyMigrations({ "/.config/MoonCloudModule.json": JSON.stringify(flat) });
    assert.deepEqual(Object.keys(docOf(bare.files, "/.config/MoonCloudModule.json")), ["MoonCloud"]);
    assert.ok(rootOf(bare.files, "/.config/MoonCloudModule.json").MoonStats);
});

test("a saved $name names its child, and unnamed children of one type become X and X-2", () => {
    const flat = { enabled: true,
                   "0.type": "NoiseEffect", "0.$name": "Left", "0.speed": 1,
                   "1.type": "NoiseEffect", "1.speed": 2,
                   "2.type": "NoiseEffect", "2.speed": 3 };
    const { files } = applyMigrations({ "/.config/Effects.json": JSON.stringify(flat) });
    const root = rootOf(files, "/.config/Effects.json");
    assert.deepEqual(Object.keys(root).filter(k => !["$patch", "enabled"].includes(k)), ["Left", "Noise", "Noise-2"]);
    assert.equal(root.Left.speed, 1);
    assert.equal(root.Left["$name"], undefined);
    assert.equal(root.Noise.speed, 2);
    assert.equal(root["Noise-2"].speed, 3);
});

test("a config that is already a state document passes through byte-exact, unreported", () => {
    const doc = '{"Effects":{"$patch":"replace","enabled":true,"Noise":{"type":"NoiseEffect","$patch":"replace","speed":3}}}';
    const { files, report } = applyMigrations({ "/.config/Effects.json": doc });
    assert.equal(files["/.config/Effects.json"], doc);
    assert.deepEqual(report, []);
});
