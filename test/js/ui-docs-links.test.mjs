// Every link from the UI into the MoonLight docs lands on a page that exists, at a heading that exists, so a link never ends in a 404.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const ui = join(root, "src", "ui");
const docs = join(root, "docs");

// mkdocs' heading anchor: lowercase, punctuation dropped, spaces as hyphens.
const slug = (heading) => heading.trim().toLowerCase().replace(/[^\w\s-]/g, "").replace(/\s+/g, "-");

test("every docs link in the UI points at an existing page and heading", () => {
    const links = [];
    for (const file of readdirSync(ui).filter((f) => /\.(js|html)$/.test(f))) {
        const text = readFileSync(join(ui, file), "utf8");
        for (const m of text.matchAll(/https:\/\/moonmodules\.org\/MoonLight\/([\w/.-]+)\.html(?:#([\w-]+))?/g)) links.push({ file, page: m[1], anchor: m[2] });
    }
    assert.ok(links.length > 0, "the scan found the UI's docs links");
    for (const { file, page, anchor } of links) {
        const md = join(docs, page + ".md");
        assert.ok(existsSync(md), `${file}: ${page}.html has no docs/${page}.md`);
        if (anchor) {
            const headings = readFileSync(md, "utf8").split("\n").filter((l) => l.startsWith("#")).map((l) => slug(l.replace(/^#+/, "")));
            assert.ok(headings.includes(anchor), `${file}: ${page}.html has no heading #${anchor}`);
        }
    }
});
