# Plan: State by subscription, streamed

The `/ws` control channel sends every client the whole module tree as one message: 46 KB on the Teensy bench board today, built into one heap block before the first byte leaves.
On a board whose largest free block is about that size the block is refused, the sink truncates, and the page receives JSON it cannot parse: a board preset switched to Custom then shows its pins only after a refresh.
Every client also gets every module's per-second telemetry, though a page shows one root at a time.

This plan sends each client only what it views, and streams what it sends.

## What exists

- A full state `{modules:[…]}` goes to every client on connect and after any schema change, serialized into one owned buffer and drained in chunks on `tick20ms` (`startBufferedTextSend`, `drainStateSend`).
- Each second, a patch carries the leaves whose value hash moved. The hash cache is one global baseline over the whole tree, and every client receives the same patch.
- The page renders one selected root (`selectedModule`) and ignores data for the others, which it receives and parses anyway.
- Measured on the Teensy (2026-10-13): full state 46,384 bytes; the outline of every module without controls 5,287; the Drivers subtree 28,197, of which the FixtureProfiles `profiles` list is 14,838 and the Drivers `palette` options 10,024; Services (SdCard) 2,302.

## The model

Publish/subscribe with per-client subscriptions: the client declares the modules it views, and the server sends those modules and their descendants in full and an outline of the rest.
WebSocket fragmentation (RFC 6455 §5.4) carries each document as a sequence of frames generated as they drain, which is what the RFC introduced fragmentation for: a message of unknown size sent without buffering it.

## Steps

### 1. One tree serializer, resumable

A cursor walks the module tree depth-first and appends to a sink until a size target is reached, then returns, resumable on the next call.
Its resumable points are a module's header, each control, and each child, so a fragment is bounded by the target or by the largest single control, whichever is larger.
A module outside the subscription is written as its outline: the header fields and its children's outlines, without `controls`.

`buildStateJson` becomes this cursor run to completion with every root subscribed, so `/api/state` and the stream share one serializer.
A unit test pins that, for any target from 1 byte up, the concatenated fragments equal the full state byte for byte.

About 150 lines and four tests.

### 2. The full state streams per client

Each control client gets its own stream: a cursor, the fragment in flight and its send offset.
The first fragment goes out as a text frame without FIN, the middle ones as continuation frames, the last with FIN; the drain generates the next fragment only when the previous one has left.
This replaces the owned whole-document buffer, `startBufferedTextSend`, and the branch that ships a truncated document.

Patches, early patches and WLED frames wait while any stream is open, as they wait for the drain today.
A tree change during a stream finishes the document in valid JSON, since the cursor re-resolves its path every fragment, and the schema-changed hook queues the resync that follows.

About 120 lines and three tests over loopback.

### 3. A client subscribes

The page sends `{"subscribe":["<module>", …]}`, naming modules at any depth by name or type; a named module brings its descendants. `"*"` subscribes to the whole tree, for a tool that wants it.
A connect alone sends no state: the first document answers the first subscribe, so a page receives one document on open, and the WLED app, which never subscribes, receives only its own frame.
A subscribe resyncs that client alone: the others are first sent the patch due, then the baseline is retaken over the new union, so none of them misses a change. A schema change still resyncs every client.

The uplink is polled before the push in `tick20ms`, so the subscription a page sends on open arrives before its first document is built.

About 60 lines and two tests.

### 4. Patches follow the subscription

The baseline covers the union of what any client subscribed to.
A patch is built once over that union, its entries grouped by module, and each client is sent the groups its subscription covers.
Telemetry (`@tickTimeUs`, `@dynamicBytes`, status) and control values travel only for subscribed modules; an outline module changes only with a resync, since the nav reads names alone.

About 80 lines and two tests.

### 5. The page subscribes to what it renders

`renderCards` subscribes to the root it renders, plus the modules the page reads outside it (step 6), and a reconnect clears the subscription so the first render after it subscribes again.
Opening a root whose controls have not arrived yet renders when they do.
The page reads `controls` defensively wherever a module may be an outline.
A page load draws from the socket's answer alone: the first-paint fetch of `/api/state` goes, and the MoonBase check it carried becomes the `/moonbase` probe.
After an add, delete, move or replace, `refetchState` asks the socket for the view again with `{"resync":n}` and resolves when the document echoing `n` has rendered; the backup reads the device name and build from the state the page holds.
The diagnostics download, the control-surface target picker and a config restore keep `/api/state`: each needs every module, once, on a click.

About 50 lines and JS tests.

### 6. What the page reads outside the viewed root

An inventory of `app.js` (2026-10-13) found these reads outside the rendered root, and each is covered:

| Reader | Needs | Covered by |
|---|---|---|
| Status bar, `uiMode`, address-move dialog | System controls, every patch | subscribing System |
| Update badge, gallery version check | Firmware `appVersion`, `moonbaseMismatch` | subscribing the FirmwareUpdate module |
| MoonCloud relay, stats nudge | MoonCloud `outbox`, MoonStats `consent` | subscribing MoonCloud |
| Preview target fps | each PreviewDriver's `targetFps` | subscribing each PreviewDriver |
| Web MIDI bridge | each MidiService's `hello`, `desk` | subscribing each MidiService |
| Gamepad bridge, nav, deep links | names, types, tree shape | the outline |
| Control-surface target picker | every control's name and type | a `/api/state` fetch when assign mode turns on |

