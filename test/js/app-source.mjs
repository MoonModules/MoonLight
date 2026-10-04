// The UI's app.js as text, and one of its top-level functions by name, for a test that runs a function without a browser.

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

export const src = readFileSync(new URL("../../src/ui/app.js", import.meta.url), "utf8");

/// A top-level function's source, by brace matching, `async` prefix included.
export function fnSource(name) {
    const at = src.indexOf(`function ${name}(`);
    assert.notEqual(at, -1, `${name} not found in app.js`);
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
