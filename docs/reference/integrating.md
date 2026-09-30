# Integrating

A device answers to three surfaces besides its own interface, for the code someone else writes: a script, a home-automation hub, a phone app.
They are shapes of one question rather than alternatives.
**HTTP** is the whole API, and the one to script against.
**MQTT** is for a home-automation hub that owns the device alongside everything else in the house.
**A WebSocket** carries the live state both ways, and accepts the small WLED command a phone app sends.

The browser page is one client of the HTTP API, on the same footing as any other.
Anything the interface can do a script can do: add an effect, set a control, read the state, upload a script.

The examples below were run against a device rather than read off the source. `<device>` is its address, as `192.168.1.42` or `moonlight.local`.

## HTTP

## The shortest useful thing

```bash
curl -X POST http://<device>/api/modules -d '{"type":"RainbowEffect","parent_id":"Layer"}'
# {"ok":true,"name":"Rainbow"}

curl -X POST http://<device>/api/control -d '{"module":"Rainbow","control":"speed","value":40}'
# {"ok":true}
```

The add returns the **name** the device gave the module, which is what every later call addresses it by. A second module of the same type is named `Rainbow-2`.

## Three things the payload decides

**The parent field is `parent_id`.** A body spelling it otherwise comes back as a 400 naming the field it wanted.

**A number rides as a JSON number.** `"value":40` sets 40, where `"value":"40"` parses to 0 and sets that. A pin is the case to watch, since 0 is a pin the chip has and will happily drive.

**The body is read as JSON whatever the `Content-Type` says.** `curl` without `-H` works.

## Reading

| Endpoint | Answers |
|---|---|
| `GET /api/state` | The whole module tree, which is what the page renders |
| `GET /api/system` | Name, firmware, chip, uptime, heap, FPS, and per-module tick times |
| `GET /api/types` | Every module type this build knows, with its role and the children it accepts |
| `GET /api/modules/<name>` | One module: its type, and every control with its current value |
| `GET /api/scripts` | The MoonLive script catalog, by role |
| `GET /api/dir?path=<dir>` | A directory listing from the device filesystem |
| `GET /api/file?path=<file>` | One file's contents |

`GET /api/types` is the one to read first when scripting: it names every type you can add and what may parent it.

## Writing

| Endpoint | Body | Does |
|---|---|---|
| `POST /api/modules` | `{"type":…,"parent_id":…}` | Adds a module, returns its name |
| `POST /api/control` | `{"module":…,"control":…,"value":…}` | Sets one control |
| `DELETE /api/modules/<name>` | | Removes a module and its children |
| `DELETE /api/dir?path=<path>` | | Removes a file, or a directory and everything in it |
| `POST /api/modules/<name>/replace` | `{"type":…}` | Swaps a module for another type in the same slot |
| `POST /api/modules/<name>/move` | | Reorders a module among its siblings |
| `POST /api/file?path=<file>` | the file body | Writes a file, creating parent directories |
| `POST /api/dir?path=<dir>` | | Creates a directory |
| `POST /api/reboot` | `{}` | Restarts the device |

## Setting everything at once

One call per control. `POST /api/modules` takes `type` and `parent_id`, so a module arrives with its defaults and each control follows in its own call.
They are cheap: a control write is a few milliseconds on a wired device.

When the whole configuration matters more than the individual calls, save a **preset** instead.
A preset captures the tree and its values together, recalls in one write, and survives a reboot.

## What a control is called

Ask the module:

```bash
curl http://<device>/api/modules/Rainbow
```

The reply lists every control with its name, type and current value. Control names are the ones the interface shows, lowercased in the style of `speed`, `multiplyX`, `ethBoard`.

## When something goes wrong

A failed call answers with a reason.
An unknown name gives `{"error":"module not found"}`, and a malformed body gives a 400 naming the field it wanted.
A call that returns `{"ok":true}` has been applied to the running tree, on the next frame.

One case is worth knowing: a control write to a module mid-rebuild can briefly refuse the connection.
The device recovers in well under a second, so a client that retries once sees nothing.

## A MoonLive script, end to end

A script is a file on the device and a control that names it, so writing one remotely is a write and a set. This whole sequence was run against a device.

