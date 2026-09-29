// MoonCloud Stats server contract: what reaches storage, and what cannot.
//
// The report endpoint is open to anyone, so its input is untrusted twice over: a field the
// firmware never sends can still arrive, and a field it does send can be any length. These tests
// pin the two rules that keep the privacy policy true on the server side as well as the device
// side: only allowlisted fields are stored, and the country is derived at the edge rather than
// from anything the caller supplies.
//
// Run: `node --test test/js`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../../mooncloud/worker.js", import.meta.url), "utf8");

// The worker is an ES module written for the Workers runtime, so it is exercised here by pulling
// the pure helpers out of it rather than by booting a fake runtime: `clean` is where every
// storage decision is made, and it is the thing worth pinning.
const cleanSource = source.slice(
  source.indexOf("function clean("),
  source.indexOf("async function handleReport")
);
const ALLOWED = JSON.parse(
  source.slice(source.indexOf("const ALLOWED = ["), source.indexOf("];") + 1)
    .replace("const ALLOWED = ", "")
    .replace(/,(\s*)\]/, "]")
    .replace(/\/\/.*$/gm, "")
);
const clean = new Function(
  "ALLOWED", "MAX_STRING", "MAX_MODULES", "MAX_NUMBER",
  `${cleanSource}; return clean;`
)(ALLOWED, 64, 64, 2147483647);

test("a report cannot smuggle a field the firmware never sends", () => {
  const row = clean(
    {
      installationId: "c26086e8da9b6fb2d4b81e4ca71f97e5",
      chip: "ESP32-S3",
      // None of these are in the allowlist. A device would never send them; someone posting by
      // hand might, and the server must not create columns for them.
      deviceName: "ewoud-livingroom",
      mac: "2A:DA:B7:C6:A0:94",
      ssid: "Travelrouter",
      password: "hunter2",
      latitude: "52.37",
    },
    "NL"
  );

  assert.equal(row.deviceName, undefined);
  assert.equal(row.mac, undefined);
  assert.equal(row.ssid, undefined);
  assert.equal(row.password, undefined);
  assert.equal(row.latitude, undefined);
  assert.equal(row.chip, "ESP32-S3");
});

// The event reaches every user's card as a pie legend, so the worker constrains it to the words it
// knows. "refresh" is the button on the Stats card: the same payload re-sent because a setup changed
// without a version change, and it must survive the filter or the button silently does nothing.
test("the event is one of the three words the card can show", () => {
  const accepted = source.slice(source.indexOf('report.event !== undefined'),
                                source.indexOf('const row = clean('));
  for (const word of ["install", "upgrade", "refresh"]) {
    assert.ok(accepted.includes(`"${word}"`), `${word} must be accepted`);
  }
  // Anything else is dropped rather than stored: a legend cannot render a word we never chose.
  assert.ok(accepted.includes("202"), "an unknown event is accepted and discarded");
});

test("the country comes from the edge, never from the report", () => {
  // A caller claiming a different country must not be believed: the value is the one Cloudflare
  // resolved, and the whole reason it is trustworthy is that no address was stored to derive it.
  const row = clean({ installationId: "x".repeat(32), country: "AQ" }, "NL");
  assert.equal(row.country, "NL");
});

test("an unknown country is recorded rather than left empty", () => {
  const row = clean({ installationId: "x".repeat(32) }, undefined);
  assert.equal(row.country, "??");
});

test("the received date is a day, not a moment", () => {
  // A timestamp precise to the second, combined with a country, is a fingerprint. A date is not.
  const row = clean({ installationId: "x".repeat(32) }, "NL");
  assert.match(row.receivedAt, /^\d{4}-\d{2}-\d{2}$/);
});

test("oversized values are dropped rather than truncated into storage", () => {
  const row = clean(
    { installationId: "x".repeat(32), chip: "E".repeat(500) },
    "NL"
  );
  assert.equal(row.chip, undefined);
});

test("the module list is bounded and flattened", () => {
  // DISTINCT fillers: the list is de-duplicated now, so two hundred copies of one name collapse
  // to one and could not show the cap working at all.
  const row = clean(
    {
      installationId: "x".repeat(32),
      modules: ["System", "Network", ...Array.from({ length: 200 }, (_, i) => `Filler${i}`)],
    },
    "NL"
  );
  assert.equal(row.modules.split(",").length, 64);
  assert.ok(row.modules.startsWith("System,Network"));
});

