// The module picker offers scripts and compiled modules as one list.
//
// To a user, adding `dot` and adding `DemoReel` are the same gesture: one is a MoonLive script and
// the other is compiled, which is a property of the row rather than a category of its own. So the
// picker merges them, sorts them together, and marks the scripted ones. These tests pin the three
// things that make that readable: the marker, the ordering, and the two filter chips.
//
// Read out of app.js rather than imported: the file is a browser script with no module boundary,
// and the alternative (a DOM harness driving the real picker) proved far slower to steer than the
// logic is to check directly.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { src, fnSource } from "./app-source.mjs";

const SCRIPTED = "\u{1F4DD}";
const COMPILED = "\u{1F4E6}";      // matches the source: NOT the gear, which is the generic role

const CATALOG = {
    dir: "/.moonlive",
    effects: { names: ["balls.mle", "aim.mle"], tags: ["\u{1F4AB}", "\u{1F4AB}"], dim: [2, 2] },
    layouts: { names: ["grid.mll"], tags: [""], dim: [2] },
    modifiers: { names: [], tags: [], dim: [] },
};

/// mlScriptItems, wired to a fixed catalog and a device holding only balls.mle.
function scriptItems(roles) {
    const build = new Function(
        "SCRIPTED_EMOJI", "COMPILED_EMOJI", "mlFetchCatalog", "fmFetchDir",
        `${fnSource("mlTypeForRole")}\n${fnSource("mlScriptItems")}\nreturn mlScriptItems;`);
    return build(SCRIPTED, COMPILED,
                 async () => CATALOG,
                 async () => [{ name: "balls.mle" }])(roles);
}

test("a script becomes a picker row named after the file, without its extension", async () => {
    const items = await scriptItems(["effect"]);
    assert.deepEqual(items.map(i => i.script).sort(), ["aim.mle", "balls.mle"]);
    const balls = items.find(i => i.script === "balls.mle");
    // The name a card would take, and the file it would load: the user picks "balls", not "balls.mle".
    assert.equal(balls.displayName, "balls");
    assert.equal(balls.role, "effect");
});

test("every script row carries the scripted marker, whatever its own tags say", async () => {
    const items = await scriptItems(["effect"]);
    for (const i of items) assert.ok(i.tags.includes(SCRIPTED), `${i.script} is unmarked`);
});

test("a script the device does not hold is marked as a download before it is chosen", async () => {
    const items = await scriptItems(["effect"]);
    const aim = items.find(i => i.script === "aim.mle");     // absent from the device
    const balls = items.find(i => i.script === "balls.mle"); // present
    assert.equal(aim.remote, true);
    assert.equal(balls.remote, false);
    assert.ok(aim.displayName.startsWith("☁"), "a remote row says so before it is picked");
});

test("only the roles a parent accepts are offered", async () => {
    const effects = await scriptItems(["effect"]);
    assert.ok(effects.every(i => i.role === "effect"));
    // A role with no scripted form contributes nothing rather than throwing.
    assert.deepEqual(await scriptItems(["driver"]), []);
});

test("scripts and compiled modules sort as one alphabetical list, markers ignored", async () => {
    // The picker's own sort key, which strips a leading marker so a prefixed row still sorts by its
    // name: without this the cloud rows collect in a block instead of sitting where they are looked for.
    const sortKey = (t) => (t.displayName || t.name).replace(/^[^\p{L}\p{N}]+/u, "");
    const compiled = [
        { name: "DemoReel", displayName: "DemoReel", role: "effect", tags: "" },
        { name: "SolidEffect", displayName: "Solid", role: "effect", tags: "" },
    ];
    const merged = [...compiled, ...(await scriptItems(["effect"]))]
        .sort((a, b) => sortKey(a).localeCompare(sortKey(b)));
    assert.deepEqual(merged.map(m => sortKey(m)), ["aim", "balls", "DemoReel", "Solid"]);
});

test("the two kind chips partition the list, the compiled one matching by absence", async () => {
    const compiled = [{ name: "DemoReel", displayName: "DemoReel", role: "effect", tags: "" }];
    const merged = [...compiled, ...(await scriptItems(["effect"]))];
    // A compiled module carries no marker of its own, so its chip matches what LACKS the scripted
    // one. That is what keeps ninety existing modules from needing a new tag.
    const isScripted = (m) => (m.tags || "").includes(SCRIPTED);
    const scripted = merged.filter(isScripted);
    const notScripted = merged.filter(m => !isScripted(m));
    assert.equal(scripted.length, 2);
    assert.equal(notScripted.length, 1);
    assert.equal(scripted.length + notScripted.length, merged.length, "the chips partition the list");
});

test("the picker's chip groups lead with the two kinds", () => {
    // Order is the legend's: what a row IS, then role, then dimension, then origin. A reader scanning
    // the chip row should meet scripted/compiled first, because that is the coarsest split.
    const groups = src.slice(src.indexOf("const CHIP_GROUPS = ["));
    const firstGroup = groups.slice(0, groups.indexOf("]", groups.indexOf("[", 20)) + 1);
    assert.ok(firstGroup.includes("SCRIPTED_EMOJI") && firstGroup.includes("COMPILED_EMOJI"),
              "the first chip group is the scripted/compiled pair");
});