The page finds these modules by type in the outline, so a renamed instance is still found.
Everything after a mutation already refetches `/api/state`, and backup and restore read it too; those stay on REST.

### 7. Ping and Close

The frame walker hands a Ping and a Close to the uplink, which answers a Ping with a Pong carrying its payload and a Close with a Close and the end of the connection (RFC 6455 §5.5). A Ping arriving mid-fragment is answered at the fragment's end, since a control frame may go between fragments and never inside one.

About 50 lines and three tests.

### 8. Docs

- `HttpServerModule.h`: the state-push sections describe subscriptions, the outline and the stream.
- `ui.md`: the paragraph that calls server-side filtering unnecessary is replaced, and the communication section names the subscribe message.
- MIGRATING: a `/ws` client receives the outline until it subscribes; `"*"` restores the whole tree.

## What it removes

- The recursive `writeModuleJson` and its `writeControls` loop: one resumable serializer serves `/api/state`, the per-module route and the stream.
- The shared full-state buffer (`StateSend`), `startBufferedTextSend` and its drain.
- The branch that ships a truncated document, and the `ui.md` paragraph that justified sending everything.
- The one-patch-for-all builder, replaced by one collection sliced per client.
- The page's first-paint fetch of the whole tree from `/api/state`, and the whole-tree refetch after every add, delete, move and replace (about 20 call sites) and in the config backup.

The change still grows the code, by roughly 250 lines on the device and 60 in the page before tests. What it saves is at run time: the largest block a board must find drops from the whole tree (46 KB on the Teensy) to one fragment, a client receives a fifth of the tree or less, and the per-second walk hashes only what someone views.

## Measured traffic

The Teensy 4.1 bench board, one `/ws` client for 30 seconds per row, 2026-10-13. The old firmware was measured before flashing; the new rows subscribe the way the page does: the root plus `SystemModule`, `FirmwareUpdateModule`, `MoonCloudModule`, `PreviewDriver` and `MidiService`.

| Client views | Document on open | Per second |
|---|---|---|
| Old firmware, any root | 46,348 bytes | 1,040 bytes: 722 of patches, 318 of the WLED frame |
| Services (the SdCard card) | 9,872 bytes | 553 bytes |
| Effects | 9,028 bytes | 430 bytes |
| Control | 12,061 bytes | 351 bytes |
| Drivers, the largest root | 35,349 bytes | 392 bytes |

- Opening the interface or another root costs 9 to 12 KB instead of 46 KB, and 35 KB on Drivers, whose palette and fixture-profile lists are most of the tree.
- Each board switch on the SdCard card resends that client's document: 10 KB instead of 46 KB, and only to the client that changed when the change is its subscription.
- The per-second stream falls by half to two thirds. What remains is mostly the status bar's System telemetry and the open root's own values, which the page shows.
- The WLED frame goes only to a client that has not subscribed, which is the WLED app.
- The largest block a document needs falls from 46 KB to one 2 KB fragment, or 15 KB for the fixture-profiles list, the largest single control.

## Questions raised, answered

**Why did SdCard's pin rows need a page refresh, while show and hide work out of the box elsewhere?**
Show and hide did work; the message carrying them did not arrive whole. A board switch changes which pin controls are hidden, a flag only a full state carries, and the Teensy built that 46 KB document in one heap block while its largest free block was 47 KB. Sometimes the block was refused, the sink truncated, and the page could not parse what came.
A second, smaller cause was in the page: a full state that arrived while a field had focus deferred its render to the next full state, which might never come. It now renders when the field loses focus.

**Why does the System card show and hide logLevel without trouble?**
The `mode` control is applied in the page: every control carries the mode it needs (`minMode`) in the state the page already holds, so switching mode redraws locally and asks the device for nothing.

**The same code works on the Dig-Octa; why not on the Teensy?**
The code was the same; the free memory was not. The Dig-Octa found the block the whole tree needed, and the Teensy, running a 128×128 layout whose Layer already reported "buffer reduced", often did not.

**Can show and hide avoid the round trip to the device?**
The round trip stays: picking a board writes the pins on the device. The page could decide visibility alone only by copying each module's rule into JavaScript, a second home for logic the module's C++ owns. What shrinks is the reply: the device now resends one client's view, about 10 KB, rather than the whole tree.

**Is streaming the textbook solution?**
Yes. RFC 6455 §5.4 introduced WebSocket fragmentation for exactly this, a message of unknown size sent without buffering it, as HTTP/1.1 chunked transfer encoding (RFC 9112 §7.1) does for responses; writing JSON to a stream rather than a string is how standard JSON writers work. Sending a client only what it views is publish/subscribe with per-client subscriptions, the shape of Home Assistant's `subscribe_entities`, OPC UA monitored items and MQTT topics.