test("a numeric suffix inside a script path is left alone, being a folder the user named", () => {
  // The suffix rule belongs to the TYPE, which is the text before the first slash. Applied to the
  // whole string it renames a directory: `folder-2` is one a user made, not a second instance.
  const row = clean(
    {
      installationId: "x".repeat(32),
      modules: [
        "effect:MoonLive/folder-2/aurora.mle",
        "effect:MoonLive/folder-2",
        "service:MoonLive-3/deep-2/button.mls",
      ],
    },
    "NL"
  );
  assert.deepEqual(row.modules.split(","), [
    "effect:MoonLive/folder-2/aurora.mle",
    "effect:MoonLive/folder-2",
    "service:MoonLive/deep-2/button.mls",
  ], "only the type loses its suffix; every path segment survives");
});

test("an instance suffix is folded into its type, so one driver counts once", () => {
  // A device names a second module of a type `NetworkSend-2`, and firmware before the type fix
  // reported that instance name: the statistics then showed one driver as four.
  const row = clean(
    {
      installationId: "x".repeat(32),
      modules: [
        "driver:NetworkSend",
        "driver:NetworkSend-2",
        "driver:NetworkSend-3",
        "effect:MoonLive-2/nebula.mle",
        "effect:MoonLive/comet-trail.mle",
      ],
    },
    "NL"
  );
  const names = row.modules.split(",");
  assert.deepEqual(names, [
    "driver:NetworkSend",
    "effect:MoonLive/nebula.mle",
    "effect:MoonLive/comet-trail.mle",
  ], "the suffix goes whether it ends the entry or precedes a script, and a hyphenated script name stays whole");
});

test("a non-string sneaking into the module list is dropped", () => {
  const row = clean(
    { installationId: "x".repeat(32), modules: ["System", 42, null, "Network"] },
    "NL"
  );
  assert.equal(row.modules, "System,Network");
});

test("the stats endpoint selects only aggregates, never a row", () => {
  // The endpoint is world-readable, so a query that returned installationId would publish exactly
  // the identifier the privacy policy promises to hold rather than show.
  const stats = source.slice(source.indexOf("async function handleStats"));
  assert.ok(stats.includes("COUNT(DISTINCT installationId)"));
  assert.ok(!/SELECT\s+installationId/i.test(stats));
  assert.ok(!/SELECT\s+\*/i.test(stats));
});

// MoonTalk post validation: the shape a device actually sends must be ACCEPTED.
//
// Written after a change truncated the sender to 8 characters at storage while the validator still
// required 32, so every message a device sent was rejected with 400 and the board silently stopped
// receiving anything. Nothing caught it: the device cleared its box, the UI showed no error, and no
// test covered the post path at all. These pin both halves of that contract.
test("a message from a device is accepted, and a malformed sender is not", () => {
    // The validator's length check and what the handler stores must agree. A slice narrower than
    // the check means nothing can ever pass it.
    const stored = source.match(/const sender = typeof msg\.sender === "string" \? msg\.sender\.slice\(0, (\d+)\)/);
    assert.ok(stored, "the talk handler must derive `sender` from the request");
    const check = source.match(/sender\.length !== (\d+)\)/);
    assert.ok(check, "the talk handler must validate the sender length");
    assert.equal(stored[1], check[1],
                 "the stored sender and the validated length must match, or every post is rejected");

    // And the published form is the SHORT one, which is the property the truncation was meant to
    // protect: stored in full, published at 8.
    assert.match(source, /m\.sender\.slice\(0, SENDER_CHARS\)/,
                 "a published message must carry only the id prefix");
});

// The numeric fields arrive as JSON NUMBERS, and every other allowlisted field is a string. The
// generic `typeof value !== "string"` test dropped them silently, so three zeros were stored for
// every device until a branch of their own was added. Nothing pinned it: the harness could not even
// run a numeric case, because it never passed MAX_NUMBER into the extracted clean().
test("memory and light counts are stored as numbers, not dropped", () => {
  const row = clean(
    { installationId: "c26086e8da9b6fb2d4b81e4ca71f97e5",
      totalHeap: 282152, freeHeap: 84788, lightCount: 256 },
    "NL"
  );
  assert.equal(row.totalHeap, 282152);
  assert.equal(row.freeHeap, 84788);
  assert.equal(row.lightCount, 256);
});

test("a numeric field that is not a number stores zero rather than text", () => {
  // The endpoint is open, so its input is untrusted: a string, a negative, or a float must not
  // reach an INTEGER column as-is.
  const row = clean(
    { installationId: "c26086e8da9b6fb2d4b81e4ca71f97e5",
      totalHeap: "abc", freeHeap: -5, lightCount: 12.7 },
    "NL"
  );
  assert.equal(row.totalHeap, 0);
  assert.equal(row.freeHeap, 0);
  assert.equal(row.lightCount, 12);   // floored, not rejected: a count is a count
});
