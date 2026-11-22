// The installer's layout choice: the board's measured flash picks the table among those the firmware's manifest lists,
// the same rule as MoonDeck's serial flash (test/python/test_flash_layout.py). Run: `node --test test/js/flash-layout.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { layoutFor } from "../../mooninstaller/flash-layout.js";

const layouts = (...mb) => mb.map((n) => ({ flashSize: `${n}MB`, partitionTable: `t-${n}mb.bin` }));

test("a board gets the layout of its own flash size", () => {
    assert.equal(layoutFor(layouts(8, 16), "16MB").layout.flashSize, "16MB");
    assert.equal(layoutFor(layouts(4, 8, 16), "4MB").layout.flashSize, "4MB");
});

test("more flash than any layout takes the largest that fits", () => {
    assert.equal(layoutFor(layouts(8, 16), "32MB").layout.flashSize, "16MB");
});

test("a board smaller than every layout is refused and told to take a -lite firmware", () => {
    const { layout, note } = layoutFor(layouts(8, 16), "4MB");
    assert.equal(layout, null);
    assert.match(note, /-lite/);
});

test("unread flash takes the smallest layout, which boots on any board the firmware serves", () => {
    assert.equal(layoutFor(layouts(8, 16), undefined).layout.flashSize, "8MB");
});
