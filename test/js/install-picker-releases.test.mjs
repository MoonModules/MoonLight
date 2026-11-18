// The release list the badge, the map and the installer share: a failed first fetch is remembered in the page alone, so a refresh asks again.
// Run: `node --test test/js/install-picker-releases.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

const stored = new Map();
globalThis.sessionStorage = { getItem: (k) => stored.get(k) ?? null, setItem: (k, v) => stored.set(k, v) };
globalThis.fetch = async () => { throw new Error("offline"); };
const { loadReleases } = await import("../../src/ui/install-picker.js");

test("a failed first fetch stays in memory, outside the session's storage a refresh reads", async () => {
    assert.equal(await loadReleases(), null);
    assert.equal(stored.size, 0);
});
