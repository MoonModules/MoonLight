// Improv-serial frame building — the pure, dependency-free wire-format core.
//
// Extracted from install-orchestrator.js so it's importable in node:test without
// pulling the orchestrator's browser-only unpkg imports (esptool-js, the Improv
// SDK). The orchestrator imports these for the actual send; test/js imports them
// to pin the byte layout. This is the SAME wire format three implementations must
// agree on: device C++ (src/core/ImprovFrame.h + platform_esp32_improv.cpp),
// Python (moondeck/build/improv_provision.py), and this file. The frame-contract
// tests (test/js, test/python) assert a shared golden vector so they can't drift.
//
// Frame layout (matches src/core/ImprovFrame.h):
//   [I][M][P][R][O][V][version=1][type][length][payload×length][checksum]
//   checksum = sum-mod-256 of the first 9+length bytes.

// SET_TX_POWER vendor RPC command ID: the pre-association TX-power cap for devices
// whose LDO browns out at full power. Sent BEFORE provisioning so the very first
// association runs capped. Matches the device-side handler.
export const IMPROV_CMD_SET_TX_POWER = 0xFD;

// APPLY_OP vendor RPC command ID: "Improv = REST over serial". Carries ONE state document
// as JSON (the body PATCH /api/state takes). Frame payload: [0xFC][seq][last][chunk…].
// Matches improvHandleApplyOp at src/platform/esp32/platform_esp32_improv.cpp.
export const IMPROV_CMD_APPLY_OP = 0xFC;

// Max op-JSON bytes per frame: the device's kImprovMaxPayload (128) minus the
// 3-byte [cmd][seq][last] header. A longer op (a big pins list) chunks.
export const APPLY_OP_CHUNK_MAX = 128 - 3;

// Improv frame type for RPC commands (matches src/core/ImprovFrame.h).
export const IMPROV_FRAME_TYPE_RPC = 0x03;

// Magic bytes that prefix every Improv frame. ASCII "IMPROV".
export const IMPROV_MAGIC = [0x49, 0x4d, 0x50, 0x52, 0x4f, 0x56];

// Wrap a payload in Improv framing. Returns a Uint8Array:
//   IMPROV + version(1) + type + length + payload + checksum(sum-mod-256).
export function buildImprovFrame(type, payload) {
    const len = payload.length;
    // The length is a single byte on the wire — a payload over 255 would truncate
    // silently and emit a corrupt frame. Throw instead. (Callers chunk well under
    // the device's 128-byte kImprovMaxPayload, so this never fires in practice; it
    // guards a future caller, matching ImprovFrame.h's oversize-payload rejection.)
    if (len > 255) throw new Error(`Improv payload length ${len} exceeds 255 (one-byte length field)`);
    const frame = new Uint8Array(6 + 1 + 1 + 1 + len + 1);
    frame.set(IMPROV_MAGIC, 0);
    frame[6] = 0x01;   // version
    frame[7] = type;
    frame[8] = len;
    frame.set(payload, 9);
    let sum = 0;
    for (let i = 0; i < 9 + len; i++) sum = (sum + frame[i]) & 0xff;
    frame[9 + len] = sum;
    return frame;
}

// The documents a catalog entry sends: one {"<Root>": <its object>} per top-level container
// of the entry's `state`, as JSON strings in the entry's key order. Each fits the device's
// 512-byte reassembly buffer where the whole state would not.
export function stateFrames(entry) {
    const state = entry && entry.state;
    if (!state || typeof state !== "object" || Array.isArray(state)) return [];
    // Drivers goes last: starting an output prints on UART0, which a classic board shares with Improv, and LED pins 1 or 3 end serial reception.
    return Object.entries(state)
        .sort(([a], [b]) => (a === "Drivers") - (b === "Drivers"))
        .map(([root, body]) => JSON.stringify({ [root]: body }));
}

// Encode ONE state document (an object, or its JSON string) into the APPLY_OP frame(s) it
// sends over serial. The JSON is UTF-8'd and split into APPLY_OP_CHUNK_MAX-byte chunks; each chunk becomes a
// frame with payload [0xFC][seq][last][chunk…]. Always at least one frame (so
// `last` always sends, even for an empty document). The device reassembles by seq and
// applies on `last=1`. Returns an array of Uint8Array frames, in send order.
export function encodeApplyOpFrames(doc) {
    const bytes = new TextEncoder().encode(typeof doc === "string" ? doc : JSON.stringify(doc));
    const total = bytes.length;
    const chunks = Math.max(1, Math.ceil(total / APPLY_OP_CHUNK_MAX));
    const frames = [];
    for (let seq = 0; seq < chunks; seq++) {
        const start = seq * APPLY_OP_CHUNK_MAX;
        const slice = bytes.subarray(start, start + APPLY_OP_CHUNK_MAX);
        const last = seq === chunks - 1 ? 1 : 0;
        const payload = new Uint8Array(3 + slice.length);
        payload[0] = IMPROV_CMD_APPLY_OP;
        payload[1] = seq & 0xFF;
        payload[2] = last;
        payload.set(slice, 3);
        frames.push(buildImprovFrame(IMPROV_FRAME_TYPE_RPC, payload));
    }
    return frames;
}

// The device's replies: an RPC result per acknowledged chunk, an error state when it refuses one.
export const IMPROV_FRAME_TYPE_ERROR = 0x02;
export const IMPROV_FRAME_TYPE_RPC_RESULT = 0x04;
// The device is still applying the previous document, or a chunk arrived out of order: send the document again from its first chunk.
export const IMPROV_ERROR_OP_REFUSED = 0x82;

// The Improv frames in bytes that also carry log text: each whole frame whose checksum holds, and the bytes to keep for the next read (an incomplete frame, or the start of a magic).
export function parseImprovFrames(bytes) {
    const frames = [];
    let i = 0;
    for (;;) {
        let p = i;
        while (p + IMPROV_MAGIC.length <= bytes.length && !IMPROV_MAGIC.every((m, k) => bytes[p + k] === m)) p++;
        if (p + IMPROV_MAGIC.length > bytes.length) return { frames, rest: bytes.slice(Math.max(i, bytes.length - IMPROV_MAGIC.length + 1)) };
        if (p + 9 > bytes.length || p + 10 + bytes[p + 8] > bytes.length) return { frames, rest: bytes.slice(p) };
        const len = bytes[p + 8];
        let sum = 0;
        for (let k = 0; k < 9 + len; k++) sum = (sum + bytes[p + k]) & 0xff;
        if (sum === bytes[p + 9 + len]) {
            frames.push({ type: bytes[p + 7], payload: bytes.slice(p + 9, p + 9 + len) });
            i = p + 10 + len;
        } else {
            i = p + 1;   // a magic inside log text, or a damaged frame
        }
    }
}

// Improv's "unknown RPC command": firmware without the operation, the one error a resend cannot fix.
export const IMPROV_ERROR_UNKNOWN_RPC = 0x02;

// What a reply frame says about an APPLY_OP chunk: "ack", "unsupported" (firmware without the operation), "refused" for any other error (send again: the device is busy, or an error state left from provisioning arrived first), or null for any other frame.
export function applyOpReply(frame) {
    if (frame.type === IMPROV_FRAME_TYPE_RPC_RESULT && frame.payload[0] === IMPROV_CMD_APPLY_OP) return "ack";
    if (frame.type !== IMPROV_FRAME_TYPE_ERROR || !frame.payload[0]) return null;
    return frame.payload[0] === IMPROV_ERROR_UNKNOWN_RPC ? "unsupported" : "refused";
}
