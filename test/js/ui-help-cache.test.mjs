// The help a device shows comes from the copy of its own release, else the site's: a cached copy from another release never shows, so an update or an offline device never names another firmware's controls.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

const RELEASE = (tag) => `https://rel/${tag}/help.json`;
const SITE = "https://site/help.json";
const help = (name) => JSON.stringify({ modules: { [name]: {} } });

// loadHelp with its globals stubbed: the stored cache, the device's version, and what each address answers.
function harness({ cached = null, version = "1.0.0", stateKnown = true, answers = {} }) {
    const store = { mm_help: cached ? JSON.stringify(cached) : null };
    const fetched = [];
    const fetch = (url) => { fetched.push(url); return Promise.resolve(answers[url] ? { ok: true, text: () => Promise.resolve(answers[url]) } : { ok: false, status: 404 }); };
    const run = new Function("lsRead", "state", "deviceFirmwareInfo", "releaseTagOf", "renderCards", "userIsEditing", "fetch", "localStorage",
        "let helpModules = {}, helpPending = false;\n" +
        "const LS_HELP = 'mm_help', LS_HELP_SOURCE = 'mm_helpSource', HELP_URL_LOCAL = 'http://local/help.json';\n" +
        `const HELP_URL = '${SITE}', HELP_URL_RELEASE = (tag) => 'https://rel/' + tag + '/help.json';\n` +
        fnSource("loadHelp") + "\nreturn { loadHelp, modules: () => helpModules, pending: () => helpPending };")(
        (k, d) => store[k] ?? d, stateKnown ? { modules: [] } : null, () => ({ version }), (v) => `v${v}`,
        () => {}, () => false, fetch, { setItem: (k, v) => { store[k] = v; } });
    return { ...run, fetched, store };
}
const settle = () => new Promise((r) => setTimeout(r, 0));

test("a cached copy of another release does not show, and one of the running release does", () => {
    assert.deepEqual(Object.keys(harness({ cached: { url: RELEASE("v0.9.0"), text: help("Old") } }).modules()), []);
    const h = harness({ cached: { url: RELEASE("v1.0.0"), text: help("Mine") } });
    h.loadHelp();
    assert.deepEqual(Object.keys(h.modules()), ["Mine"]);
});

test("the site's cached copy serves any release, and an old plain-text cache is dropped", () => {
    const h = harness({ cached: { url: SITE, text: help("Any") } });
    h.loadHelp();
    assert.deepEqual(Object.keys(h.modules()), ["Any"]);
    const old = harness({});
    old.store.mm_help = help("Plain");
    old.loadHelp();
    assert.deepEqual(Object.keys(old.modules()), []);
});

test("the release's copy is asked first, then the site's, and the answer is cached with its address", async () => {
    const h = harness({ answers: { [SITE]: help("Site") } });
    h.loadHelp();
    await settle(); await settle(); await settle();
    assert.deepEqual(h.fetched, [RELEASE("v1.0.0"), SITE]);
    assert.deepEqual(JSON.parse(h.store.mm_help).url, SITE);
    assert.deepEqual(Object.keys(h.modules()), ["Site"]);
});

test("before the state names the firmware nothing is fetched", () => {
    const h = harness({ stateKnown: false });
    h.loadHelp();
    assert.equal(h.pending(), true);
    assert.deepEqual(h.fetched, []);
});
