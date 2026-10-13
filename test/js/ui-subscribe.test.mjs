// The page tells the device which root it shows, and the device answers with that root in full and an outline of the rest.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { src, fnSource } from "./app-source.mjs";

const always = src.match(/const ALWAYS_SUBSCRIBED = (\[[^\]]*\]);/);

function subscriber() {
    const sent = [];
    const ws = { readyState: 1, send: (m) => sent.push(JSON.parse(m)) };
    const subscribe = new Function("ws", "WebSocket",
        `const ALWAYS_SUBSCRIBED = ${always[1]}; let subscribedRoot; ${fnSource("subscribe")} return subscribe;`)(ws, { OPEN: 1 });
    return { sent, subscribe };
}

test("a subscription names the root and the modules the page reads outside it", () => {
    const { sent, subscribe } = subscriber();
    subscribe("Drivers");
    assert.deepEqual(sent, [{ subscribe: ["Drivers", ...JSON.parse(always[1])] }]);
});

test("the same root is subscribed once, and another root subscribes again", () => {
    const { sent, subscribe } = subscriber();
    subscribe("Drivers");
    subscribe("Drivers");
    subscribe("Services");
    assert.equal(sent.length, 2);
    assert.equal(sent[1].subscribe[0], "Services");
});

test("every module the page always subscribes to is a registered type", () => {
    // A type renamed on the device would silently stop the status bar or the preview, with every other test green.
    const types = readFileSync(new URL("../../src/module_types.cpp", import.meta.url), "utf8");
    for (const t of JSON.parse(always[1])) assert.ok(types.includes(`("${t}"`), `${t} is not registered`);
});

test("a reconnect subscribes again, since the device keeps no subscription across sockets", () => {
    const open = src.slice(src.indexOf("ws.onopen = () => {"), src.indexOf("ws.onmessage"));
    assert.ok(open.includes("subscribedRoot = undefined;"));
    assert.ok(open.indexOf("subscribedRoot = undefined;") < open.indexOf("subscribe("));
});

test("the cards subscribe to the root they render, and an outline keeps the previous cards until its controls arrive", () => {
    const render = fnSource("renderCards");
    const sub = render.indexOf("subscribe(root.name)");
    const wait = render.indexOf("!Array.isArray(root.controls)");
    const clear = render.indexOf('main.innerHTML = ""');
    assert.ok(sub > 0 && wait > sub, "subscribe before deciding whether the root has arrived");
    assert.ok(clear > wait, "the DOM is cleared only once there is something to draw");
});

test("the target picker offers controls from the whole tree, not only the root on screen", () => {
    assert.ok(fnSource("setAssignMode").includes('fetch("/api/state")'));
    assert.ok(fnSource("assignableTargets").includes("assignTree ||"));
});

test("a page load draws from the subscribed state and fetches no whole tree", () => {
    // The first-paint fetch of /api/state cost a refresh the whole tree a second time, beside the socket's own answer.
    assert.ok(!fnSource("init").includes('fetch("/api/state")'));
    assert.ok(fnSource("init").includes("moonbaseHandoff()"), "a tab over MoonBase still hands over on load");
});

test("after an add, delete or move the page asks the socket for its view, not /api/state for the whole tree", () => {
    const refetch = fnSource("refetchState");
    assert.ok(!refetch.includes("fetch("), "no HTTP fetch of the tree");
    assert.ok(refetch.includes("resync: id"), "the request carries the id its answer echoes");
});

test("a full state releases the callers waiting for it, and not those whose request came later", () => {
    const run = new Function(`let stateWaiters = []; ${fnSource("answerStateWaiters")}
        const done = [];
        stateWaiters = [1, 2, 3].map(id => ({ id, resolve: () => done.push(id) }));
        answerStateWaiters(2);
        return { done, left: stateWaiters.map(w => w.id) };`);
    const { done, left } = run();
    assert.deepEqual(done, [1, 2]);
    assert.deepEqual(left, [3]);
});

test("the full-state handler answers the waiting callers with the id the document echoes", () => {
    const at = src.indexOf("if (!Array.isArray(data.modules)) return;");
    assert.ok(src.slice(at, at + 1500).includes("if (data.resync) answerStateWaiters(data.resync);"));
});

test("a config backup names its device from the state the page holds, without fetching the tree", () => {
    const backup = fnSource("fmBackupConfig");
    assert.ok(!backup.includes('fetch("/api/state")'));
    assert.ok(backup.includes("state && state.modules"));
});

// Every module type the page names is either always subscribed or read where it is already whole, each with its reason.
// A new name fails here until someone decides which: a read of another root's controls that is not subscribed finds an outline, without them.
const READ_WHERE_WHOLE = {
    MoonStatsModule: "a MoonCloud child, subscribed with it",
    MoonTalkModule: "a MoonCloud child, subscribed with it",
    NetworkModule: "read only by its own root's cards (the address-move check)",
    EthernetModule: "read only by its own root's cards",
    WiFiModule: "read only by its own root's cards",
    FileManagerModule: "its own card's panel",
    ControlModule: "its name, which the outline carries, or its own card",
    GamepadService: "its name, which the outline carries",
    MoonLiveEffect: "a type to create, not a module read",
    MoonLiveLayout: "a type to create, not a module read",
    MoonLiveModifier: "a type to create, not a module read",
    MoonLiveService: "a type to create, not a module read",
    RainbowEffect: "a comment's example",
};

test("every module type the page names is subscribed or read where it is whole", () => {
    const subscribed = new Set(JSON.parse(always[1]));
    const named = new Set([...src.matchAll(/"([A-Z][A-Za-z0-9]*(?:Module|Service|Driver|Effect|Layout|Modifier))"/g)].map(m => m[1]));
    const unexplained = [...named].filter(t => !subscribed.has(t) && !(t in READ_WHERE_WHOLE));
    assert.deepEqual(unexplained, [], "add the type to ALWAYS_SUBSCRIBED, or to READ_WHERE_WHOLE with the reason its controls are there");
    const stale = Object.keys(READ_WHERE_WHOLE).filter(t => !named.has(t));
    assert.deepEqual(stale, [], "a type the page does not name leaves the list");
});
