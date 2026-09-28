// A browser refuses an insecure WebSocket opened from an HTTPS page, so a device behind a TLS proxy
// served its interface and then showed a black preview: the outputs kept running, because the block
// is in the browser rather than on the device. Reported on Discord (2026-09-27), diagnosed by the
// reporter: "the page attempts to load insecure websockets and browser blocks that inside an HTTPS
// page".
//
// Both channels (/ws for state, /wsp for the preview) must therefore follow the page's own scheme.
// The helper is pure, so it is lifted out of the source and RUN here rather than pattern-matched;
// the second test is what keeps a future channel from hardcoding its own URL again.
//
// Run: `node --test "test/js/**/*.test.mjs"`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

/// Lift `wsUrl` out of the browser script and make it callable against a stubbed `location`.
function wsUrlWith(location) {
    const src = app.match(/function wsUrl\s*\([\s\S]*?\n\}/);
    assert.ok(src, "wsUrl() is missing from app.js");
    return new Function("location", `${src[0]}; return wsUrl;`)(location);
}

test("an HTTPS page opens a secure WebSocket", () => {
    const wsUrl = wsUrlWith({ protocol: "https:", host: "panels.example.com" });
    assert.equal(wsUrl("/ws"), "wss://panels.example.com/ws");
    assert.equal(wsUrl("/wsp"), "wss://panels.example.com/wsp");
});

test("a plain HTTP page is unchanged, so the direct case pays nothing", () => {
    const wsUrl = wsUrlWith({ protocol: "http:", host: "192.168.1.158" });
    assert.equal(wsUrl("/ws"), "ws://192.168.1.158/ws");
    assert.equal(wsUrl("/wsp"), "ws://192.168.1.158/wsp");
});

test("a port survives, since location.host carries it", () => {
    const wsUrl = wsUrlWith({ protocol: "http:", host: "localhost:8080" });
    assert.equal(wsUrl("/ws"), "ws://localhost:8080/ws");
});

test("no channel hardcodes its own scheme", () => {
    const hard = app.match(/["'`]wss?:\/\//g) || [];
    assert.deepEqual(hard, [],
        "a WebSocket URL is built by hand: route it through wsUrl() so it follows the page");
    const sockets = app.match(/new WebSocket\(/g) || [];
    const viaHelper = app.match(/new WebSocket\(\s*wsUrl\(|ws = new WebSocket\(url\)/g) || [];
    assert.equal(sockets.length, viaHelper.length,
        "every WebSocket is constructed from wsUrl()");
});
