// The UI's app.js and the modules it shares with MoonDeck (the device map, its node popup, the install flow, the update rules and the page helpers) as text,
// and one of their top-level functions by name, for a test that runs a function without a browser.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

export const UI_FILES = ["app.js", "deviceinstall.js", "devicemap.js", "devicepopup.js", "releases.js", "uikit.js"];
export const src = UI_FILES.map(f => readFileSync(new URL(`../../src/ui/${f}`, import.meta.url), "utf8")).join("\n");

/// A top-level function's source, by brace matching, `async` prefix included.
export function fnSource(name) {
    const at = src.indexOf(`function ${name}(`);
    assert.notEqual(at, -1, `${name} not found in ${UI_FILES.join(", ")}`);
    const from = src.startsWith("async ", at - 6) ? at - 6 : at;
    // The BODY's brace, not the first one after the name: a defaulted object parameter (`opts = {}`) puts a brace inside the parameter list.
    const open = src.indexOf("{", src.indexOf(")", src.indexOf("(", at)));
    let depth = 0;
    for (let i = open; i < src.length; i++) {
        if (src[i] === "{") depth++;
        else if (src[i] === "}" && --depth === 0) return src.slice(from, i + 1);
    }
    assert.fail(`unbalanced braces in ${name}`);
}