**Must the device send the whole state at all?**
No, and this plan stops it: a client gets the root it views and an outline of the rest. One root per page holds by design; two clients on different roots each get their own.

**Does this save lines of code?**
No: about 250 lines more on the device and 60 in the page before tests, against what *What it removes* lists. The saving is at run time: memory, bandwidth and the per-second walk.

**Is sending the whole tree still sustainable as MoonLight grows?**
No. Its cost grew with every module anywhere; now a client's cost grows with the outline, about 120 bytes per module, plus the one root it opens.

**Which root is the most expensive: Effects with many layers, or large MoonLive scripts?**
Drivers, at 28 KB of the Teensy's 46 KB, nearly all of it two option lists: FixtureProfiles `profiles` (15 KB) and the Drivers `palette` options (10 KB). Effects grows a few hundred bytes to a kilobyte per layer, effect or modifier. A MoonLive script costs only the controls it declares: its text stays in its file, which the editor fetches on its own.
The next limit is one large control, which a fragment cannot split: the profiles list sets the largest block a board must find.

**Does it save telemetry, every card's tick time and memory? Is telemetry already sent only while a browser shows the page?**
Yes on both. A hidden tab closes its socket after a short grace, so the device already sent it nothing; a visible tab received every module's `@tickTimeUs`, `@dynamicBytes`, `@status` and `@severity` each second, and now receives only those of the modules it views. The bytes are small next to the preview; the device-side saving is the per-second walk, which hashes only what someone views.

**Does it make a page refresh faster?**
Yes. A refresh used to fetch the whole tree twice, once from `/api/state` as a first-paint shortcut and once over the socket: 92 KB on the Teensy. The shortcut is gone and the socket answers with the view alone, 9 to 12 KB, 35 KB on Drivers. A tab left open over a device that switched to MoonBase still hands over, now through the small `/moonbase` probe the shortcut's 404 used to trigger.
After an add, delete, move or replace the page used to refetch the whole tree too; it now asks the socket for its view again (`{"resync":n}`, answered by the document echoing `n`), and the backup reads the three values it needs from the state it holds.

**Which other whole-tree fetches can go?**
Three stay, because each needs every module, once, on a click: the diagnostics download, the control-surface target picker in assign mode, and a config restore, which compares the backup with the whole live tree.

**Can we see the traffic we gained?**
See *Measured traffic*.

**Clicking through the Teensy's menu during the measurements: does that interfere?**
It did: each root a page opened changed its subscription, and a subscription change resynced every client. Now only the client that changed is resent its view, the others first receiving the patch due so that none misses a change.

**The LEDs flashed white, followed by a reset: is this change the cause?**
No: the first flash came on the old firmware. Each reset reported `UNKNOWN`, no fault and no watchdog, which a full-white strip dipping the USB supply explains, and every flash came while the device built a large state document. It is a backlog item with that evidence, *LEDs on the Teensy flash white, then the board resets*.

**Does the device answer WebSocket pings?**
It did not: the frame walker acted on text frames alone, so a Ping (`0x9`) and a Close (`0x8`) went unanswered, where RFC 6455 §5.5.2 says an endpoint must answer a Ping with a Pong and §5.5.1 a Close with a Close. The page was unaffected, since the browser's WebSocket API sends no pings and the page's keepalive is a text `"ping"`; a client that pings, Python's `websockets` by default and possibly Home Assistant, closed the connection after its pong timeout, 40 seconds in the measurement script. Step 7 answers both.

**Should Drivers' palette and fixture-profile lists be made smaller?**
They are 25 of Drivers' 35 KB, and most of it is repetition. The palette options carry 66 palettes with 16 hex colors each for the dropdown's swatches, which change only with the firmware or a palette script: names alone in the state, and the colors from a cacheable route the browser keeps across reloads, is the textbook shape. The fixture profiles' summary is 1.9 KB, and 14.5 KB is each expanded row's editor fields with their name, type and bounds repeated per profile: the row schema sent once, as `optionSets` already does for the role names, and the values per row. Together Drivers would fall to about 12 KB. That is its own change, backlogged as *Drivers' palette and fixture-profile lists, sent once rather than in every view* in `backlog-light.md`.

## Verification

- Desktop: unit and JS tests, the app run with two browsers on different roots.
- Teensy: the SdCard Custom switch shows and hides its pins without a refresh, twenty switches in a row, with the probe script reading every document whole (2026-10-13: 20 of 20).
- One ESP32 board, after the Teensy.

## Estimate

| Step | Size | Work |
|---|---|---|
| 1. Resumable serializer | ~150 lines | 2 hours |
| 2. Streamed per client | ~120 lines | 2 hours |
| 3. Subscribe | ~60 lines | 1 hour |
| 4. Patches by subscription | ~80 lines | 1.5 hours |
| 5-6. Page subscribes | ~80 lines | 1.5 hours |
| 7. Docs | | 1 hour |
