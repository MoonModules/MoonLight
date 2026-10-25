// A container's 🗑 removes everything a user added to it, through the one path every state document takes: `$patch: replace` listing nothing, nested under the container's ancestors.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

function clearWith(modules, name) {
    const sent = [];
    const scope = {
        state: { modules },
        fetch: async (url, opts) => { sent.push({ url, method: opts.method, doc: JSON.parse(opts.body) }); },
        refetchState: () => {},
    };
    const run = new Function(...Object.keys(scope),
        `${fnSource("findParentOf")}\n${fnSource("clearChildren")}\nreturn clearChildren(findModuleByName(state.modules, ${JSON.stringify(name)}));\n` +
        `function findModuleByName(ms, n) { for (const m of ms) { if (m.name === n) return m; const f = findModuleByName(m.children || [], n); if (f) return f; } return null; }`);
    return run(...Object.values(scope)).then(() => sent);
}

const tree = [
    { name: "Effects", children: [{ name: "Layer", children: [{ name: "Fire" }, { name: "Mirror" }] }] },
    { name: "Drivers", children: [{ name: "Preview" }, { name: "NetworkSend" }] },
];

test("clearing a top-level container sends one PATCH that replaces its children with none", async () => {
    const sent = await clearWith(tree, "Effects");
    assert.deepEqual(sent, [{ url: "/api/state", method: "PATCH", doc: { Effects: { $patch: "replace" } } }]);
});

test("clearing a nested container addresses it under every ancestor", async () => {
    const sent = await clearWith(tree, "Layer");
    assert.deepEqual(sent[0].doc, { Effects: { Layer: { $patch: "replace" } } });
});
