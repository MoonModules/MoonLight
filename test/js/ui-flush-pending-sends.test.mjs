// An action that follows typing (a rename, Enter, a send button) first lands every write a debounce still holds for that card, each once.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

function desk() {
    const pendingSends = {}, dragTimers = {};
    const api = new Function("pendingSends", "dragTimers",
        `${fnSource("debounceSend")}\n${fnSource("flushPendingSends")}\nreturn { debounceSend, flushPendingSends };`)(pendingSends, dragTimers);
    return { ...api, pendingSends, dragTimers };
}

const settle = (ms) => new Promise(r => setTimeout(r, ms));

test("a flush sends each held write once, cancels its debounce, and finishes only when every send has", async () => {
    const d = desk();
    const sent = [];
    let release;
    const firstHeld = new Promise(r => { release = r; });
    d.debounceSend("Knob:a", 5, async () => { sent.push("a"); await firstHeld; });
    d.debounceSend("Knob:b", 5, async () => { sent.push("b"); });
    d.debounceSend("Other:c", 5000, async () => { sent.push("c"); });
    let done = false;
    const flush = d.flushPendingSends("Knob:").then(() => { done = true; });
    await settle(20);   // past b's debounce: a timer left running would send b a second time here
    assert.deepEqual(sent, ["a"]);
    assert.equal(done, false);
    release();
    await flush;
    assert.deepEqual(sent, ["a", "b"]);
    assert.deepEqual(Object.keys(d.pendingSends), ["Other:c"]);
    clearTimeout(d.dragTimers["Other:c"]);   // the other card's write, left to its own debounce
});
