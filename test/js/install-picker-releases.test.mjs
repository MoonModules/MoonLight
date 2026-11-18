// The release list the badge, the map and the installer share: a failed first fetch is remembered in the page alone, so a refresh asks again,
// and a first page of prereleases still holds the newest stable release.
// Run: `node --test test/js/install-picker-releases.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

const stored = new Map();
globalThis.sessionStorage = { getItem: (k) => stored.get(k) ?? null, setItem: (k, v) => stored.set(k, v) };
globalThis.fetch = async () => { throw new Error("offline"); };
const { loadReleases, latestStable } = await import("../../src/ui/install-picker.js");

test("a failed first fetch stays in memory, outside the session's storage a refresh reads", async () => {
    assert.equal(await loadReleases(), null);
    assert.equal(stored.size, 0);
});

// GitHub answering `pages` by address: the first page of releases, or the newest stable one; every address asked is logged.
function serve(pages) {
    const asked = [];
    globalThis.fetch = async (url) => {
        asked.push(url);
        const body = url.endsWith("/releases/latest") ? pages.latest : pages.list;
        return { ok: body !== undefined, json: async () => structuredClone(body) };
    };
    return asked;
}

const prerelease = (n) => ({ tag_name: `v9.0.0-rc${n}`, prerelease: true, draft: false });
const stable = { tag_name: "v8.0.0", prerelease: false, draft: false };

test("a first page of prereleases is completed with the newest stable release /releases/latest names", async () => {
    const asked = serve({ list: Array.from({ length: 10 }, (_, i) => prerelease(10 - i)), latest: stable });
    const releases = await loadReleases({ bypassCache: true });
    assert.equal(latestStable(releases)?.tag_name, "v8.0.0");
    assert.equal(releases.length, 11);
    assert.equal(asked.length, 2);
});

test("a first page holding a stable release is the one request", async () => {
    const asked = serve({ list: [prerelease(1), stable], latest: { tag_name: "v0.0.1", prerelease: false } });
    const releases = await loadReleases({ bypassCache: true });
    assert.equal(latestStable(releases)?.tag_name, "v8.0.0");
    assert.equal(asked.length, 1);
});

test("a /releases/latest that does not answer leaves the first page as it is", async () => {
    serve({ list: [prerelease(1)] });
    const releases = await loadReleases({ bypassCache: true });
    assert.deepEqual(releases.map(r => r.tag_name), ["v9.0.0-rc1"]);
});
