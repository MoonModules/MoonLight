// MoonCloud card contracts, pinned in the source the way ui-visibility does.
//
// Every MoonCloud bug found by hand lived in app.js, not in the firmware: the C++ tests
// were right that the device posted the message and cleared its control, while the browser showed
// stale text and a stale board. That is the class these pin.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const app = readFileSync(join(ROOT, "src", "ui", "app.js"), "utf8");

test("MoonTalk adds no special case to the shared control path", () => {
    // MoonTalk is a proof of the plumbing a later member reuses, so it earns its place by adding
    // NOTHING to the UI. An earlier shape carried twelve special cases here: two module-scoped
    // globals, a control renderer branch keyed on the literal control name, an incremental message
    // cache and a reload callback. Each was a workaround for the one before it, and each broke the
    // next thing. A second member has to be able to follow the same path.
    for (const leak of ["moonTalkReload", 'ctrl.name === "message"']) {
        assert.ok(!app.includes(leak),
                  `${leak} is a MoonTalk special case in the shared UI`);
    }

    // A session cache is NOT a special case: Talk and Stats each keep one, both cleared by the same
    // shared rule. It was on this list while Talk had a bespoke incremental cache with its own
    // reload callback; what made that a leak was the callback and the control-name branch, not the
    // caching. Both members now cache the same way.
    assert.ok(app.includes("let moonTalkCache") && app.includes("let moonCloudStatsCache"),
              "both MoonCloud members cache per session, or neither should");
    // Stats caches PER FILTER, so clicking a slice refetches and clicking back is free. A single
    // slot would have served the previous filter's numbers under the new heading.
    assert.match(app, /moonCloudStatsCache\.has\(query\)/,
                 "the stats cache must be keyed on the active filter");
});

