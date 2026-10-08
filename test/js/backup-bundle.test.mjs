// The backup/restore engine core (src/ui/migrate.js): the filesystem walk that builds a
// bundle, the dirs-before-files restore ordering, and the restore report that names what an old
// config loses on new firmware. Pure functions with injected I/O, so these run with mocks.
// Run: `node --test test/js/backup-bundle.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { collectFiles, restoreDirs, diffRestore } from "../../src/ui/migrate.js";

const tree = {
    "/": [{ name: ".config", isDir: true, size: 0 }, { name: "scripts", isDir: true, size: 0 }],
    "/.config": [{ name: "Network.json", isDir: false, size: 15 },
                 { name: "presets", isDir: true, size: 0 }],
    "/.config/presets": [{ name: "p1.json", isDir: false, size: 9 }],
    "/scripts": [{ name: "a.mle", isDir: false, size: 7 }],
};
const content = {
    "/.config/Network.json": '{"ssid":"abc"}\n',
    "/.config/presets/p1.json": '{"x":123}',
    "/scripts/a.mle": "let x=1",
};

test("the walk collects every file at every depth, byte-verified", async () => {
    const { files, skipped } = await collectFiles(async d => tree[d] || [], async p => content[p]);
    assert.deepEqual(Object.keys(files).sort(), Object.keys(content).sort());
    assert.equal(files["/scripts/a.mle"], "let x=1");
    assert.deepEqual(skipped, []);
});

test("a non-text file is skipped and named, not archived mangled", async () => {
    // Reading binary as UTF-8 inflates it (replacement characters), so bytes > listed size.
    const t = { ...tree, "/scripts": [...tree["/scripts"], { name: "fw.bin", isDir: false, size: 3 }] };
    const c = { ...content, "/scripts/fw.bin": "\uFFFD\uFFFD\uFFFD" };   // 9 bytes for a 3-byte file
    const { files, skipped } = await collectFiles(async d => t[d] || [], async p => c[p]);
    assert.deepEqual(skipped, ["/scripts/fw.bin"]);
    assert.equal(files["/scripts/fw.bin"], undefined);
    assert.deepEqual(Object.keys(files).sort(), Object.keys(content).sort());   // the rest intact
});

test("a truncated read fails the backup loudly instead of archiving it", async () => {
    const bad = { ...content, "/scripts/a.mle": "let " };   // 4 bytes, listing says 7
    await assert.rejects(
        collectFiles(async d => tree[d] || [], async p => bad[p]),
        /truncated/);
});

test("the HLS segment dir is transient encoder output, never backed up", async () => {
    const t2 = {
        "/": [{ name: ".hls", isDir: true, size: 0 }, { name: "ok.json", isDir: false, size: 2 }],
        "/.hls": [{ name: "stream.m3u8", isDir: false, size: 5 }],
    };
    const { files, skipped } = await collectFiles(async d => t2[d] || [], async () => "ok");
    assert.deepEqual(Object.keys(files), ["/ok.json"]);   // segments neither archived...
    assert.deepEqual(skipped, []);                        // ...nor noise in the report
});

test("a mid-walk rewrite re-lists once and archives the fresh bytes", async () => {
    // The device's debounced autosave can rewrite a file between listing and read: the walk
    // re-lists and re-reads once, so the grown file is archived, not misread as non-text.
    let listCalls = 0;
    const t2 = {
        "/": [{ name: "grow.json", isDir: false, size: 4 }],
    };
    const fetchDir = async d => {
        listCalls++;
        if (listCalls > 1) return [{ name: "grow.json", isDir: false, size: 9 }];
        return t2[d] || [];
    };
    const { files, skipped } = await collectFiles(fetchDir, async () => "grown-new", "/");
    assert.deepEqual(skipped, []);
    assert.equal(files["/grow.json"], "grown-new");
});

test("a file that fails to read is skipped and named; the rest still archive", async () => {
    const t2 = { "/": [
        { name: "ok.json", isDir: false, size: 2 },
        { name: "gone.json", isDir: false, size: 5 },
    ] };
    const { files, skipped } = await collectFiles(
        async d => t2[d] || [],
        async p => { if (p === "/gone.json") throw Error("404"); return "ok"; });
    assert.deepEqual(skipped, ["/gone.json"]);
    assert.deepEqual(Object.keys(files), ["/ok.json"]);
});

