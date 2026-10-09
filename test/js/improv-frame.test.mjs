// Improv frame-contract tests — pin the wire format the device C++
// (src/core/ImprovFrame.h), Python (moondeck/build/improv_provision.py), and the
// installer JS (mooninstaller/improv-frame.js) must all agree on byte-for-byte.
// The golden vectors here are asserted identically in test/python/test_improv_frame.py
// so the JS and Python builders can't drift; they're hand-verified against the C++
// checksum (sum-mod-256) too. Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import {
    buildImprovFrame,
    encodeApplyOpFrames,
    stateFrames,
    APPLY_OP_CHUNK_MAX,
    IMPROV_CMD_APPLY_OP,
    IMPROV_FRAME_TYPE_RPC,
    IMPROV_MAGIC,
} from "../../mooninstaller/improv-frame.js";

const hex = (u8) => Array.from(u8).map((b) => b.toString(16).padStart(2, "0")).join(" ");
const bytes = (s) => Array.from(new TextEncoder().encode(s));

test("frame layout: magic, version, type, length, payload, checksum", () => {
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, new Uint8Array([0x01]));
    assert.deepEqual(Array.from(frame.subarray(0, 6)), IMPROV_MAGIC, "magic");
    assert.equal(frame[6], 0x01, "version");
    assert.equal(frame[7], IMPROV_FRAME_TYPE_RPC, "type");
    assert.equal(frame[8], 1, "length");
    assert.equal(frame[9], 0x01, "payload");
    assert.equal(frame.length, 11, "total = 9 header + 1 payload + 1 checksum");
});

test("checksum is sum-mod-256 of the first 9+length bytes", () => {
    const payload = new Uint8Array([0xAA, 0xBB, 0xCC]);
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, payload);
    let sum = 0;
    for (let i = 0; i < frame.length - 1; i++) sum = (sum + frame[i]) & 0xff;
    assert.equal(frame[frame.length - 1], sum);
});

test("golden vector G1: buildImprovFrame(RPC, [0x01])", () => {
    // Shared with test/python G1. Hand-verified checksum 0xe3.
    const frame = buildImprovFrame(IMPROV_FRAME_TYPE_RPC, new Uint8Array([0x01]));
    assert.equal(hex(frame), "49 4d 50 52 4f 56 01 03 01 01 e3");
});

test("golden vector G2: a small state document is a single APPLY_OP frame", () => {
    const op = { Layouts: { Grid: { type: "GridLayout", width: 8 } } };
    const frames = encodeApplyOpFrames(op);
    assert.equal(frames.length, 1, "fits one frame");
    const f = frames[0];
    assert.equal(f[7], IMPROV_FRAME_TYPE_RPC);
    assert.equal(f[9 + 0], IMPROV_CMD_APPLY_OP, "payload[0] = 0xFC");
    assert.equal(f[9 + 1], 0, "seq");
    assert.equal(f[9 + 2], 1, "last");
    // payload after the 3-byte header is the document JSON, byte-identical
    assert.deepEqual(Array.from(f.subarray(9 + 3, 9 + f[8])), bytes(JSON.stringify(op)));
});

test("golden vector G3: a >125-byte document chunks into ordered frames", () => {
    const op = { Drivers: { X: { type: "RmtLedDriver", pins: "1".repeat(140) } } };
    const json = JSON.stringify(op);
    assert.ok(new TextEncoder().encode(json).length > APPLY_OP_CHUNK_MAX, "forces >1 chunk");
    const frames = encodeApplyOpFrames(op);
    assert.equal(frames.length, 2);
    // frame 0: seq 0, last 0, full chunk
    assert.equal(frames[0][9 + 1], 0, "f0 seq");
    assert.equal(frames[0][9 + 2], 0, "f0 not-last");
    assert.equal(frames[0][8] - 3, APPLY_OP_CHUNK_MAX, "f0 carries a full chunk");
    // frame 1: seq 1, last 1, remainder
    assert.equal(frames[1][9 + 1], 1, "f1 seq");
    assert.equal(frames[1][9 + 2], 1, "f1 last");
    // reassembling the chunks reproduces the document JSON exactly
    let reassembled = [];
    for (const f of frames) reassembled.push(...f.subarray(9 + 3, 9 + f[8]));
    assert.deepEqual(reassembled, bytes(json));
});

test("APPLY_OP always emits at least one frame (so `last` always sends)", () => {
    const frames = encodeApplyOpFrames({});
    assert.equal(frames.length, 1);
    assert.equal(frames[0][9 + 2], 1, "last=1 on the lone frame");
});

test("an already-serialized document is framed as given", () => {
    const json = '{"System":{"deviceModel":"x"}}';
    assert.deepEqual(encodeApplyOpFrames(json), encodeApplyOpFrames(JSON.parse(json)));
});

const ENTRY = { state: {
    System: { deviceModel: "x" },
    Layouts: { $patch: "replace", Grid: { type: "GridLayout", width: 8 } },
    Drivers: { $patch: "replace", RmtLed: { type: "RmtLedDriver", pins: "16" } },
} };

test("an entry sends one document per top-level container, in key order", () => {
    const docs = stateFrames(ENTRY).map((j) => JSON.parse(j));
    assert.deepEqual(docs.map((d) => Object.keys(d)[0]), ["System", "Layouts", "Drivers"]);
    assert.ok(docs.every((d) => Object.keys(d).length === 1));
});

// Starting an output prints on UART0, which a classic board shares with Improv, so a document after Drivers could lose its acknowledgement.
test("Drivers goes last whatever the entry's key order", () => {
    const entry = { state: { Drivers: ENTRY.state.Drivers, System: ENTRY.state.System, Layouts: ENTRY.state.Layouts } };
    assert.deepEqual(stateFrames(entry).map((j) => Object.keys(JSON.parse(j))[0]), ["System", "Layouts", "Drivers"]);
});

test("a document carries its container object unchanged", () => {
    const docs = stateFrames(ENTRY).map((j) => JSON.parse(j));
    assert.deepEqual(docs[2], { Drivers: ENTRY.state.Drivers });
});

test("an empty or malformed entry sends no documents", () => {
    for (const entry of [undefined, null, {}, { state: null }, { state: [] }, { state: "x" }]) {
        assert.deepEqual(stateFrames(entry), []);
    }
});

test("no catalog entry has a container document the device cannot reassemble (512 bytes)", async () => {
    const { readFileSync } = await import("node:fs");
    const catalog = JSON.parse(readFileSync(new URL("../../mooninstaller/deviceModels.json", import.meta.url), "utf8"));
    for (const entry of catalog) {
        for (const doc of stateFrames(entry)) {
            assert.ok(new TextEncoder().encode(doc).length <= 512, `entry "${entry.name}": ${doc.slice(0, 40)} exceeds 512 bytes`);
        }
    }
});
