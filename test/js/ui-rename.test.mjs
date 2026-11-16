// A renamed module keeps its place in the view: the open tab, the open controls and a resized textarea follow the new name.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { fnSource } from "./app-source.mjs";

function renameWith(view, from, to) {
    const stored = {};
    const scope = {
        selectedTabs: view.selectedTabs,
        expandedSet: view.expandedSet,
        textareaSizes: view.textareaSizes,
        LS_TABS: "tabs",
        LS_TA_SIZE: "sizes",
        safeLocalSet: (key, value) => { stored[key] = JSON.parse(value); },
        saveExpanded: () => { stored.expanded = [...view.expandedSet]; },
    };
    new Function(...Object.keys(scope), `${fnSource("renameViewKeys")}\nrenameViewKeys(${JSON.stringify(from)}, ${JSON.stringify(to)});`)(...Object.values(scope));
    return stored;
}

test("the tab that showed a module stays open under its new name", () => {
    const view = { selectedTabs: { Layer: "NetworkReceive", Drivers: "Preview" }, expandedSet: new Set(), textareaSizes: {} };
    const stored = renameWith(view, "NetworkReceive", "Receiver 1");
    assert.deepEqual(view.selectedTabs, { Layer: "Receiver 1", Drivers: "Preview" });
    assert.deepEqual(stored.tabs, view.selectedTabs);
});

test("a renamed container keeps the tab it had open among its children", () => {
    const view = { selectedTabs: { Layer: "Fire" }, expandedSet: new Set(), textareaSizes: {} };
    renameWith(view, "Layer", "Back");
    assert.deepEqual(view.selectedTabs, { Back: "Fire" });
});

test("open controls and a resized textarea follow the new name, and other modules keep theirs", () => {
    const view = {
        selectedTabs: {},
        expandedSet: new Set(["Script", "Fire"]),
        textareaSizes: { "Script:source": 240, "Scripts:source": 90 },
    };
    const stored = renameWith(view, "Script", "Clock");
    assert.deepEqual([...view.expandedSet].sort(), ["Clock", "Fire"]);
    assert.deepEqual(view.textareaSizes, { "Clock:source": 240, "Scripts:source": 90 });
    assert.deepEqual(stored.sizes, view.textareaSizes);
});
