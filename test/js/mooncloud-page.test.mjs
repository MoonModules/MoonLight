// The public stats page folds every chart past its 7th slice into "other" and unfolds it in the legend, from the same worker module the server runs.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../mooncloud/worker.js", import.meta.url), "utf8");

test("the worker loads as a module, its page included", async () => {
    // The page is a template string, where a CSS escape such as "\25B8" is an octal escape that stops the whole worker loading.
    const worker = await import(new URL("../../mooncloud/worker.js", import.meta.url));
    assert.equal(typeof worker.default.fetch, "function");
});

test("other holds the rows it folds, and the legend unfolds them as links", () => {
    const start = source.indexOf("function topSlices(");
    const fn = source.slice(start, source.indexOf("\n}\n", start) + 2);
    const topSlices = new Function(`${fn} return topSlices;`)();
    const rows = Array.from({ length: 9 }, (_, k) => ({ name: `e${k}`, count: 9 - k }));
    const other = topSlices(rows)[7];
    assert.equal(other.name, "other");
    assert.deepEqual(other.rest.map((r) => r.name), ["e7", "e8"]);
    assert.equal(other.count, 2 + 1);
    const legend = source.slice(source.indexOf("function legend("), source.indexOf("function legendLine("));
    assert.ok(legend.includes('createElement("details")') && legend.includes("legendLine(row, null, key)"));
});
