// A GIF89a encoder, written to the format's specification (W3C GIF89a, with LZW as it defines it), so a preset's preview becomes a file without a library in the firmware.
// One global palette for every frame, by median cut, and the NETSCAPE2.0 extension so the clip loops.

const MAX_CODE = 4095;   // 12-bit codes, the format's ceiling

// A palette of up to 256 colors by median cut (Heckbert 1982): the colors, at 5 bits per channel and weighted by how often they occur, are split into boxes of equal weight along each box's widest channel, and a box's average is its color.
function buildPalette(frames) {
    const counts = new Uint32Array(32768);
    for (const f of frames) {
        for (let i = 0; i < f.length; i += 4) counts[((f[i] >> 3) << 10) | ((f[i + 1] >> 3) << 5) | (f[i + 2] >> 3)]++;
    }
    const channel = (c, k) => (c >> (10 - 5 * k)) & 31;
    const used = [];
    for (let c = 0; c < counts.length; c++) if (counts[c]) used.push(c);
    let boxes = [used];
    while (boxes.length < 256) {
        // Split the box spanning the widest channel range; a box of one color cannot split.
        let pick = -1, widest = 0, axis = 0;
        boxes.forEach((box, i) => {
            if (box.length < 2) return;
            for (let k = 0; k < 3; k++) {
                let lo = 31, hi = 0;
                for (const c of box) { const v = channel(c, k); if (v < lo) lo = v; if (v > hi) hi = v; }
                if (hi - lo > widest) { widest = hi - lo; pick = i; axis = k; }
            }
        });
        if (pick < 0) break;
        const box = boxes[pick].sort((a, b) => channel(a, axis) - channel(b, axis));
        const half = box.reduce((n, c) => n + counts[c], 0) / 2;
        let acc = 0, cut = 1;
        for (; cut < box.length - 1; cut++) { acc += counts[box[cut - 1]]; if (acc >= half) break; }
        boxes.splice(pick, 1, box.slice(0, cut), box.slice(cut));
    }
    const index = new Uint8Array(32768);
    const rgb = boxes.map((box, i) => {
        let n = 0, r = 0, g = 0, b = 0;
        for (const c of box) {
            const w = counts[c];
            n += w; r += w * channel(c, 0); g += w * channel(c, 1); b += w * channel(c, 2);
            index[c] = i;
        }
        return [Math.round(r / n) << 3, Math.round(g / n) << 3, Math.round(b / n) << 3];
    });
    return { rgb, index };
}

// One frame's pixels as LZW codes packed least-significant bit first, in 255-byte sub-blocks.
function lzw(indices, minCodeSize) {
    const clear = 1 << minCodeSize, end = clear + 1;
    const out = [];
    let cur = 0, bits = 0;
    let size = minCodeSize + 1;
    const emit = code => {
        cur |= code << bits;
        bits += size;
        while (bits >= 8) { out.push(cur & 255); cur >>= 8; bits -= 8; }
    };
    let dict = new Map(), next = end + 1;
    emit(clear);
    let prefix = indices[0];
    for (let i = 1; i < indices.length; i++) {
        const k = indices[i];
        const key = prefix * 256 + k;
        const hit = dict.get(key);
        if (hit !== undefined) { prefix = hit; continue; }
        emit(prefix);
        if (next <= MAX_CODE) {
            dict.set(key, next++);
            if (next > (1 << size) && size < 12) size++;
        } else {
            emit(clear);
            dict = new Map(); next = end + 1; size = minCodeSize + 1;
        }
        prefix = k;
    }
    emit(prefix);
    emit(end);
    if (bits > 0) out.push(cur & 255);
    const blocks = [];
    for (let i = 0; i < out.length; i += 255) {
        const chunk = out.slice(i, i + 255);
        blocks.push(chunk.length, ...chunk);
    }
    return blocks;
}

/// Encode RGBA frames, all `width` by `height`, as a looping GIF shown `delayCs` hundredths of a second each.
export function encodeGif(frames, width, height, delayCs) {
    const { rgb, index } = buildPalette(frames);
    const bytes = [];
    const u16 = v => bytes.push(v & 255, (v >> 8) & 255);
    bytes.push(..."GIF89a".split("").map(c => c.charCodeAt(0)));
    u16(width); u16(height);
    bytes.push(0xF7, 0, 0);   // a global table of 256 entries, 8 bits a color
    for (let i = 0; i < 256; i++) bytes.push(...(rgb[i] || [0, 0, 0]));
    bytes.push(0x21, 0xFF, 11, ..."NETSCAPE2.0".split("").map(c => c.charCodeAt(0)), 3, 1, 0, 0, 0);   // loop forever
    for (const f of frames) {
        bytes.push(0x21, 0xF9, 4, 0x04);   // graphic control: each frame replaces the last
        u16(delayCs);
        bytes.push(0, 0);
        bytes.push(0x2C); u16(0); u16(0); u16(width); u16(height); bytes.push(0);
        const px = new Uint8Array(width * height);
        for (let p = 0, i = 0; p < px.length; p++, i += 4) px[p] = index[((f[i] >> 3) << 10) | ((f[i + 1] >> 3) << 5) | (f[i + 2] >> 3)];
        bytes.push(8);
        for (const b of lzw(px, 8)) bytes.push(b);   // a loop: a frame's stream passes the engines' argument limit as a spread
        bytes.push(0);
    }
    bytes.push(0x3B);
    return new Uint8Array(bytes);
}
