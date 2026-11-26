// The help's Source link at the device's own build (help.js sourceAtBuild). Run: `node --test test/js/help-source.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { sourceAtBuild } from "../../src/ui/help.js";

const main = "https://github.com/MoonModules/MoonLight/blob/main/src/light/layers/Layer.h";

test("a build id pins the source link to its commit, a trailing + and the date left off", () => {
    assert.equal(sourceAtBuild(main, "84769c65+ · Nov 24 2026"),
        "https://github.com/MoonModules/MoonLight/blob/84769c65/src/light/layers/Layer.h");
    assert.equal(sourceAtBuild(main, "84769c65"), "https://github.com/MoonModules/MoonLight/blob/84769c65/src/light/layers/Layer.h");
});

test("a branch whose name holds a slash is replaced whole", () => {
    assert.equal(sourceAtBuild("https://github.com/MoonModules/MoonLight/blob/fix/foo/src/light/layers/Layer.h", "84769c65"),
        "https://github.com/MoonModules/MoonLight/blob/84769c65/src/light/layers/Layer.h");
});

test("a build that names no commit leaves the link as the help wrote it", () => {
    assert.equal(sourceAtBuild(main, ""), main);
    assert.equal(sourceAtBuild(main, "unknown"), main);
    assert.equal(sourceAtBuild(undefined, "84769c65"), undefined);
});
