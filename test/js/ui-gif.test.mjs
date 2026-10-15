// The preview GIF a user saves for a preset: the file must open in any viewer, so the test decodes it back as a GIF89a reader would.

import { test } from "node:test";
import assert from "node:assert/strict";
import { encodeGif } from "../../src/ui/gif.js";

// A GIF89a reader, enough for the files encodeGif writes: the global palette and each frame's indices.
function decode(bytes) {
    let p = 0;
    const u8 = () => bytes[p++];
    const u16 = () => { const v = bytes[p] | (bytes[p + 1] << 8); p += 2; return v; };
    assert.equal(String.fromCharCode(...bytes.slice(0, 6)), "GIF89a");
    p = 6;
    const width = u16(), height = u16(), flags = u8();
    p += 2;
    const palette = [];
    for (let i = 0; i < 2 << (flags & 7); i++) palette.push([u8(), u8(), u8()]);
    const frames = [];
    let delays = [], loops = false;
    for (;;) {
        const b = u8();
        if (b === 0x3B) break;
        if (b === 0x21) {
            const label = u8();
            if (label === 0xF9) { p += 2; delays.push(u16()); p += 2; continue; }
            const size = u8();
            const tag = String.fromCharCode(...bytes.slice(p, p + size));
            p += size;
            if (tag === "NETSCAPE2.0") loops = true;
            for (let n = u8(); n; n = u8()) p += n;
            continue;
        }
        assert.equal(b, 0x2C);
        p += 8; u8();
        const min = u8();
        const data = [];
        for (let n = u8(); n; n = u8()) { data.push(...bytes.slice(p, p + n)); p += n; }
        frames.push(unlzw(data, min, width * height));
    }
    return { width, height, palette, frames, delays, loops };
}

function unlzw(data, min, count) {
    const clear = 1 << min, end = clear + 1;
    let size = min + 1, dict = [], prev = null;
    const reset = () => { dict = []; for (let i = 0; i < clear; i++) dict.push([i]); dict.push(null, null); size = min + 1; prev = null; };
    reset();
    const out = [];
    let bit = 0;
    const read = () => {
        let v = 0;
        for (let i = 0; i < size; i++, bit++) v |= ((data[bit >> 3] >> (bit & 7)) & 1) << i;
        return v;
    };
    while (out.length < count) {
        const code = read();
        if (code === clear) { reset(); continue; }
        if (code === end) break;
        let entry;
        if (code < dict.length && dict[code]) entry = dict[code];
        else entry = [...prev, prev[0]];
        out.push(...entry);
        if (prev) dict.push([...prev, entry[0]]);
        prev = entry;
        if (dict.length === (1 << size) && size < 12) size++;
    }
    return out;
}

function frame(width, height, colorAt) {
    const f = new Uint8ClampedArray(width * height * 4);
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
        const [r, g, b] = colorAt(x, y);
        f.set([r, g, b, 255], (y * width + x) * 4);
    }
    return f;
}

test("a saved preview loops, keeps its frame timing, and every pixel decodes to its color", () => {
    const w = 64, h = 48;
    const colors = [[255, 0, 0], [0, 255, 0], [0, 0, 248], [0, 0, 0], [248, 200, 64]];
    const frames = [0, 1, 2].map(t => frame(w, h, (x, y) => colors[(x + y + t) % colors.length]));
    const gif = decode(encodeGif(frames, w, h, 10));
    assert.equal(gif.width, w);
    assert.equal(gif.height, h);
    assert.equal(gif.frames.length, 3);
    assert.deepEqual(gif.delays, [10, 10, 10]);
    assert.ok(gif.loops);
    for (let t = 0; t < 3; t++) {
        for (let i = 0; i < w * h; i++) {
            const x = i % w, y = Math.floor(i / w);
            assert.deepEqual(gif.palette[gif.frames[t][i]], colors[(x + y + t) % colors.length].map(v => v & 0xF8));   // 5 bits a channel
        }
    }
});

test("a frame long enough to fill the code table restarts it and still decodes", () => {
    // Noise defeats the dictionary, so 128 by 128 runs well past 4096 codes.
    const w = 128, h = 128;
    let seed = 1;
    const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) & 255;
    const f = frame(w, h, () => [rnd() & 0xF8, rnd() & 0xF8, rnd() & 0xF8]);
    const gif = decode(encodeGif([f], w, h, 10));
    assert.equal(gif.frames[0].length, w * h);
    assert.ok(gif.frames[0].every(i => i < 256));
});

test("a gradient of more than 256 colors keeps every pixel close to its color", () => {
    const w = 32, h = 32;
    const f = frame(w, h, (x, y) => [x * 8, y * 8, 0]);   // 1024 distinct colors
    const gif = decode(encodeGif([f], w, h, 10));
    for (let i = 0; i < w * h; i++) {
        const [r, g] = gif.palette[gif.frames[0][i]];
        assert.ok(Math.abs(r - (i % w) * 8) <= 16 && Math.abs(g - Math.floor(i / w) * 8) <= 16);
    }
});
