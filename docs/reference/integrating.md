# Integrating

A device answers to three surfaces besides its own interface, for the code someone else writes: a script, a home-automation hub, a phone app.
They are shapes of one question rather than alternatives.
**HTTP** is the whole API, and the one to script against.
**MQTT** is for a home-automation hub that owns the device alongside everything else in the house.
**A WebSocket** carries the live state both ways, and accepts the small WLED command a phone app sends.

The browser page is one client of the HTTP API, on the same footing as any other.
Anything the interface can do a script can do: add an effect, set a control, read the state, upload a script.

## HTTP

Every route, with its parameters and answers, is in [the REST API reference](api.md). `GET /api/types` is the one to read first when scripting: it names every type you can add and what may parent it.

## Step by step

Each recipe is a process in MoonWalk, whose API steps link their route in the reference:

- Add a module and set its controls: [Script against the HTTP API](../moonwalk.md#http-api).
- Set many modules and controls in one call: [Set everything at once with a state document](../moonwalk.md#apply-state).
- Write a MoonLive script and run it: [Run a MoonLive script over the API](../moonwalk.md#moonlive-api).
- List and apply presets: [Save, edit and apply a preset](../moonwalk.md#presets).
- Back up and restore a device, `/.config` with one file per module plus `/moonlive` for the user's scripts: [Back up and restore a device](../moonwalk.md#backup-restore).
- Update the firmware by upload, by URL or through MoonBase: [Update a running board over the network](../moonwalk.md#ota-board).

## Three things the payload decides

**The parent field is `parent_id`.** A body spelling it otherwise comes back as a 400 naming the field it wanted.

**A number rides as a JSON number.** `"value":40` sets 40, where `"value":"40"` parses to 0 and sets that. A pin is the case to watch, since 0 is a pin the chip has and will happily drive.

**The body is read as JSON whatever the `Content-Type` says.** `curl` without `-H` works.

## Setting everything at once

`PATCH /api/state` takes a **state document**: the part of the tree you want, keyed by module name, applied the way JSON Merge Patch ([RFC 7386](https://www.rfc-editor.org/rfc/rfc7386)) applies one.
Its shape is the module tree, not the interface schema `GET /api/state` returns; `GET /api/modules/<name>/document`, below, reads it back.

- A value sets a control of the module it sits in, with the same checks `POST /api/control` runs.
- An object is a child module: found by name, created when it is missing and names a `type`, replaced in place when its `type` differs.
- `null` removes a module, and `"$patch": "replace"` in a module's object keeps only the children it lists, in its order, besides the ones the firmware wires itself, such as the WiFi card or Drivers' FixtureProfiles.
  Its controls stay as they are.
- What would fail at creation is found before anything changes: an unknown container or type, a role the parent refuses, a name the tree cannot hold.
- Every removal applies next, the children of a re-typed module included, so a module can move to another branch under its name.
  The rest applies in document order, and the tree rebuilds once at the end.
- A new or re-pointed script declares its controls when it compiles, so the document sets them after that rebuild. The answer counts them as `deferred`.
- Past 16 new or re-pointed modules in one document, an unknown control on a script module is deferred as well, and the device log names it after the rebuild.
- A root key starting with `$` is the file's own, such as a preset's `$slot` (its pad), so a preset file applies as it is.
- `"$name": "<new>"` in a module's object renames that module in place: its controls, its children and its place stay, and a control surface binding to it follows the new name.
  It takes a module you can remove or replace, below the top level, and a name of 1 to 15 characters no other module holds.
  A preset, a gallery look or a backup saved before the rename still names the old one, so applying it brings that module back under its old name.

The answer is `{"ok":true,"changes":N}`, or a 400 naming the first failure and where it is, such as `{"error":"no such control","at":"Effects.Layer.Swirl.nope","changes":2}`.
A failure only writing finds, such as a value out of range, leaves what came before it applied.
A document is at most 32 KB, and a larger one is refused with a 413.
`POST /api/state` takes the same document, for a client that cannot send `PATCH`.
`PATCH /api/state?live=1` applies a document that drives rather than configures, as a show building its look on another device does. It is checked and applied the same way, and nothing it changes is marked to save, so a reboot returns the device to its saved state.
Adding, replacing, renaming and removing a module through `/api/modules` applies a one-member document, so the same rules hold on both paths.
`PATCH /api/modules/<name>` with `{"name":"<new>"}` renames, answering `{"ok":true,"name":"<new>"}`, or a 409 when another module holds the name.

`GET /api/modules/<name>/document` reads a module back as a document, its path from the top level around it, and a card's `{ }` button shows the same.
A secret stays out of every document, and applying one leaves the secret as it is.
That is a password control, and a list holding one, whole, such as WiFi's known networks with their names and addresses.

A [preset](presets.md) is such a document saved on the device, recalled with one click, and kept across a reboot, and the [gallery](https://github.com/MoonModules/MoonLight-Gallery) shares them.

## What a control is called

`GET /api/modules/<name>` lists every control of the module with its name, type and current value. Control names are the ones the interface shows, lowercased in the style of `speed`, `multiplyX`, `ethBoard`.

## When something goes wrong

A failed call answers with a reason.
An unknown name gives `{"error":"module not found"}`, and a malformed body gives a 400 naming the field it wanted.
A call that returns `{"ok":true}` has been applied to the running tree, on the next frame.

One case is worth knowing: a control write to a module mid-rebuild can briefly refuse the connection.
The device recovers in well under a second, so a client that retries once sees nothing.

## Finding devices

A script that hardcodes an address breaks when DHCP moves one. Two ways round it, neither requiring a scan:

**mDNS.** Every device advertises itself, so `http://<deviceName>.local` reaches it by name. The name is the one on the System card, `MM-` plus a suffix by default.

**MoonCloud.** A device that has opted in appears in the account's device list with its current address, which is how the installer finds boards it has never seen.

## MQTT

For a hub rather than a script. The device publishes its state and subscribes for commands under a topic tree keyed on the **MAC address**.
A rename therefore never moves the topics, and a hub's binding survives it. Home Assistant discovery is one control away, off by default.

The broker, port, credentials and the discovery toggle are all controls on the Mqtt module, so the same `POST /api/control` above configures them. The topic tree, the color mapping and a worked Homebridge configuration are on [the MQTT card](../moonmodules/core/system.md#mqtt).

MQTT is a transport over the same apply-core the HTTP path uses. A value set over MQTT and a value set over HTTP take the same route into the running tree.

## WebSocket

`/ws` carries the live state to the interface: the module tree on connect and value patches once a second. That is what makes a second browser tab show a slider move in the first. `/wsp` carries the preview frames as they are drawn, on a socket of their own so a large frame never delays the state.

It also reads inbound.
The native WLED app sets brightness and on-off by sending `{"on":true,"bri":128}`, and the device honors that frame.
Anything richer belongs on the HTTP API: the socket's inbound half exists for app compatibility rather than as a general control channel.

## The WLED API

The device answers the WLED JSON API, so the WLED apps and Home Assistant discover it, read its state and set it. `GET /json`, `/json/info` and `/json/si` carry one `info` object, so python-wled can merge each poll of `/json/si` into its first read of `/json`. `POST /json/state`, `POST /json` and the WebSocket take one state document. `POST /json` answers `{"success":true}`, or the whole device object when the body carries `"v":true`. `GET /win` takes the same fields as query keys. A field the device has no meaning for is ignored.

| Field | Effect |
|--|--|
| `on`, `bri` | The Drivers module's `on` and `brightness`. `"on":"t"` toggles. |
| `ps` | Applies the look preset in slot `ps`, counted from 1 as `/presets.json` lists them. |
| `seg` | One segment object or an array of them. |

A segment is a layer. `GET /json` carries one `seg` element per layer, with the layer's name in `n`, so Home Assistant shows a light per layer. A device with no layer reports one stand-in segment. Each segment carries `lc` set to 1 (RGB), which Home Assistant needs to create the segment's light.

| Segment field | Reads and writes |
|--|--|
| `id` | The layer's index, or the array position when a write omits it. A lone object addresses the first layer. A write to a layer that does not exist is ignored. |
| `on`, `bri` | The layer's enabled flag and opacity. `"on":"t"` toggles. |
| `fx` | The index of the layer's first effect among the registered effect types, in the order `effects` lists them. A write replaces that effect, or adds one to an empty layer. |
| `sx`, `ix`, `c1`, `c2`, `c3` | The first five byte controls of that effect, in declaration order. `c3` runs 0 to 31, the others 0 to 255. A write is clamped to the control's own range. |
| `o1`, `o2`, `o3` | The first three Bool controls of that effect. |
| `pal` | The palette index of the Drivers module, the same on every segment. |
| `col` | The primary color, as `[r,g,b]` or hex, which a write maps to the nearest palette. A `pal` in the same write wins. |

The device has no counterpart for the nightlight (`nl`), UDP sync (`udpn`), live override (`lor`) or a segment's `rev` and `frz`. A write that sets them succeeds and changes nothing, so the matching Home Assistant switches read back unchanged.

The index of an effect type moves when a type is added, so a client resolves an effect by its name in `/json/effects`. `/json/fxdata` names each effect's sliders and options in the `sliders;colors;palette;flags` format of WLED's own interface, and `/json/palettes` lists the palette names that `pal` indexes. `/json/eff`, `/json/fxda` and `/json/pal` answer the same.

`GET /win` takes `T` (0 off, 1 on, 2 toggle), `A` (brightness) and `PL` (preset). `SS` names the layer the other keys address, the first by default. The keys `FX`, `SX`, `IX`, `X1`, `X2`, `X3`, `M1`, `M2`, `M3`, `FP`, `SB`, `CL`, `R`, `G` and `B` set the layer's effect, sliders, options, palette, opacity and color. The keys go in the query, or after `&` in the path as `/win&A=128`. A value that does not start with a digit, such as WLED's relative step `~10`, is ignored.

`docs/reference/openapi.yaml` lists the routes.

## Which to use

Script something: **HTTP**. Everything is there, it is synchronous, and an error comes back as a reason rather than a silence.

Wire it into a house: **MQTT**. The hub owns the device, discovery does the setup, and the topics outlive a rename.

Watch it change: **the WebSocket**, which is what the interface is already doing.
