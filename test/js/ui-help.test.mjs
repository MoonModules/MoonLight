// The help the app shows comes from a file fetched off the network, so its text is rendered as
// text: escaped first, then the three inline forms the generator keeps (code, bold, absolute links).
// Run: `node --test test/js/ui-help.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { helpMarkdown, safeUrl, chipsMatch, typeDisplayName, COMPILED_EMOJI, SCRIPTED_EMOJI } from "../../src/ui/help.js";

test("code, bold and absolute links render", () => {
    assert.equal(helpMarkdown("An `.fseq` file, **fit** or [the docs](https://moonmodules.org/MoonLight/)"),
        'An <code>.fseq</code> file, <strong>fit</strong> or <a href="https://moonmodules.org/MoonLight/" target="_blank" rel="noopener">the docs</a>');
});

test("markup in the text stays text", () => {
    assert.equal(helpMarkdown('<img src=x onerror="alert(1)">'), '&lt;img src=x onerror=&quot;alert(1)&quot;&gt;');
});

test("a link that is not http or https stays text", () => {
    assert.equal(helpMarkdown("[click](javascript:alert(1))"), "[click](javascript:alert(1))");
});

test("paragraphs stay apart", () => {
    assert.equal(helpMarkdown("One.\n\nTwo."), "One.<br><br>Two.");
});

test("a module matches when it carries every active chip", () => {
    assert.equal(chipsMatch(["🔥", "🟦", "🎶"], new Set(["🔥", "🎶"])), true);
    assert.equal(chipsMatch(["🔥", "🟦"], new Set(["🔥", "🎶"])), false);
});

test("the compiled chip matches a module with no scripted marker", () => {
    assert.equal(chipsMatch(["🔥"], new Set([COMPILED_EMOJI])), true);
    assert.equal(chipsMatch(["🔥", SCRIPTED_EMOJI], new Set([COMPILED_EMOJI])), false);
});

test("a type's display name drops its role noun", () => {
    assert.equal(typeDisplayName("BeatRipplesEffect"), "BeatRipples");
    assert.equal(typeDisplayName("Drivers"), "Drivers");
});

test("a link or picture from the file must be https and stay inside its attribute", () => {
    assert.equal(safeUrl("https://github.com/MoonModules/MoonLight/blob/main/src/x.h"), "https://github.com/MoonModules/MoonLight/blob/main/src/x.h");
    assert.equal(safeUrl("javascript:alert(1)"), null);
    assert.equal(safeUrl('https://x" onmouseover="alert(1)'), null);
    assert.equal(safeUrl("http://plain.example/x"), null);
    assert.equal(safeUrl(undefined), null);
});
