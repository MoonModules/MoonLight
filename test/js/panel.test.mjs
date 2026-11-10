// The module panel's sizing (panel.js): the height a taller panel gains is shared among its expandable areas.
// Run: `node --test test/js/panel.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { shareExtra } from "../../src/ui/panel.js";

test("the height a panel gains or gives up is shared among its expandable areas in proportion to their natural heights", () => {
    assert.deepEqual(shareExtra([100, 300], 200), [150, 450], "a map three times a text area's height takes three times its share");
    assert.deepEqual(shareExtra([120], 0), [120], "no extra height leaves an area at its natural size");
    assert.deepEqual(shareExtra([0, 0], 50), [0, 0], "areas of no natural height take nothing rather than dividing by zero");
    assert.deepEqual(shareExtra([100, 300], -200), [50, 150], "a smaller panel takes height back in the same proportion");
});
