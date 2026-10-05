// A row field, a popup's name and a read-only row value are controls, drawn by createControl, so each has what a card's control has: the reset, the edit guards, the commit rules.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

for (const name of ["fillEditableListDetail", "fillListDetail", "openPadEditor", "presetNameControl"]) {
    test(`${name} draws its values through createControl rather than its own inputs`, () => {
        const body = fnSource(name);
        assert.ok(body.includes("createControl("), `${name} must use createControl`);
        assert.ok(!/createElement\("(input|select|textarea)"\)/.test(body), `${name} must not build its own inputs`);
    });
}

// The list rebuild keeps a value still being typed only for fields it can find by this key; moving row fields onto createControl once dropped it, so no field survived a rebuild.
test("every field createControl draws carries the key a list rebuild restores it by", () => {
    const body = fnSource("createControl");
    assert.ok(/querySelectorAll\("input, select, textarea"\)\)[^\n]*dataset\.dragkey = key/.test(body));
    assert.ok(fnSource("updateModuleControls").includes('querySelectorAll("[data-dragkey]")'));
});

test("the card's save-as-preset name is the surface's own name control", () => {
    const popup = fnSource("openDocumentPopup");
    assert.ok(popup.includes('presetNameControl("preset name")'));
    assert.ok(!/createElement\("input"\)/.test(popup));
});
