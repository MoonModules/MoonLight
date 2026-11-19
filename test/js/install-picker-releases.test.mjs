// The release list the badge, the map and the installer share, read from the release feed on Pages: asked at most once per the feed's max-age
// unless a view asks past it, a reply that did not change is not parsed again, and a page whose feed does not answer keeps what it showed.
// Run: `node --test test/js/install-picker-releases.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

// The feed answering with `list` and `modified` as its Last-Modified, or not answering; every ask, its options and every parse are logged.
const log = { asked: [], parsed: 0 };
function serve(list, modified = "Wed, 18 Nov 2026 22:30:15 GMT") {
    globalThis.fetch = async (url, opts) => {
        log.asked.push({ url, opts });
        if (list === undefined) throw new Error("offline");
        return { ok: true, headers: new Map([["last-modified", modified]]), json: async () => { log.parsed++; return structuredClone(list); } };
    };
}
serve(undefined);
const { loadReleases, latestStable } = await import("../../src/ui/install-picker.js");

const latest = { tag_name: "latest", name: "7.1.0-dev.153", prerelease: true, draft: false };
const stable = { tag_name: "v7.0.0", name: "7.0.0", prerelease: false, draft: false };

test("a page whose feed stayed silent has no list, and waits before asking again", async () => {
    assert.equal(await loadReleases(), null);
    assert.equal(await loadReleases(), null);
    assert.equal(log.asked.length, 1);
});

test("the list is read from the release feed on Pages, and an opened view asks past the HTTP cache", async () => {
    serve([latest, stable]);
    log.asked.length = 0;
    const releases = await loadReleases({ revalidate: true });
    assert.equal(latestStable(releases)?.tag_name, "v7.0.0");
    assert.equal(log.asked[0].url, "https://moonmodules.org/MoonLight/install/releases.json");
    assert.deepEqual(log.asked[0].opts, { cache: "no-cache" });
});

test("between asks the page reads the list it holds, asking the feed at most once per its max-age, so the badge's second costs no request", async () => {
    const before = log.asked.length;
    const first = await loadReleases();
    const again = await loadReleases();
    assert.equal(again, first);
    assert.equal(log.asked.length, before);
});

test("a view that asks past the cache gets a new release parsed; one unchanged keeps its identity for the map", async () => {
    const parsedBefore = log.parsed;
    const same = await loadReleases({ revalidate: true });   // the same Last-Modified: not parsed again
    assert.equal(log.parsed, parsedBefore);
    serve([{ ...latest, name: "7.1.0-dev.154" }, stable], "Thu, 19 Nov 2026 08:00:00 GMT");
    const releases = await loadReleases({ revalidate: true });
    assert.equal(releases[0].name, "7.1.0-dev.154");
    assert.notEqual(releases, same);
});

test("a feed that stops answering leaves the list already shown", async () => {
    serve(undefined);
    const releases = await loadReleases({ revalidate: true });
    assert.equal(releases?.[0].name, "7.1.0-dev.154");
});

test("a call while a request is on its way gets that request's answer, so a cold page's second caller sees the list", async () => {
    const next = { tag_name: "v7.1.0", name: "7.1.0", prerelease: false, draft: false };
    let answer;
    globalThis.fetch = async () => {
        await new Promise((resolve) => { answer = resolve; });
        return { ok: true, headers: new Map([["last-modified", "Thu, 19 Nov 2026 20:00:00 GMT"]]), json: async () => [next, stable] };
    };
    const first = loadReleases({ revalidate: true });
    const second = loadReleases();   // asked while the first is on its way
    answer();
    assert.equal((await first)[0].tag_name, "v7.1.0");
    assert.equal((await second)[0].tag_name, "v7.1.0");
});