test("siblings of one type are matched by name and checked against their own controls", () => {
    // Two script modules of the same type publish DIFFERENT controls; the diff matches each child by its name, as the device applies it.
    const state = [{ name: "Effects", type: "Effects", controls: [], children: [
        { name: "Script", type: "MoonLiveEffect", controls: [{ name: "script" }, { name: "speed" }] },
        { name: "Script-2", type: "MoonLiveEffect", controls: [{ name: "script" }, { name: "cols" }] },
    ] }];
    const doc = (second) => JSON.stringify({ Effects: { "$patch": "replace", enabled: true,
        Script: { type: "MoonLiveEffect", speed: 3 },
        "Script-2": { type: "MoonLiveEffect", ...second } } });
    assert.deepEqual(diffRestore({ "/.config/Effects.json": doc({ cols: 8 }) }, state), []);
    // A control that is real on the first sibling only is reported on the second.
    const r = diffRestore({ "/.config/Effects.json": doc({ speed: 3 }) }, state);
    assert.equal(r.length, 1);
    assert.equal(r[0].kind, "control");
    assert.match(r[0].where, /Script-2\.speed/);
});

test("restore creates parent directories before children, each once", () => {
    const dirs = restoreDirs({
        "/.config/presets/p1.json": "", "/.config/Network.json": "", "/scripts/a.mle": "",
    });
    assert.deepEqual(dirs, ["/.config", "/scripts", "/.config/presets"]);
});

const liveState = [
    { name: "Network", type: "NetworkModule", controls: [{ name: "ssid" }, { name: "password" }] },
    { name: "Effects", type: "Effects", controls: [{ name: "enabled" }],
      children: [{ name: "Noise", type: "NoiseEffect", controls: [{ name: "speed" }] }] },
];

test("the report names a module that this firmware no longer has", () => {
    const r = diffRestore({ "/.config/OldDriver.json": '{"OldDriver":{"enabled":true,"a":1}}' }, liveState);
    assert.equal(r.length, 1);
    assert.equal(r[0].kind, "module");
    assert.match(r[0].detail, /module OldDriver does not exist/);
});

test("the report names a control that no longer exists, defaults noted", () => {
    const cfg = JSON.stringify({ Network: { "$patch": "replace", ssid: "x", enabled: true, oldKnob: 3 } });
    const r = diffRestore({ "/.config/NetworkModule.json": cfg }, liveState);
    assert.equal(r.length, 1);
    assert.equal(r[0].kind, "control");
    assert.match(r[0].detail, /oldKnob/);
    assert.match(r[0].detail, /back at the default/);
});

test("a control gone from a matched child is reported on that child", () => {
    const cfg = JSON.stringify({ Effects: { Noise: { type: "NoiseEffect", speed: 5, oldKnob: 1 } } });
    const r = diffRestore({ "/.config/Effects.json": cfg }, liveState);
    assert.equal(r.length, 1);
    assert.equal(r[0].kind, "control");
    assert.match(r[0].where, /Effects\.Noise\.oldKnob/);
    assert.match(r[0].detail, /does not exist on NoiseEffect/);
});

test("an unknown child type is reported once, not per key", () => {
    const cfg = JSON.stringify({ Effects: {
        Noise: { type: "NoiseEffect", speed: 5 },
        Gone: { type: "GoneEffect", a: 1, b: 2 },
    } });
    const r = diffRestore({ "/.config/Effects.json": cfg }, liveState);
    assert.equal(r.length, 1);                       // GoneEffect once, not per key
    assert.equal(r[0].kind, "module");
    assert.match(r[0].detail, /GoneEffect/);
});

test("a fully modern config yields an empty report; preset payloads are skipped", () => {
    const r = diffRestore({
        "/.config/NetworkModule.json": '{"Network":{"ssid":"x","password":"y"}}',
        "/.config/presets/p1.json": '{"whatever":"Layers"}',
    }, liveState);
    assert.deepEqual(r, []);
});

test("a child the live tree does not hold yet has its type checked and its controls left alone", () => {
    const files = { "/.config/Services.json": JSON.stringify({ Services: { Audio: { type: "AudioService", gain: 5 } } }) };
    const state = [{ name: "Services", type: "Services", controls: [], children: [] }];
    assert.deepEqual(diffRestore(files, state, ["Services", "AudioService"]), []);
    // without the registry entry the same file reports the missing child type
    const rep = diffRestore(files, state, ["Services"]);
    assert.equal(rep.length, 1);
    assert.match(rep[0].detail, /AudioService does not exist/);
});