```bash
# What is already there. `dir` is the factory catalog, compiled into the firmware.
curl http://<device>/api/scripts

# Write a script. The directory is created if it is missing.
curl -X POST "http://<device>/api/file?path=/moonlive/hello.mle" --data-binary '
class HelloEffect {
  byte hue = 0;
  void defineControls() { addControl("hue", hue, 0, 255); }
  void tick() { fill(paletteR(hue, 255), paletteG(hue, 255), paletteB(hue, 255)); }
}'

# Read it back.
curl "http://<device>/api/file?path=/moonlive/hello.mle"

# Give it something to run in, and point that at the script.
curl -X POST http://<device>/api/modules -d '{"type":"MoonLiveEffect","parent_id":"Layer"}'
curl -X POST http://<device>/api/control -d '{"module":"MoonLive","control":"script","value":"hello.mle"}'
```

Two things follow from the last call.
The module's status carries the compile result: a byte count on success.
A failure gives a message with an offset, as `a script is a class: expected class <Name> { … } @0`.
The script's **own** controls also appear on the module once it compiles, so `hue` above is then set like any other:

```bash
curl -X POST http://<device>/api/control -d '{"module":"MoonLive","control":"hue","value":128}'
```

Editing the file again recompiles on the next frame and keeps the control's live value, so a script can be iterated on from an editor without touching the module.

**Two directories, and the order matters.** `/.moonlive` is the factory catalog served from flash, which `GET /api/scripts` lists and a file read cannot reach. `/moonlive` is the user's, on the filesystem, and it **shadows** the factory one.
A user file of the same name wins, which is how a shipped script gets overridden and equally how a factory update becomes invisible. Write to `/moonlive`.

## Presets

A preset is the answer when a whole configuration matters more than the calls that build it. It is a file at `/.config/presets/<name>.json` holding the tree and its values together, and the Control module is the surface that saves and applies one.

```bash
# What presets a device has.
curl "http://<device>/api/dir?path=/.config/presets"

# Read one, which is also how you copy a configuration between devices.
curl "http://<device>/api/file?path=/.config/presets/stage.json"

# Apply one by name, through the Control module.
curl -X POST http://<device>/api/control -d '{"module":"Control","control":"preset","value":"stage"}'
```

The list is rebuilt from the folder rather than stored beside it.
So a preset written straight to `/.config/presets/` appears once the module next rescans: a reboot, or any save, rename or delete on the surface.
One written through the Control module shows up at once.

## Backup and restore

The device's own configuration is readable as files, one per module, which is what a backup is made of.

```bash
# Every module's persisted state.
curl "http://<device>/api/dir?path=/.config"

# One module's.
curl "http://<device>/api/file?path=/.config/Drivers.json"

# Write it back, to this device or another.
curl -X POST "http://<device>/api/file?path=/.config/Drivers.json" --data-binary @Drivers.json
```

A restore lands on the next reboot, since a module reads its file at startup. A full backup is `/.config` plus `/moonlive` for the user's scripts, which together are everything a device knows that its firmware does not.

## Firmware

```bash
# What is running, and what the device thinks is available.
curl http://<device>/api/system

# Update from a URL the device fetches itself.
curl -X POST http://<device>/api/firmware/url -d '{"url":"https://…/firmware-esp32s3-n16r8-v6.0.0.bin"}'

# Or push a binary from the machine holding it.
curl -X POST http://<device>/api/firmware/upload --data-binary @firmware.bin
```

The MoonBase partition has its own three, in the same shape, for the web interface rather than the firmware: `/api/firmware/moonbase`, `/api/firmware/moonbase-update-url` and `/api/firmware/moonbase-update`.

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

`/ws` carries the live state to the interface: the module tree on connect, value patches once a second, and preview frames as they are drawn. That is what makes a second browser tab show a slider move in the first.

It also reads inbound.
The native WLED app sets brightness and on-off by sending `{"on":true,"bri":128}`, and the device honours that frame.
Anything richer belongs on the HTTP API: the socket's inbound half exists for app compatibility rather than as a general control channel.

## Which to use

Script something: **HTTP**. Everything is there, it is synchronous, and an error comes back as a reason rather than a silence.

Wire it into a house: **MQTT**. The hub owns the device, discovery does the setup, and the topics outlive a rename.

Watch it change: **the WebSocket**, which is what the interface is already doing.
