// A full renderCards() rebuilds the module DOM, so it destroys an open native select or a field
// being typed into, the control disappears from under the cursor mid-click.
//
// Both re-render triggers must therefore check `userIsEditing()` first: the /api/types arrival, and
// the WebSocket FULL STATE. The WS one is what users actually hit, because a full state is sent on
// every (re)connect, a browser under load on a big layout reconnects repeatedly, and a dropdown
// becomes impossible to select before it vanishes. Reported on Discord (2026-08-25): "every time I
// get near the option it disappears", and still present after disabling the preview, which is what
// ruled out preview cost as the cause.
//
// app.js is a browser script rather than a module, so this pins the contract by reading the source:
// every renderCards() call on a state-arrival path is guarded. A behavioural test would need a DOM.
//
// Run: `node --test "test/js/**/*.test.mjs"`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

test("the editing guard lives in exactly one place", () => {
    const defs = app.match(/function userIsEditing\s*\(/g) || [];
    assert.equal(defs.length, 1, "userIsEditing must have one home, not be re-implemented per caller");
    // The predicate must cover all three interaction shapes a rebuild would destroy.
    const body = app.slice(app.indexOf("function userIsEditing"));
    for (const shape of ["input, textarea", "closest(\"select\")", 'select[data-open="true"]']) {
        assert.ok(body.includes(shape), `userIsEditing must consider ${shape}`);
    }
});

test("the WebSocket full state does not rebuild the DOM while the user is interacting", () => {
    // The handler stores `state` then decides whether to re-render; the guard must sit between.
    const i = app.indexOf("if (!Array.isArray(data.modules)) return;");
    assert.ok(i > 0, "WS full-state handler not found, this test needs updating");
    const handler = app.slice(i, i + 1200);
    const guardAt = handler.indexOf("userIsEditing()");
    const renderAt = handler.indexOf("renderStructure()");
    assert.ok(guardAt > 0, "the WS full state must check userIsEditing() before rebuilding");
    assert.ok(renderAt > 0, "the WS full state renders through renderStructure()");
    assert.ok(guardAt < renderAt, "the guard must precede the render, or the DOM is already gone");
    const fn = app.slice(app.indexOf("function renderStructure"));
    assert.ok(fn.slice(0, fn.indexOf("\n}\n")).includes("renderCards()"), "renderStructure must rebuild the cards");
});

// A full state deferred mid-edit renders when the edit ends, not on the next full state, which comes only on a schema change or a reconnect:
// without that, a board preset switched elsewhere shows its pins only after a page refresh.
test("a full state deferred while editing renders as soon as the edit ends", () => {
    const i = app.indexOf("if (!Array.isArray(data.modules)) return;");
    const handler = app.slice(i, i + 1200);
    assert.ok(handler.includes("structuralRenderPending = true"), "the deferral must remember the render it skipped");
    const deferred = app.slice(app.indexOf("function renderIfDeferred"));
    const body = deferred.slice(0, deferred.indexOf("\n}\n"));
    assert.ok(body.includes("structuralRenderPending") && body.includes("renderStructure()"), "the deferred render checks for one pending");
    assert.ok(app.includes('addEventListener("focusout", renderIfDeferred)'), "focus leaving a field ends an edit");
    assert.ok(app.includes('addEventListener("selectionchange"'), "a cleared text selection ends an edit too, and moves no focus");
});

test("the /api/types arrival keeps its guard too", () => {
    const i = app.indexOf('fetch("/api/types")');
    assert.ok(i > 0, "/api/types fetch not found, this test needs updating");
    const block = app.slice(i, i + 400);
    assert.ok(block.includes("userIsEditing()"),
              "/api/types must not rebuild the DOM mid-edit either");
});

test("a truncated preview frame feeds no adaptation counters: validate first, then count", () => {
    // Source-pinned: renderPreviewFrame must complete BOTH length checks before adaptFrames_ or
    // windowDrops_ move, or a garbage frame steers the resolution controller.
    const src = readFileSync(new URL("../../src/ui/preview3d.js", import.meta.url), "utf8");
    const fn = src.slice(src.indexOf("function renderPreviewFrame"));
    const body = fn.slice(0, fn.indexOf("drawLights(rgb)"));
    const lastCheck = body.lastIndexOf("byteLength < 9 + count * 3");
    assert.ok(lastCheck > 0, "the body-length check must exist");
    assert.ok(body.indexOf("adaptFrames_++") > lastCheck, "frames counted only after full validation");
    assert.ok(body.indexOf("windowDrops_ +=") > lastCheck, "drops counted only after full validation");
});
