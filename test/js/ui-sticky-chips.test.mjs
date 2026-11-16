// The type picker's chip filter holds per role: set once, it is there for every effect switched after it, and a picker of another role keeps its own.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

function chipsWith(pickerChips) {
    const stored = {};
    const scope = {
        pickerChips,
        LS_CHIPS: "chips",
        SCRIPTED_EMOJI: "📝",
        COMPILED_EMOJI: "📦",
        safeLocalSet: (key, value) => { stored[key] = JSON.parse(value); },
    };
    const api = new Function(...Object.keys(scope),
        `${fnSource("stickyChips")}\n${fnSource("rememberChips")}\nreturn { stickyChips, rememberChips };`)(...Object.values(scope));
    return { ...api, stored };
}

test("chips set in the effect picker open with the next effect picker", () => {
    const c = chipsWith({});
    c.rememberChips(["effect"], ["🧊", "🎵", "🔥"], new Set(["🧊", "🎵"]));
    assert.deepEqual(c.stickyChips(["effect"]).sort(), ["🎵", "🧊"].sort());
    assert.deepEqual(c.stored.chips, { effect: ["🧊", "🎵"] });
});

test("the add picker of a layer and the replace picker of an effect share the effect's chips", () => {
    const c = chipsWith({ effect: ["🧊"] });
    assert.deepEqual(c.stickyChips(["effect", "modifier"]), ["🧊"]);
});

test("a picker of another role keeps its own chips", () => {
    const c = chipsWith({ effect: ["🧊"] });
    assert.deepEqual(c.stickyChips(["driver"]), []);
});

test("a remembered chip the picker does not offer survives that picker, and clearing what it offers clears only that", () => {
    const c = chipsWith({ effect: ["🧊", "🎵"] });
    c.rememberChips(["effect"], ["🎵", "🔥"], new Set());   // a list that omits the cube chip: the cube stays remembered
    assert.deepEqual(c.stickyChips(["effect"]), ["🧊"]);
});

test("a scripted chip remembered by one role and a compiled chip by another cancel, so the add picker never opens empty", () => {
    const c = chipsWith({ effect: ["📝", "🔥"], modifier: ["📦"] });
    assert.deepEqual(c.stickyChips(["effect", "modifier"]), ["🔥"]);
    assert.deepEqual(c.stickyChips(["effect"]), ["📝", "🔥"]);
});
