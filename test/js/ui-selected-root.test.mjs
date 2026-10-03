// The root a user was last on survives a refresh, whichever state arrives first.
//
// Two payloads race on load: the WebSocket full state and the /api/state fetch. Both set `state` and
// both trigger the first render, so either can be the one that decides what is on screen. Only the
// fetch used to consult the saved selection, so when the socket won, the selection stayed null and
// renderCards fell back to the first root: a refresh with a Layer or the File Manager open landed on
// Control instead. Intermittent exactly as a race is, and it healed after any nav click, which sets
// the selection in memory and hides the bug for the rest of the session.
//
// So this pins the rule at its source: both arrival paths restore before the render that reads it.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const src = readFileSync(new URL("../../src/ui/app.js", import.meta.url), "utf8");

/// A top-level function's source, by brace matching.
function fnSource(name) {
    const at = src.indexOf(`function ${name}(`);
    assert.notEqual(at, -1, `${name} not found in app.js`);
    const open = src.indexOf("{", at);
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}" && --depth === 0) return src.slice(at, i + 1);
    }
    assert.fail(`unbalanced braces in ${name}`);
}

/// restoreSelectedRoot, given a tree, what localStorage holds and the page's query; returns the root and the tabs it selected.
function restoreWithTabs({ modules, saved, already = null, search = "" }) {
    const tabs = {};
    let replaced = null;
    const build = new Function("state", "lsRead", "LS_SELECTED", "LS_TABS", "selectedModuleIn", "selectedTabs",
                               "safeLocalSet", "location", "history", `
        let selectedModule = selectedModuleIn;
        ${fnSource("findModule")}
        ${fnSource("findParentOf")}
        ${fnSource("restoreSelectedRoot")}
        restoreSelectedRoot();
        return selectedModule;`);
    const root = build({ modules }, () => saved, "mm_selectedRoot", "mm_selectedTabs", already, tabs, () => {},
                       { search, pathname: "/" }, { replaceState: (_s, _t, url) => { replaced = url; } });
    return { root, tabs, replaced };
}

/// restoreSelectedRoot's root alone, for the cases without a link.
function restore(args) {
    return restoreWithTabs(args).root;
}

const TREE = [{ name: "Control" }, { name: "Layouts" }, { name: "File Manager" }, { name: "System" }];

test("the saved root is restored, so a refresh returns to where the user was", () => {
    assert.equal(restore({ modules: TREE, saved: "File Manager" }), "File Manager");
    assert.equal(restore({ modules: TREE, saved: "Layouts" }), "Layouts");
});

test("a choice made this session is not overwritten by what was saved earlier", () => {
    // The user clicked System a moment ago; a later full state must not send them back.
    assert.equal(restore({ modules: TREE, saved: "File Manager", already: "System" }), "System");
});

test("a saved root that is no longer in the tree is ignored, leaving the fallback to choose", () => {
    // A module deleted since the last visit, or a config from another device.
    assert.equal(restore({ modules: TREE, saved: "Gone" }), null);
    assert.equal(restore({ modules: TREE, saved: null }), null);
});

test("a link naming a module opens it with its tab selected, over what was saved", () => {
    // The captive portal sends a phone to `?open=WiFi`: the Network root, its WiFi tab open.
    const tree = [{ name: "Control" }, { name: "Network", children: [{ name: "Ethernet" }, { name: "WiFi" }, { name: "AccessPoint" }] }];
    const r = restoreWithTabs({ modules: tree, saved: "Control", already: "Control", search: "?open=WiFi" });
    assert.equal(r.root, "Network");
    assert.equal(r.tabs.Network, "WiFi");
    assert.equal(r.replaced, "/", "the query is dropped, so a refresh shows what the user chose since");
    // A name not in the tree is ignored, and the saved root restores as usual.
    assert.equal(restore({ modules: tree, saved: "Control", search: "?open=Gone" }), "Control");
});

test("an empty or absent tree leaves the selection alone rather than guessing", () => {
    assert.equal(restore({ modules: [], saved: "File Manager" }), null);
});

test("both state arrivals restore the selection before the render that reads it", () => {
    // The actual bug: the WebSocket handler set `state` and rendered without consulting the saved
    // root, so whichever payload won the race decided whether the user's tab survived. Checking the
    // call sites, because a correct restoreSelectedRoot that only one path calls is the bug itself.
    const wsAt = src.indexOf("if (!Array.isArray(data.modules)) return;");
    assert.notEqual(wsAt, -1, "the websocket full-state handler moved");
    // To the RENDER, not to the first setTargetFps: the mid-edit early-return above it mentions the
    // same call, and slicing there cut the block off before the lines under test.
    const wsBlock = src.slice(wsAt, src.indexOf("renderNav()", wsAt));
    const restoreAt = wsBlock.indexOf("restoreSelectedRoot()");
    const renderAt = wsBlock.indexOf("renderCards()");
    assert.notEqual(restoreAt, -1, "the websocket path must restore the saved root");
    assert.ok(restoreAt < renderAt, "restore must run BEFORE the render that reads the selection");

    // And the fetch path, which is the one that always had it.
    const httpAt = src.indexOf("const snap = await resp.json();");
    assert.notEqual(httpAt, -1, "the /api/state fetch moved");
    const httpBlock = src.slice(httpAt, src.indexOf("renderCards()", httpAt));
    assert.ok(httpBlock.includes("restoreSelectedRoot()"), "the fetch path must restore too");
});