test("the card re-reads only on a write that changed the server, once", () => {
    // Pressing `send` publishes a message and answering `consent` sends a report. Typing in
    // `message` or toggling `shareName` changes only the device, and re-reading after those showed
    // exactly what was already on screen.
    const i = app.indexOf('mod?.type === "MoonTalkModule" ? (controlName === "send"');
    assert.ok(i > 0, "no MoonCloud refresh rule keyed on the write that changes the server");

    // BOTH members rebuild on their own consent: the rebuild is what shows or hides the data, and
    // without it a Talk consent change did nothing until the page was reloaded.
    const trigger = app.slice(i, i + 260);
    assert.match(trigger, /controlName === "send" \|\| controlName === "consent"/,
                 "Talk must rebuild on its own consent, not only on send");
    assert.match(trigger, /"MoonStatsModule" \? \(controlName === "consent" \|\| controlName === "send update"\)/,
                 "Stats rebuilds on its consent, and on the button that re-sends its report");
    const rule = app.slice(i, app.indexOf("\n        }", i) + 1);
    assert.ok(rule.includes("refetchState"), "the rule must refresh the card");
    assert.ok(!/\.value\s*=\s*""/.test(rule), "it must not reach into the DOM");
    assert.ok(!app.includes('["MoonStatsModule", "MoonTalkModule"].includes(mod?.type)'),
              "matching the whole module type re-reads on every keystroke in `message`");

    // Switching consent OFF changes nothing on the server, so the card redraws from what it has.
    assert.match(rule, /if \(!turnedOn\) \{ moonCloudGeneration\+\+; refetchState\(\); return true; \}/,
                 "switching off must not clear the cache or re-read");

    // Switching ON reads ONCE, after a delay: the report leaves from tick1s rather than during the
    // write, so an immediate read shows the totals without this device's own install. An earlier
    // shape read three times and rebuilt the card three times with it.
    assert.ok(!/for \(const delay of \[/.test(rule),
              "one read, not a retry loop: each one rebuilds the card");
    assert.equal((rule.match(/refetchState\(\)/g) || []).length, 2,
                 "exactly two call sites: the off path and the single delayed on path");
});

test("both MoonCloud fetches use one compiled-in address", () => {
    // There is ONE MoonCloud, so its address is a constant rather than a control. A field on the
    // card invited editing a value whose only correct setting is the default, and a typo there
    // means reports vanish silently, because a failed report is never retried. Moving the server
    // is a release.
    assert.ok(app.includes("const kMoonCloudUrl ="), "the address must be a single constant");
    for (const path of ["/api/stats", "/api/talk"]) {
        assert.ok(app.includes(`kMoonCloudUrl + "${path}"`),
                  `${path} must be built from that constant`);
    }
    // And nothing reads it from state any more.
    assert.ok(!app.includes("moonCloudBase("), "the per-container resolver is gone");
    assert.ok(!app.includes("moonTalkUrl("), "the per-child resolver is gone");
});


test("a card that cannot reach MoonCloud says so", () => {
    // Found on the bench: a stale negative DNS entry made the host unresolvable to the browser
    // while the firmware's own POSTs kept succeeding. Messages were arriving on the server and the
    // board rendered empty, with nothing on screen to say why. The device looked broken and was not.
    //
    // A rejected fetch, a non-ok status and a genuinely empty answer are three different facts. Only
    // the last one may render as absence, because the reader's next action differs: check the
    // network, versus wait for someone to post.
    // Both readers must reject a non-ok response rather than folding it into a null body: `r.ok ?
    // r.json() : null` was the shape that made a 500 indistinguishable from an empty board.
    const rejects = app.match(/r\.ok \? r\.json\(\) : Promise\.reject/g) || [];
    assert.equal(rejects.length, 2,
                 "both /api/stats and /api/talk must treat a non-ok status as a failure");

    // And neither may swallow the failure silently.
    assert.ok(!app.includes("/* no section: the card is complete without it */"),
              "an unreachable stats server must render a message, not drop the section");
    for (const draw of ["draw(null, false)"]) {
        assert.ok(app.includes(draw), `${draw} must report the failure to the renderer`);
    }
    assert.ok(app.includes('"Cannot reach the MoonCloud server."'),
              "the failure must be stated in words, not left as an empty card");
});

test("a clicked slice filters every chart, and says so", () => {
    // The plan's reasoning: a filter has to be something the reader chose, or it is a hidden default
    // with extra steps. So every chart counts everything until a slice is clicked, and when one is,
    // the card states what it narrowed to and offers the way back.
    assert.match(app, /class="mooncloud-filter"|mooncloud-filter"/,
                 "an active filter must be stated on the card");
    assert.ok(app.includes("Clear"), "an active filter must offer a way back");

    // `other` is a bucket of everything past the top slices, so it names no value to filter by: the pie never filters by it, and the legend unfolds it instead (ui-mooncloud-other).
    const guards = app.match(/r\.name !== "other"/g) || [];
    assert.equal(guards.length, 1, "the pie may not make `other` a filter");

    // Build maps to `dev`, whose stored values are 0 and 1 rather than the labels shown.
    assert.match(app, /name === "development" \? "1" : "0"/,
                 "the Build pie must translate its labels to the dev flag");
});

test("Enter in a text field presses the card's send button", () => {
    // Generic rather than a MoonTalk rule: any module pairing a text field with a `send` button gets
    // it, and a module without one is untouched. That is what keeps the shared control path free of
    // per-module branches.
    const i = app.indexOf('input.addEventListener("keydown"');
    assert.ok(i > 0, "a text control must handle Enter");
    // Bounded by the handler's own closing brace, not a character count: a fixed window stops
    // covering the assertions below the moment a comment line is added.
    const handler = app.slice(i, app.indexOf("\n                });", i) + 1);

    assert.ok(handler.includes('e.key !== "Enter"'), "it must act only on Enter");
    assert.ok(handler.includes('c.name === "send"'),
              "it must look for a send button rather than naming a module");

    // The write is flushed and awaited before the press. Typing is debounced 500 ms, so pressing
    // send first would publish the text as it stood one keystroke ago.
    assert.ok(handler.includes("cancelSend(key)"),
              "the pending debounced write must be cancelled");
    assert.match(handler, /await write\(ctrl\.name, input\.value\);\s*\n\s*await write\("send", 1\)/,
                 "the text must be written and awaited BEFORE send is pressed");

    // Talk is a MoonCloud child, so the lookup has to walk the tree.
    assert.ok(handler.includes("allModules()"),
              "a nested module's controls are not reachable from state.modules directly");
});


test("consent has no reset-to-default button", () => {
    // Off is where consent starts, so the button's only possible action is to withdraw it: a
    // decision, not a reset. It joins the existing surface-control opt-out rather than getting a
    // mechanism of its own, and the checkbox already expresses both answers.
    const i = app.indexOf("function appendResetButton(");
    assert.ok(i > 0, "no reset-button renderer");
    const fn = app.slice(i, app.indexOf("\n}", i) + 2);
    assert.match(fn, /ctrl\.name === "consent"/,
                 "consent must opt out of the reset button");
    assert.ok(fn.includes("ctrl.fader || ctrl.encoder || ctrl.switchRow"),
              "it opts out through the existing guard, not a second one");
});


test("without consent nothing is fetched, and the card still shows its shape", () => {
    // Reading carries no identifier, but it is still a request to our server from a device whose
    // owner said no. The chart headings are a constant in this file, so the card can draw its own
    // shape without asking anyone.
    assert.ok(!app.includes("function zeroed(stats)"),
              "zeroing a fetched answer means it was fetched: the request is what consent gates");

    for (const fn of ["renderMoonCloudStats", "renderMoonTalk"]) {
        const i = app.indexOf("const load = ", app.indexOf("function " + fn + "("));
        const load = app.slice(i, app.indexOf("\n    };", i));
        assert.match(load, /if \(!consented\(mod\)\)/,
                     `${fn} must not fetch without consent`);
    }

    // A zero-row pie draws no slices at all, which rendered as a bare sliver: the placeholder is a
    // shape of its own.
    assert.ok(app.includes("mooncloud-pie-placeholder"),
              "an empty chart needs a placeholder, not an empty SVG");
});



test("without consent the board says it was not read, not that it is empty", () => {
    // Three states, three different facts: never asked, asked and empty, asked and unreachable.
    // Collapsing the first two says the board is empty when nobody looked, which is the same
    // mistake as rendering an unreachable server as an empty one.
    const i = app.indexOf("function renderMoonTalk(");
    const fn = app.slice(i, app.indexOf("\nfunction ", i + 1));

    assert.match(fn, /const draw = \(messages, reached = true, asked = true\)/,
                 "draw must be able to tell `not asked` from `empty`");
    assert.match(fn, /if \(!consented\(mod\)\) \{[^}]*draw\(\[\], true, false\); return; \}/,
                 "the unconsented path must say it did not ask");
    assert.ok(fn.includes("Turn on consent to read the board."),
              "and say so in words a reader can act on");
    assert.ok(fn.includes("No messages yet."),
              "an actually-empty board still says so");

    // The unconsented draw must not be CACHED: a cached `[]` survived the rebuild that consenting
    // triggers, the cache hit returned it, and the board stayed blank until a manual refresh.
    assert.match(fn, /if \(!consented\(mod\)\) \{ moonTalkCache = null;/,
                 "an empty draw from an unconsented card is not an answer to remember");
});


test("the consent explanation is a module status, not a bespoke UI block", () => {
    // Every module already has a status slot and the card already renders it, so a hand-rolled
    // element beside the checkbox was a second mechanism for a job the first one does. The text
    // lives with the module that knows what it exchanges, which is also what the privacy policy
    // points at instead of carrying a list.
    assert.ok(!app.includes("renderMoonCloudConsent"),
              "no bespoke consent renderer: the status row shows it");
    assert.ok(!app.includes("mooncloud-consent"),
              "and no bespoke element for it either");
});
