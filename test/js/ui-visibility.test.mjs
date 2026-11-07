// Tab-visibility hibernation: a hidden tab must cost the device nothing and, critically, must not
// keep MEASURING, its throttled timers read ~0 fps and would coarsen the shared preview for every
// viewer (coarsest request wins; observed live on the desktop before this landed).
//
// app.js is a browser script, so these pin the contract in the source, like ui-render-guard does.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

test("hiding the tab closes the preview socket immediately, keeping the pane's intent", () => {
    const i = app.indexOf('addEventListener("visibilitychange"');
    assert.ok(i > 0, "no visibilitychange handler");
    const h = app.slice(i, i + 1600);
    const hiddenBranch = h.slice(h.indexOf("document.hidden"));
    assert.ok(hiddenBranch.includes("closePreviewSocket()"),
              "hide must close the preview socket without clearing previewWanted");
    assert.ok(!hiddenBranch.slice(0, hiddenBranch.indexOf("} else")).includes("disconnectPreview("),
              "hide must NOT dismiss the pane, intent has to survive for the return path");
});

test("the control socket closes only after a grace, so alt-tab costs no resync", () => {
    const i = app.indexOf("WS_HIDE_GRACE_MS");
    assert.ok(i > 0, "no hide grace constant");
    assert.ok(/setTimeout\([\s\S]{0,900}WS_HIDE_GRACE_MS\)/.test(app),
              "the control-socket close must sit behind the grace timer");
    assert.ok(app.includes("clearTimeout(wsHideTimer)"),
              "returning within the grace must cancel the pending close");
});

test("returning reopens control first, then the preview via the pane's own intent", () => {
    const i = app.indexOf('addEventListener("visibilitychange"');
    const visible = app.slice(i, i + 2200);
    const elseAt = visible.indexOf("} else {");
    const ret = visible.slice(elseAt);
    const ctl = ret.indexOf("connectWs()");
    const prev = ret.indexOf("connectPreview()");
    assert.ok(ctl > 0 && prev > 0 && ctl < prev,
              "control reconnect must precede the preview reconnect");
    assert.ok(ret.includes("if (previewWanted)"),
              "the preview reopens only when the pane still wants frames");
});

test("closing the preview socket stops the adaptation loop with it", () => {
    const i = app.indexOf("function closePreviewSocket");
    assert.ok(i > 0);
    assert.ok(app.slice(i, i + 400).includes("preview.adaptStop()"),
              "a closed socket must not keep measuring, that is the backgrounded-tab bug");
});

test("hiding cancels a pending reconnect, so no control socket opens on a hidden tab", () => {
    const i = app.indexOf('addEventListener("visibilitychange"');
    const hidden = app.slice(i, app.indexOf("} else {", i));
    assert.ok(/clearTimeout\(wsReconnectTimer\)/.test(hidden),
              "the hide path must cancel a reconnect armed before the tab hid");
});

test("a preview reconnect armed before hiding is cancelled, and never fires on a hidden tab", () => {
    assert.ok(app.includes("wspRetryTimer = setTimeout("),
              "the preview retry must be tracked, not a fire-and-forget setTimeout");
    const closer = app.slice(app.indexOf("function closePreviewSocket"));
    assert.ok(/clearTimeout\(wspRetryTimer\)/.test(closer.slice(0, 400)),
              "closing the preview socket must cancel a pending retry");
    assert.ok(app.includes("!document.hidden) connectPreview()"),
              "a retry that fires anyway must refuse to open a socket on a hidden tab");
});

// A tab hidden for less than the grace keeps its socket, and the device sends only what changed, so a
// frame dropped while hidden was a change lost until it changed again: the Audio status stuck on a
// format switched from another tab (Safari, 2026-11-07).
test("the control socket applies every frame while the tab is hidden", () => {
    const i = app.indexOf("ws.onmessage = (e) => {");
    assert.ok(i > 0, "no control-socket message handler");
    const guard = app.slice(i, app.indexOf("\n", app.indexOf("return;", i)));
    assert.ok(!guard.includes("wsPaused"),
              "the control socket must not drop frames while hidden: the next patch carries only what changed");
});
