// The installer says which release it installs: a stable release by its tag, the moving prerelease by its version, since its tag is always `latest`.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { releaseLabel } from "../../src/ui/install-picker.js";

test("a release reads as its tag, the moving prerelease as latest plus its version", () => {
    assert.equal(releaseLabel({ tag_name: "v6.0.0", name: "v6.0.0" }), "v6.0.0");
    assert.equal(releaseLabel({ tag_name: "latest", name: "6.1.0-dev.20" }), "latest 6.1.0-dev.20");
    assert.equal(releaseLabel({ tag_name: "latest", name: "latest" }), "latest");   // a name that says nothing more is left out
    assert.equal(releaseLabel({ tag_name: "latest" }), "latest");
});
