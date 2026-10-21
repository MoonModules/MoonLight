// The help the app shows comes from a file fetched off the network, so its text is rendered as
// text: escaped first, then the three inline forms the generator keeps (code, bold, absolute links).
// Run: `node --test test/js/ui-help.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { helpMarkdown, safeUrl, chipsMatch, typeDisplayName, walkLinks, COMPILED_EMOJI, SCRIPTED_EMOJI } from "../../src/ui/help.js";

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

// toggleHelp out of app.js, with the consent it shares with MoonWalk, and the browser around them stubbed: what it asks, stores and fetches.
import { fnSource } from "./app-source.mjs";
function toggleWorld({ consent = "", answer = false } = {}) {
    const store = { mm_helpConsent: consent };
    const w = { asked: 0, loads: 0, shown: null, hidden: true };
    const toggle = new Function("document", "localStorage", "confirm", "lsRead", "applyHelpShown", "loadHelp", "helpModules",
        "const LS_HELP_CONSENT = 'mm_helpConsent', LS_HELP_SHOWN = 'mm_helpShown';\n" + fnSource("docsSiteConsent") + "\n" + fnSource("toggleHelp") + "\nreturn toggleHelp;")(
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

function linkWorld({ inHelp = true, blocked = false, meta = false, walk = undefined } = {}) {
    const w = { opened: [], walked: [], prevented: false, popup: { opener: "app", location: {}, focused: false, focus() { this.focused = true; } } };
    const open = new Function("window", "openWalk", fnSource("openHelpLink") + "\nreturn openHelpLink;")(
        { open: (...args) => { w.opened.push(args); return blocked ? null : w.popup; } }, (at) => w.walked.push(at));
    open({ target: { closest: () => (inHelp ? { href: "https://en.wikipedia.org/wiki/Chebyshev_distance", dataset: { walk } } : null) },
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

test("a MoonWalk link in the help opens the map in the app at its step, not a popup", () => {
    const w = linkWorld({ walk: "first-light-show/grid-size" });
    assert.deepEqual(w.walked, ["first-light-show/grid-size"]);
    assert.equal(w.opened.length, 0);
    assert.equal(w.prevented, true);
});

test("a link outside the help, or a modified click, is left to the browser", () => {
    assert.equal(linkWorld({ inHelp: false }).opened.length, 0);
    assert.equal(linkWorld({ meta: true }).opened.length, 0);
});

test("a list renders as one, its items formatted", () => {
    assert.equal(helpMarkdown("- **Pin**: a strip\n- *Network*: Art-Net"), "<ul><li><strong>Pin</strong>: a strip</li><li><em>Network</em>: Art-Net</li></ul>");
});

test("a star inside code or a link's address stays literal, and a link's text may be emphasized", () => {
    assert.equal(helpMarkdown("Globs: `src/**/*.cpp` and `improv_*`, *all* of them."), "Globs: <code>src/**/*.cpp</code> and <code>improv_*</code>, <em>all</em> of them.");
    assert.equal(helpMarkdown("[*the* page](https://a.com/*foo*)"), '<a href="https://a.com/*foo*" target="_blank" rel="noopener"><em>the</em> page</a>');
});

test("a heading renders one level under the view's own", () => {
    assert.equal(helpMarkdown("## What you built\n\nA layout."), "<h4>What you built</h4><br><br>A layout.");
});

test("a clip on its own line plays with its poster, a picture shows, and anything not https stays text", () => {
    assert.equal(helpMarkdown("![A grid](https://moonmodules.org/a.webm)"), '<video src="https://moonmodules.org/a.webm" poster="https://moonmodules.org/a.jpg" controls playsinline title="A grid"></video>');
    assert.equal(helpMarkdown("![A card](https://moonmodules.org/a.png)"), '<img src="https://moonmodules.org/a.png" alt="A card">');
    assert.equal(helpMarkdown("![x](javascript:alert(1))"), "![x](javascript:alert(1))");
});

test("a clip that ends the text still plays", () => {
    assert.equal(helpMarkdown("Watch it.\n\n![A wall](https://moonmodules.org/b.webm)\n"), 'Watch it.<br><br><video src="https://moonmodules.org/b.webm" poster="https://moonmodules.org/b.jpg" controls playsinline title="A wall"></video>');
});

test("a fenced block keeps its text as written, escaped and unformatted", () => {
    assert.equal(helpMarkdown("```\nxattr -dr **x** <app>\n```"), "<pre><code>xattr -dr **x** &lt;app&gt;</code></pre>");
});

test("a fenced block with a blank line inside stays one block", () => {
    assert.equal(helpMarkdown("Run:\n\n```\nfirst\n\nsecond\n```"), "Run:<br><br><pre><code>first\n\nsecond</code></pre>");
});

test("a NUL in the text cannot pose as a code block", () => {
    assert.equal(helpMarkdown("a\u00000\u0000b"), "a0b");
});

const walk = {
    processes: { show: { title: "Build your first light show" }, wifi: { title: "Join your WiFi" } },
    byModule: { GridLayout: ["show/grid-width", "show/grid-height"], Ghost: ["gone/s"] },
    byControl: { "GridLayout.width": ["show/grid-width"] },
};
const PAGE = "https://moonmodules.org/MoonLight/moonwalk.html";

test("a module's help lists each MoonWalk process it takes part in once, opening at its first step", () => {
    assert.deepEqual(walkLinks(walk, "GridLayout", null, PAGE),
        [{ label: "MoonWalk: Build your first light show", href: `${PAGE}#show/grid-width`, ref: "show/grid-width" }]);
});

test("a control's help names the step that sets it", () => {
    assert.deepEqual(walkLinks(walk, "GridLayout", "height", PAGE), []);
    assert.equal(walkLinks(walk, "GridLayout", "width", PAGE)[0].ref, "show/grid-width");
});

test("a walk link's address is the host's page with the ids encoded, so the file cannot choose where it leads", () => {
    const odd = { processes: { 'x"><img': { title: "T" } }, byModule: { M: ['x"><img/s?#'] } };
    assert.equal(walkLinks(odd, "M", null, PAGE)[0].href, `${PAGE}#x%22%3E%3Cimg/s%3F%23`);
});

test("without MoonWalk's index, or for a process the index lacks, the help shows no link", () => {
    assert.deepEqual(walkLinks(null, "GridLayout", null, PAGE), []);
    assert.deepEqual(walkLinks(walk, "Ghost", null, PAGE), []);
});
