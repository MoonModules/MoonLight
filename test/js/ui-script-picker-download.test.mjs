// A factory script is a NAME on the device until someone picks it: the catalog carries names only
// and the browser fetches the text from GitHub. So every path that points a module at a script has
// to make sure the file is there first, or the card reports "script not found" and nothing renders.
//
// `mlEnsureLocal` is that rule. The module picker calls it, and its own docstring says it "runs
// first on both paths" - but the script dropdown had a second copy of the same logic, gated on its
// own `remote` list, and that copy is the one that failed: picking a script left the module still
// pointing at the previous one, with no error shown.
//
// Two mechanisms for one job is the debt this pins down. app.js is a browser script rather than a
// module, so this reads the source: a behavioral test would need a DOM and a device.
//
// Run: `node --test "test/js/**/*.test.mjs"`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const src = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

test("one helper owns making a picked script local", () => {
    assert.match(src, /async function mlEnsureLocal\(/,
        "mlEnsureLocal is the single place a picked script is downloaded before it is used");
});

test("the script dropdown goes through that helper rather than its own copy", () => {
    // The dropdown's change handler: from the listener to the sendControl that commits the pick.
    const start = src.indexOf('picker.addEventListener("change"');
    assert.ok(start > 0, "the script dropdown still has a change handler");
    const handler = src.slice(start, src.indexOf("sendControl", start));
    assert.match(handler, /mlEnsureLocal\(/,
        "the dropdown asks the shared helper to make the file local");
    assert.doesNotMatch(handler, /mlDownloadScript\(/,
        "a second download path here is what left the module pointing at the previous script");
});
