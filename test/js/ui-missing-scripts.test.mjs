// A module naming a shipped script the device lacks lists it on System; the open page downloads it, the picker's own path.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

/// The page's download loop over a stand-in device: `missing` is System's hidden control, and each download is recorded.
function page(missing, failing = []) {
    const downloads = [];
    const modules = [{ name: "Effects", controls: [] }, { name: "System", controls: [{ name: "missing scripts", value: missing }] }];
    const run = new Function("allModules", "mlDownloadScript",
        `let mlMissingBusy = false; const mlMissingTried = new Map();
         ${fnSource("mlGroupForExt")} ${fnSource("downloadMissingScripts")} return downloadMissingScripts;`)(
        () => modules,
        async (name, group) => {
            downloads.push([name, group]);
            if (failing.includes(name)) throw new Error("download failed");
        });
    return { downloads, run, modules };
}

test("each missing script downloads into its own kind's folder", async () => {
    const p = page("stadbeest-walk.mle|stadbeest-fire.mlp|jsondemo.mls");
    await p.run();
    assert.deepEqual(p.downloads, [["stadbeest-walk.mle", "effects"], ["stadbeest-fire.mlp", "palettes"], ["jsondemo.mls", "services"]]);
});

test("nothing missing downloads nothing", async () => {
    const p = page("");
    await p.run();
    assert.deepEqual(p.downloads, []);
});

test("a name is tried once a minute at most, so every state patch does not repeat it", async () => {
    const p = page("a.mle", ["a.mle"]);
    await p.run();
    await p.run();
    assert.equal(p.downloads.length, 1);
});

test("a name that is not a script is left alone", async () => {
    const p = page("notes.txt|x.mle");
    await p.run();
    assert.deepEqual(p.downloads, [["x.mle", "effects"]]);
});
