// The pie folds everything past its 7th slice into "other", and the legend unfolds it: the rows ride along, so nothing a reader can filter by is hidden.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

const top = new Function(`${fnSource("moonCloudTopSlices")} return moonCloudTopSlices;`)();
const rows = Array.from({ length: 10 }, (_, k) => ({ name: `d${k}`, count: 10 - k }));

test("other holds the rows it folds, in order, and counts them", () => {
    const shown = top(rows);
    assert.equal(shown.length, 8);
    const other = shown[7];
    assert.equal(other.name, "other");
    assert.deepEqual(other.rest.map((r) => r.name), ["d7", "d8", "d9"]);
    assert.equal(other.count, 3 + 2 + 1);
    assert.equal(top(rows.slice(0, 7)).length, 7);   // seven or fewer: no other
});

test("the legend unfolds other into rows that filter", () => {
    const legend = fnSource("moonCloudLegend");
    assert.ok(legend.includes("r.rest") && legend.includes("moonCloudLegendLine(row, null, onPick)"));
});

// A bucketed chart filters by the picked row's bounds, so the rows "other" folds must be found by name too, or picking one does nothing.
test("a picked row inside other filters a bucketed chart by its bounds", () => {
    assert.ok(fnSource("renderMoonCloudStats").includes("shown.flatMap(r => r.rest ? [r, ...r.rest] : [r]).find(r => r.name === name)"));
});
