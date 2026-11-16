// An action that follows typing (a rename, Enter, a send button) first lands every write a debounce still holds for that card, each once.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

function flusher(pendingSends, dragTimers) {
    return new Function("pendingSends", "dragTimers",
        `${fnSource("flushPendingSends")}\nreturn flushPendingSends;`)(pendingSends, dragTimers);
}

test("a debounce that fires while the first write is awaited leaves the flush sending the rest, each once", async () => {
    const sent = [];
    const pendingSends = {};
    const dragTimers = {};
    pendingSends["Knob:a"] = async () => {
        sent.push("a");
        // The second control's own timer firing mid-wait, as debounceSend's does: it deletes its entry and sends.
        delete pendingSends["Knob:b"];
        await Promise.resolve();
    };
    pendingSends["Knob:b"] = async () => { sent.push("b"); };
    pendingSends["Other:c"] = async () => { sent.push("c"); };
    await flusher(pendingSends, dragTimers)("Knob:");
    assert.deepEqual(sent, ["a", "b"]);
    assert.deepEqual(Object.keys(pendingSends), ["Other:c"]);
});
