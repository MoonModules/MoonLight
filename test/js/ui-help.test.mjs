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

test("a table renders with its first row as the head, its cells formatted", () => {
    assert.equal(helpMarkdown("Pick one.\n\n| Want | Use |\n|---|---|\n| Many strands | `i80` |"),
        "Pick one.<br><br><table><tr><th>Want</th><th>Use</th></tr><tr><td>Many strands</td><td><code>i80</code></td></tr></table>");
});

test("a table without a separator row keeps every body row", () => {
    assert.equal(helpMarkdown("| Want | Use |\n| Few | RMT |"),
        "<table><tr><th>Want</th><th>Use</th></tr><tr><td>Few</td><td>RMT</td></tr></table>");
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

// toggleHelp out of app.js, with the browser around it stubbed: what it asks, stores and fetches.
import { fnSource } from "./app-source.mjs";
function toggleWorld({ consent = "", answer = false } = {}) {
    const store = { mm_helpConsent: consent };
    const w = { asked: 0, loads: 0, shown: null, hidden: true };
    const toggle = new Function("document", "localStorage", "confirm", "lsRead", "applyHelpShown", "loadHelp", "helpModules",
        "const LS_HELP_CONSENT = 'mm_helpConsent', LS_HELP_SHOWN = 'mm_helpShown';\n" + fnSource("toggleHelp") + "\nreturn toggleHelp;")(
        { body: { classList: { contains: () => w.hidden } } },
        { setItem: (k, v) => { store[k] = v; } },
        () => { w.asked++; return answer; },
        (k, d) => (k in store ? store[k] : d),
        (on) => { w.shown = on; },
        () => { w.loads++; },
        {});
    return { toggle, store, w };
}

test("turning help on asks first, and a no downloads and stores nothing", () => {
    const { toggle, store, w } = toggleWorld({ answer: false });
    toggle();
    assert.equal(w.asked, 1);
    assert.equal(w.loads, 0);
    assert.equal(store.mm_helpConsent, "");
    assert.equal(w.shown, null);
});

test("a yes is kept, so help downloads and the question is not asked again", () => {
    const first = toggleWorld({ answer: true });
    first.toggle();
    assert.equal(first.store.mm_helpConsent, "1");
    assert.equal(first.w.loads, 1);
    assert.equal(first.w.shown, true);
    const later = toggleWorld({ consent: "1" });
    later.toggle();
    assert.equal(later.w.asked, 0);
    assert.equal(later.w.loads, 1);
});

function linkWorld({ inHelp = true, blocked = false, meta = false } = {}) {
    const w = { opened: [], prevented: false, popup: { opener: "app", location: {}, focused: false, focus() { this.focused = true; } } };
    const open = new Function("window", fnSource("openHelpLink") + "\nreturn openHelpLink;")(
        { open: (...args) => { w.opened.push(args); return blocked ? null : w.popup; } });
    open({ target: { closest: () => (inHelp ? { href: "https://en.wikipedia.org/wiki/Chebyshev_distance" } : null) },
           button: 0, metaKey: meta, preventDefault: () => { w.prevented = true; } });
    return w;
}

test("a help link opens in one named popup with its opener cut, so the app stays open", () => {
    const w = linkWorld();
    assert.deepEqual(w.opened, [["", "mm-help", "width=960,height=720"]]);
    assert.equal(w.prevented, true);
    assert.equal(w.popup.opener, null);
    assert.equal(w.popup.location.href, "https://en.wikipedia.org/wiki/Chebyshev_distance");
    assert.equal(w.popup.focused, true);
});

test("a blocked popup leaves the link to open its own tab", () => {
    const w = linkWorld({ blocked: true });
    assert.equal(w.prevented, false);
});

test("a link outside the help, or a modified click, is left to the browser", () => {
    assert.equal(linkWorld({ inHelp: false }).opened.length, 0);
    assert.equal(linkWorld({ meta: true }).opened.length, 0);
});