test("a replace always renames the card, whichever kind of row was picked", () => {
    // A card is named after what it RUNS, so swapping what it runs renames it. Both branches of
    // replacePickedType pass a name: the compiled one was the bug (a card auto-named "MoonLive-3"
    // stayed "MoonLive-3" after becoming a Lissajous), because the device preserves any name that is
    // not the old type's default and cannot tell a generated name from a chosen one.
    const body = fnSource("replacePickedType");
    const compiled = body.slice(body.indexOf("if (!item.script)"));
    assert.match(compiled.slice(0, 120), /replaceModule\([^)]*picked\)/,
                 "the compiled branch must pass the picked name");
    assert.match(body.slice(body.indexOf("mlTypeForRole")), /replaceModule\([^)]*picked\)/,
                 "the script branch must pass the picked name");
    // And the name is the row's, with the cloud marker stripped: that glyph says a download is
    // coming, it is not part of what the card is called.
    assert.match(body, /displayName\.replace\(\/\^\\u2601/,
                 "the cloud marker is stripped before the name is used");
});

test("pointing a card at a script re-renders it, so a failing script still shows its source", () => {
    // A card is built BEFORE its script is set: created or replaced first, pointed at the file
    // second. Something has to rebuild it, or the picker keeps reading "(none)" over an empty editor.
    //
    // A script that COMPILES hides the gap, which is why this needs pinning: defining controls
    // changes the module's schema, the device fires a full resync, and the card is rebuilt for free.
    // A script that FAILS defines nothing, the schema signature is unchanged, so only the status text
    // arrives and the card ends up reporting an error about a script it claims not to have. Worse, the
    // editor holds no text, so the error's offset converts against nothing and reads "line 1, col 1".
    const body = fnSource("setCardScript");
    assert.match(body, /sendControl\([^)]*"script"/, "it sets the script control");
    assert.match(body, /refetchState\(\)/, "and re-renders, which is the whole point");
    assert.ok(body.indexOf("sendControl") < body.indexOf("refetchState"),
              "the re-render must come after the value it is meant to show");

    // Both entry points go through it: a fix in one path only is the bug waiting to come back.
    assert.match(fnSource("addPickedType"), /setCardScript\(/, "the add path uses it");
    assert.match(fnSource("replacePickedType"), /setCardScript\(/, "the replace path uses it");
});

test("a replaced card is found by POSITION, so a name collision still reaches the right module", () => {
    // The device keeps names unique, so replacing a card with a script whose name is already taken
    // in that layer gives it a suffix ("dot-2"). Everything after the replace therefore has to find
    // the slot WITHOUT its old name, which no longer exists, and without assuming it got the name it
    // asked for: looking it up by the requested name matched the OTHER card and sent the script
    // there, and looking the parent up by the old name found nothing at all, so the script was never
    // applied. A replace swaps a child in place, so its index is the one handle that survives.
    const body = fnSource("replacePickedType");
    const replaceAt = body.indexOf("await replaceModule(");
    assert.ok(body.indexOf("findParentOf(targetMod.name)") < replaceAt,
              "the parent must be captured BEFORE the replace, while the old name still resolves");
    assert.ok(body.indexOf("findIndex(") < replaceAt,
              "and so must the index, for the same reason");
    assert.match(body.slice(replaceAt), /children\[index\]/,
                 "afterwards the slot is read at that index, not by a name that may not be its own");
});

// A FORK THAT CHANGES NOTHING IS NOT A FORK.
//
// Opening a shipped script and pressing Save (or clicking away, which also saves) used to write a
// user copy byte-identical to the shipped one. That copy then shadows the library forever: every
// later update is invisible behind a "change" the user never made. The editor therefore compares
// what is on screen against what it loaded, and writes nothing when they match.
//
// Pinned on the SOURCE rather than through a DOM harness for the reason at the top of this file:
// the guard lives inside fmMountEditor's closure, and what matters is that all three of its parts
// are present, in the right order, against the right variable.
test("saving an unmodified factory script does not create a shadowing copy", () => {
    const editor = fnSource("fmMountEditor");

    // The text as loaded is remembered, so an untouched file can be recognized.
    assert.match(editor, /loadedText = body\.value/,
                 "fmMountEditor must remember the text it loaded");

    // The guard: only when the write goes somewhere other than the read (a fork), and only when
    // nothing changed. Both conditions, or it would suppress a real save.
    const guard = /dest !== path && loadedText !== null && saved === loadedText/;
    assert.match(editor, guard, "the no-op fork guard must test destination AND content");

    // It must sit BEFORE the write, or the file is created and the check is decoration.
    const guardAt = editor.search(guard);
    const writeAt = editor.indexOf("fmSaveFrom(body, dest)");
    assert.ok(guardAt > -1 && writeAt > -1 && guardAt < writeAt,
              "the guard must run before the write it prevents");
});
