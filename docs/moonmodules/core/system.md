# Core system

The device's fixed infrastructure: identity, network, provisioning, firmware, and the inspection tools. These modules are **always present and wired by code**, not user-added; the user does not add or delete them. User-added capability modules (Audio, IR) live in the `Services` container instead, see [core/services.md](services.md). Every row links to its generated technical page (the full API, from the `.h`) and its tests. Cross-cutting rationale that no single `.h` owns lives in the prose sections below the table.

## System modules

<a id="system"></a>

### System

The device's identity and vitals: name (behind mDNS `<name>.local`, the SoftAP SSID, the DHCP hostname), uptime, heap and the tick it renders at. Its fixed children (Tasks, the I2C bus, Pins) hang beneath it.

<img src="../../assets/core/SystemModule.png" width="300" alt="System module controls">

- `deviceName`: the identity behind mDNS, the SoftAP SSID and the DHCP hostname.
- `deviceModel`: the board model (drives the installer catalog entry).
- `mode`: how much of the UI is shown, `user`, `expert` (🎚️) or `developer` (🔧), cumulative.
- `logLevel`: serial verbosity, defaulting to Warn. The first 60 s always logs at Info.
- read-only rate: `uptime`, `fps`, `tickTimeUs`.
- read-only memory: `heap`, `psram`, `maxBlock`, `flash`.
- read-only identity: `mac`, `chip`, `cpu`, `sdk`, `bootReason`.

Detail: [technical](moxygen/SystemModule.md)

[Tests](../../reference/tests/unit-tests.md#systemmodule)

<a id="network"></a>

### Network

Brings the device onto the LAN before the HTTP and WebSocket servers start, trying Ethernet, then WiFi, then its own access point. Each interface's settings are on its own card below it.

<img src="../../assets/core/NetworkModule.png" width="300" alt="Network module controls">

- `mDNS`: the `<name>.local` hostname, on IPv4 and IPv6.
- read-only: `mode`, the interface in use.

**No DHCP server on an Ethernet-only build?** After about 20 seconds without a lease the device gives itself a `169.254.x.y` address (link-local). A computer on the same network without DHCP does the same, so open `<name>.local` in its browser and set a static address on the Ethernet card. A DHCP server that appears later still wins. Builds with WiFi try WiFi next, then their own access point.

Detail: [technical](moxygen/NetworkModule.md)

[Tests](../../reference/tests/unit-tests.md#networkmodule)

<a id="ethernet"></a>

### Ethernet

The wired interface, a child of Network: which board's wiring it uses and how it gets an address. Present where Ethernet is compiled in, and as a developer preview on the desktop.

<img src="../../assets/core/EthernetModule.png" width="300" alt="Ethernet module controls">

- `ethBoard`: the board's Ethernet wiring by name; `Custom` exposes every pin.
- `ipSettings`: DHCP or Static; Static reveals `ip`, `gateway`, `subnet` and `dns`.

Gateway and DNS are optional. A Static setting that cannot work, such as a gateway outside the subnet, is not used: the interface keeps DHCP and the card says why. Changing the addressing of the interface the page reaches the device through asks first, naming where the device will be.

Detail: [technical](moxygen/EthernetModule.md)

[Tests](../../reference/tests/unit-tests.md#ethernetmodule)

<a id="wifi"></a>

### WiFi

The WiFi station, a child of Network: the networks the device knows, in the order it prefers them, and the radio's settings.

<img src="../../assets/core/WiFiModule.png" width="300" alt="WiFi module controls">

- `scan`: look for the networks in range; `scanned` says how long ago it last looked.
- `available`: the networks in range, with signal bars and a lock; Connect joins one now.
- `known`: the networks it joins, in priority order, each with its `ssid`, `password` and IP settings.
- `txPowerSetting`: caps the radio's transmit power for a board that browns out; 0 lifts it.
- read-only: `rssi` and `txPower` (dBm) on a live radio.
- read-only after a join from the access point: `address` and `localName`, links to the device.

Joining is connect-first, as on a phone: tap a network under `available`, type its password, Connect. ⌄ details.

Detail: [technical](moxygen/WiFiModule.md)

[Tests](../../reference/tests/unit-tests.md#wifimodule)

<a id="access-point"></a>

### Access point

The device's own WiFi network, a child of Network, at 4.3.2.1. A captive portal: joining it opens the UI by itself, as a sign-in screen, and `http://4.3.2.1` reaches it too.

<img src="../../assets/core/AccessPointModule.png" width="300" alt="Access point module controls">

- `opens`: `on failure` (when nothing else joins), `always`, or `never (not recommended)`.
- read-only: `name`, `MM-` and four MAC digits, which says nothing about the owner.
- read-only: `clients`, the devices on it.

It carries the first known WiFi network's password, open only on a first setup or with an open home network. Its clients receive no stored password or config file.

`always` waits for a WiFi join in progress to settle, since one radio serves both. `never` applies only while Ethernet or a known WiFi network is configured, and the card says when it opens anyway. Joining it opens the WiFi card. ⌄ details.

Detail: [technical](moxygen/AccessPointModule.md)

[Tests](../../reference/tests/unit-tests.md#accesspointmodule)

<a id="improv-provisioning"></a>

### Improv provisioning

Serial/BLE Improv Wi-Fi provisioning: the web installer hands credentials to a fresh device over this protocol during the flash-and-connect flow. [Improv Wi-Fi](https://github.com/improv-wifi) is an open standard, and its [sdk-cpp](https://github.com/improv-wifi/sdk-cpp) / [sdk-js](https://github.com/improv-wifi/sdk-js) are the specification this implements, so any Improv-capable installer can provision a MoonLight device.

<img src="../../assets/core/ImprovProvisioningModule.png" width="300" alt="Improv provisioning module controls">

- `provision_status`: read-only provisioning state.

Detail: [technical](moxygen/ImprovProvisioningModule.md) · [frame format](moxygen/ImprovFrame.md) · [chunk reassembly](moxygen/ImprovOpReassembler.md)

<a id="devices"></a>

### Devices

Discovers other MoonLight devices on the LAN and lists them, persisting the last-known list across a reboot. A wired-by-code child of Network.

<img src="../../assets/core/DevicesModule.png" width="300" alt="Devices module, discovered LAN devices">

- `devices`: a List of discovered devices; each row expands to a detail panel. Persistable.
- `addressing`: `multicast + broadcast` (default), which WLED devices hear too, or `multicast`.

WLED announces and listens on broadcast, so it lists a device only from the broadcast copy. Presence is one small packet every ten seconds, so the copy costs the rest of the LAN next to nothing. A network that drops multicast is detected here, and the status says so. See [multicast and IGMP snooping](../../explanation/architecture/moonlight.md#multicast-and-igmp-snooping).

Detail: [technical](moxygen/DevicesModule.md)

[Tests](../../reference/tests/unit-tests.md#devicesmodule)

<a id="mqtt"></a>

### MQTT

Bridges the light to an MQTT broker so a home-automation hub can control it, as a transport over the shared apply-core rather than new control logic. Our own dependency-free MQTT 3.1.1 client, disabled until a broker is set. A wired-by-code child of Network. Topics, color-wheel mapping and the Homebridge config: ⌄ details.

<img src="../../assets/core/MqttModule.png" width="300" alt="MQTT module controls">

- `broker`: the broker hostname (e.g. `homeassistant.lan`) or IP. A hostname is resolved via DNS.
- `port`: broker port (default 1883).
- `username` / `password`: broker credentials, optional, the password stored obfuscated.
- `haDiscovery`: announce a Home Assistant discovery light, off by default.

HA already discovers the device over the WLED shim with no broker, so this stays off to avoid a duplicate entity. Turn it on for broker-only or cross-subnet setups. See the [home-automation guide](../../how-to/home-automation.md).
- read-only: `mqtt_status`, from `disabled` and `idle` through to `connected`, or an error.

Detail: [technical](moxygen/MqttModule.md)

[Tests](../../reference/tests/unit-tests.md#mqttmodule)

<a id="firmware-update"></a>

### Firmware update

Over-the-air firmware flashing, the one operation that swaps the binary and needs a power cycle (every *config* change applies live; a firmware OTA does not).

<img src="../../assets/core/FirmwareUpdateModule.png" width="300" alt="Firmware update module controls">

- `firmware`: the OTA image to flash.
- read-only: `version`, `build` and `partition`.
- `image`: on a device carrying two images, the app or MoonBase, drawn as the card's two tabs.

When MoonBase's version differs from the app's, the card warns and the top bar shows `⬆ MoonBase`; installing the matching one from its tab clears it. The `image` control's presence also tells the UI that installs run through the reboot-into-MoonBase cycle, behind one "updating firmware" overlay, and that a **Restart in MoonBase** button belongs on the card ([MoonBase](../../explanation/architecture/moonbase.md)).

Detail: [technical](moxygen/FirmwareUpdateModule.md) · [image vetting](moxygen/FirmwareImage.md)

[Tests](../../reference/tests/unit-tests.md#firmwareupdatemodule)

<a id="mooncloud"></a>

### MoonCloud

The container for everything MoonLight does with a server MoonModules runs. It holds no settings of its own: each thing it does is a child with its own consent, because a user who wants one has not thereby agreed to the other.

<img src="../../assets/core/MoonCloudModule.png" width="300" alt="MoonCloud module card">

- **Stats**, below: one opt-in report per install or upgrade, and the totals back.
- **Talk**, below: a public message board between devices.
- **Sync** (planned): device to device over the internet, a joint show across houses.

Detail: [technical](moxygen/MoonCloudModule.md)

<a id="stats"></a>

### Stats

One opt-in report about this install, sent once per install or upgrade, so development effort goes where the users are. Off until you answer yes. What it sends is in the [privacy policy](../../legal/privacy-policy.md).

<img src="../../assets/core/MoonStatsModule.png" width="300" alt="Stats module controls">

- `consent`: a checkbox, off by default. Nothing is sent and no identifier computed while off.
- read-only: `version` and `reportedVersion`, which differ exactly when a report is due.
- `send update`: reports again now, for a setup that changed without a version change.

The two versions differing is what makes an upgrade send one report and a reboot send nothing. The button replaces this install's row rather than adding one.

The report carries the chip, flash, PSRAM, SDK, device model, memory, light count, and which modules you added. It carries no device name, no addresses, no credentials and no text you typed, which a unit test asserts.

Detail: [technical](moxygen/MoonStatsModule.md)

[Tests](../../reference/tests/unit-tests.md#moonstatsmodule)

<a id="talk"></a>

### Talk

A public message board between MoonLight devices, in the shape Meshtastic's channel chat has. Off until you turn it on, and a message is sent because you typed one: nothing posts on its own.

<img src="../../assets/core/MoonTalkModule.png" width="300" alt="Talk module controls">

- `consent`: a checkbox, off by default. Nothing is published or read while it is off.
- `shareName`: whether your device name rides along, **off by default and a separate decision**.
- `message`: what to say, up to 280 characters. Typing changes nothing on its own.
- `send`: publishes the message and clears the box, as does Enter in the message field.

A device name identifies a person rather than a machine. Without it your messages carry the first 8 characters of your installation id, which groups them without naming you.

**Everything sent is public and permanent**: no private message, no recipient, no delete. **There is no authentication**, so a sender id can be fabricated by hand.

Detail: [technical](moxygen/MoonTalkModule.md)

[Tests](../../reference/tests/unit-tests.md#moontalkmodule)

<a id="file-manager"></a>

### File Manager

Browse and manage the device filesystem: a folder tree with an inline text editor. Distinct from Filesystem, the persistence engine. Behavior: ⌄ details.

<img src="../../assets/core/FileManagerModule.png" width="300" alt="File Manager panel, folder tree + toolbar">

- `file browser`: the panel itself: a folder tree, a toolbar and an inline text editor.
- **Backup (⤓)**, download the device's files as one `.json` bundle.

**Keep it private: it contains the WiFi password.** Through the access point the config files are left out. Every file is byte-verified against the listing, and an unreadable one is skipped and named.
- **Restore (⟲)**, upload a backup bundle, pressing twice since it overwrites the device's files.

Known renames from [MIGRATING.md](../../reference/MIGRATING.md) apply before upload, then a report lists what needs an eye. Every file applies as it lands, bar network settings and the web server's `port`, which the dialog names.
- `show hidden`: reveal dot-prefixed files and folders, such as `.config`.
- `filesystem`: read-only usage bar (used / total bytes, from the platform).
- `lastSaved`: read-only; how long ago config was persisted (read from the Filesystem engine).

Detail: [technical](moxygen/FileManagerModule.md)

<a id="i2c-bus"></a>

### I2C bus

A fixed System module (wired-by-code, always present) that owns the board's I²C bus: it opens the bus on its pins, every device on the bus attaches to it, and a scan probes it. A device names only its own address, such as AudioService's `codecAddr`; the wires are stated here, once per board. The pins default to unused (−1), so a board without an I²C device claims no GPIO.

<img src="../../assets/core/I2cBusModule.png" width="300" alt="I2C bus module controls">

- `sda` / `scl`: the bus GPIOs, −1 for unused; a board's catalog entry sets them.
- `scan`: a button; press to probe the bus now.
- read-only: `result`, each address found, named by the module driving it: `0x18 Audio (ES8311)`.

Detail: [technical](moxygen/I2cBusModule.md)

<a id="tasks"></a>

### Tasks

A read-only diagnostic showing **what runs where**: you cannot optimize which module runs on which core until you can see it. A fixed System module, wired-by-code, with each task's MoonModules nested beneath it.

<img src="../../assets/core/TasksModule.png" width="300" alt="Tasks module, a row per FreeRTOS task">

- read-only: `tasks`, a row per FreeRTOS task.
- read-only: `core0` / `core1`, what executes on each core, empty on a single-core chip.

Each row carries `name`, `state`, `core`, `prio` and `stack`, the minimum free stack seen. A `cpu` percentage appears only in a profiling build, off by default because the run-time counter costs about 5% of the tick.

Expand a row for the modules in that task, each as `Name · Nus · NB · Nheap`. A closing `∑ modules` line cross-checks them against the tick. Empty on desktop.

Detail: [technical](moxygen/TasksModule.md)

<a id="pins"></a>

### Pins

A read-only diagnostic showing **which module owns each GPIO, for what role, and whether that pin is safe for it**, the device's pin ownership map, keyed by physical GPIO. A fixed System module, wired-by-code.

It walks the live tree for every claimed pin, holding no state, and flags double claims.

<img src="../../assets/core/PinsModule.png" width="300" alt="Pins module, the GPIO ownership map">

- read-only: `pins`, a row per claimed GPIO.

Each row carries `gpio`, `owner` and `role`, plus live `dir`, `level` and `drive`. A row takes a colored edge when unsafe: red for a reserved or double-claimed pin, yellow for a driven role on a strap, per [gpio-usage.md](../../reference/hardware/gpio-usage.md).

Detail: [technical](moxygen/PinsModule.md)

<a id="control"></a>

### Control

A grid of preset pads, a row of rotary encoders above them, a row of on/off switches above those, and a bank of faders below, the layout of a Mackie-style control desk ([X-Touch](https://www.behringer.com/product.html?modelCode=0808-AAF), [QCon Pro G2](https://www.iconproaudio.com/product/qcon-pro-g2/)), so a physical surface maps onto it without a translation layer.

<img src="../../assets/core/ControlModule.png" width="300" alt="Control module surface: encoders, preset pads, faders">

<video src="../../assets/moontube/10-control.webm" controls playsinline width="720" title="The surface: the mapped controls, assigning your own, and OSC arriving from outside."></video>

- `presets`: one pad per preset file. Click applies, right-click names, drag rearranges.
- `switch1` … `switch8`, the switch row. `switch1` drives `Drivers.on`, the rest unbound.
- `encoder1` … `encoder8`, rotary encoders. Drag or scroll to turn, right-click to see the binding.
- `fader1` … `fader8`, faders. `fader1` drives `Drivers.brightness`, the rest unbound.

Detail: [technical](moxygen/ControlModule.md)

[Tests](../../reference/tests/unit-tests.md#controlmodule)

<a id="filesystem"></a>

### Filesystem

<img src="../../assets/core/FilesystemModule.png" width="300" alt="Filesystem module controls">

The persistence **engine**: writes control values to `/.config/*.json` and restores them on boot, overlaying loaded values through each control's pointer during `defineControls()`. The File Manager browses the stored files.

Calling `defineControls()` again at runtime, when a Select changes mode, clears and rebuilds the set, so only the controls relevant to the current mode show. That is how a conditional `hidden` flag re-evaluates, and how a config change applies live with no reboot.

Detail: [technical](moxygen/FilesystemModule.md)

[Tests](../../reference/tests/unit-tests.md#filesystemmodule)

## WiFi, details

The device tries the known networks in priority order, each for ten seconds, and opens its access point only after the last. Networks a scan in the last five minutes did not see go to the end of that order, so no time goes to joining what is out of range. Each network keeps its own `ipSettings`, DHCP or Static, as a phone does, and a Static setting that cannot work joins by DHCP with the reason on its row. Editing the joined network's addressing from a page it carries asks first, as on Ethernet.

Connect joins a network at once. A network picked from `available` is saved to `known` only once it joined, and "incorrect password" says why when it did not. A scan takes the radio off-channel for a few seconds, which drops incoming lights while connected, so the device scans by itself only when its access point opens, and otherwise when you press `scan`.

## Access point, details

A join asked for from a phone on the access point keeps the access point up beside the new network for two minutes, and the WiFi card shows `address` and `localName` to follow. A phone's sign-in screen has no address bar, so open a link in the browser. Joining moves the radio to the router's channel, which knocks the phone off, and a phone often lands back on its own network, where the sign-in screen is gone. So the network's row shows `afterJoining`, the device's `.local` link, before you press Connect.

While the access point runs, its DNS answers every name with 4.3.2.1, and the web server sends any page request through it for another name to the UI, while the API answers as itself. That is what makes a phone show the sign-in screen.

## Control, details

A control is one named, typed value on a module, declared once in `defineControls()` and reachable by name from every surface: the web UI, the REST interface, a preset file, a script, a physical desk. The type decides how it renders, what it accepts, and how it persists, so a module never writes UI code and never parses its own JSON.

[`Control.h`](moxygen/Control.md) owns the type and its operations. The discriminator sits with the functions that interpret it, so adding a type is one enum entry plus the cases the compiler then demands, rather than a search for every `switch` in the tree.

Detail: [technical](moxygen/Control.md)

#### The module tree

Every module is the same building block, whatever it does: [`MoonModule`](moxygen/MoonModule.md) declares the lifecycle, and [`Scheduler`](moxygen/Scheduler.md) owns the top-level modules, boots them in phases, and drives every tick. A module is a node in a tree, so a driver, a layer and an effect nest the same way and the same walk serves all of them.

[`ModuleFactory`](moxygen/ModuleFactory.md) turns a type name into an instance, which is what lets a preset file name a module this build has never instantiated.

Detail: [technical](moxygen/MoonModule.md) · [Scheduler](moxygen/Scheduler.md) · [ModuleFactory](moxygen/ModuleFactory.md)

#### Surfaces and mappings

[`ControlSurface`](moxygen/ControlSurface.md) is the desk layout itself: which bank a control belongs to, and how a physical encoder or fader finds the value it drives.

[`InputMapping`](moxygen/InputMapping.md) is the binding table behind it. A row says what an input does to a control: set it, step it, toggle it. A delta steps down as well as up, so one encoder covers a range without a second control to reverse it.

Detail: [technical](moxygen/ControlSurface.md) · [InputMapping](moxygen/InputMapping.md)

#### Presets

A preset is a file: `/.config/presets/<name>.json`. Saving writes one, applying reads one, deleting removes one. Nothing else holds preset state, so there is no second copy to keep in step: the list is read from the folder at startup rather than persisted alongside it. After that a save, rename or delete re-reads its one file, and so does a file the file API writes or removes, which every module hears about, so a preset written through the File Manager or the gallery appears on its own. One file at a time matters on an ESP32, where walking the folder costs about 15 ms per preset and reading one about 60 ms.

The name becomes the file name, so it is restricted to printable ASCII without `/`, `\` or `.`, a validator on the control, which every write path runs. The file's `$slot` records which pad the preset occupies, so a surface arranged to match a physical desk survives a reboot. The save form (`name`, `slot`, `source`) is input for the next save rather than configuration, so none of it is written to flash.

##### What a preset holds

A preset is a [state document](../../reference/integrating.md#setting-everything-at-once), the one `PATCH /api/state` applies, plus its `$slot`, a root `$` key the engine reads as the file's own:

```json
{
  "$slot": 12,
  "Effects": { "$patch": "replace", "enabled": true,
    "Layer": { "type": "Layer", "$patch": "replace", "enabled": true,
      "Noise": { "type": "NoiseEffect", "$patch": "replace", "speed": 3, "enabled": true } } }
}
```

A save writes the `source` module through the same writer `GET /api/modules/<name>/document` uses, rooted at the top level. The pad editor names one of the four containers, which captures it whole: *a look*, *a geometry*, *a hardware setup* or *a service configuration*. A card's `{ }` button saves that card alone, which applied puts back that card and leaves its siblings. A hand-written or gallery preset may hold less, such as `{"Drivers":{"palette":"Ocean"}}`.

An `Effects` preset is a look and applies to a board with completely different hardware; a `Drivers` preset with pins is device-specific. A secret stays out of a document, so a preset never carries a password.

A preset that fails names the first failure and where it is, and what came before it stays applied. A preset in the flat format older builds wrote is listed but not applied, and a backup and restore converts it. A malformed file leaves the live tree untouched.

##### One active preset per container

The four containers are the **roles**: layout, effects, driver, service. A preset holds the roles of the containers it sets and leaves the others alone, so a layout preset and a look can be active at the same time, and a palette preset holds the driver pad beside a running look.

A pad is tinted by its role: layout blue, effects violet, driver green, service amber.

##### Applying is a rebuild

A saved container carries `"$patch": "replace"`, so applying it creates, replaces and destroys modules to match: a preset carrying more than the device has adds it, and one describing less removes what it omits. Every removal runs before any creation, so a module can move between layers under its own name.

Structural mutation quiesces the render worker, and mutations run inline on the render tick, so a large restore stalls rendering for its duration. `prepareTree()` runs once at the end. Presets are a cold-path feature; the tick path is untouched.

##### The gallery

Under the pads, the gallery lists [MoonLight-Gallery](https://github.com/MoonModules/MoonLight-Gallery)'s `index.json`, most liked first. The browser fetches it from `raw.githubusercontent.com`, so the device needs no internet of its own. A preset installs onto the first free pad, or applies once through `PATCH /api/state`, and either way brings the scripts it names that the device lacks: the gallery's copy, else the one the firmware ships. A script installs into `/moonlive`.

#### Home Assistant

Looks reach Home Assistant two ways, and only `Effects` presets travel either of them.

**The WLED integration** (`/presets.json`) is the native path: HA renders looks in its own preset dropdown, shows which one is applied, and applies one when it is chosen. This is what HA calls a preset.

**MQTT discovery** publishes the same looks as the light entity's **effect list**. HA has no preset concept over MQTT, so they arrive as effects, the same result from the user's side, reached through a different mechanism.

HA caches the preset list and re-fetches only when the device's `info.fs.pmt` value changes, so the device reports a revision counter there that bumps on every preset save, rename and delete, a counter rather than a timestamp, so two changes inside one second still read as two. A constant there leaves HA showing the list it read at setup forever; over MQTT the same revision re-announces the effect list mid-session.

Only looks are exposed, on both paths. A `Drivers` or `Layouts` preset rewires pins or geometry, which must not be reachable from something that believes it is choosing a color scheme, the restriction is enforced at the apply entry point, not merely by omitting them from the list.

Home Assistant's WLED integration connects on **port 80 only**: its host field rejects a port, so a desktop build (which defaults to 8080) needs `--port 80`, and that needs root:

```sh
sudo uv run moondeck/run/run_desktop.py --port 80
```

The discovery buffers are sized to the looks this device actually has, and grow or shrink as presets are added and removed. There is no cap on the number: a fixed one would either reserve memory a small setup never uses, or silently publish nothing once the list outgrew it.

## MQTT, details

The topic prefix is `MoonLight/<mac>`, a **stable** identifier (the last 6 hex of the device's MAC), fixed for the device's life. Renaming the device does **not** change its topics, so a hub's config never breaks on a rename (the WLED/Tasmota/Home-Assistant convention). It's derived, not a stored control.

**Topics** (for a device whose MAC ends `563cfe`): the device SUBSCRIBEs to the `set` topics and PUBLISHes the `get` topics on change (and on connect, so a controller never reads "No Response"). It also publishes its friendly `deviceName` on the retained `name` topic, so a hub can show the human name while the topics stay MAC-stable:

| direction | topic | payload |
|---|---|---|
| set → device | `MoonLight/563cfe/on/set` | `true` / `false` |
| device → get | `MoonLight/563cfe/on/get` | `true` / `false` |
| set → device | `MoonLight/563cfe/brightness/set` | `0`–`100` |
| device → get | `MoonLight/563cfe/brightness/get` | `0`–`100` |
| set → device | `MoonLight/563cfe/hsv/set` | `h,s,v` (hue `0`–`359`, sat/val `0`–`100`) |
| device → get | `MoonLight/563cfe/hsv/get` | `h,s,v` |
| device → get | `MoonLight/563cfe/name` | the friendly `deviceName` (retained) |
| device → get | `MoonLight/563cfe/update/state` | `{"installed_version":…,"latest_version":…,"release_url":…,"title":…}` (retained; HA update entity) |
| set → device | `MoonLight/563cfe/update/set` | target version string (empty = install latest); triggers OTA against the matching GitHub release asset |

The HomeKit color wheel has no "palette" concept, so `hsv/set`'s hue+saturation pick the **nearest palette** (each built-in palette has a representative color; the closest one is selected) and the value drives brightness, the color wheel becomes a natural palette selector.

**Homebridge**, install [`homebridge-mqttthing`](https://github.com/arachnetech/homebridge-mqttthing) and add a `lightbulb` accessory. Use the device's own MAC suffix (read it from the `mqtt_status`/topics, or `mosquitto_sub -t 'MoonLight/#'`) in place of `563cfe`:

```json
{
  "accessory": "mqttthing",
  "type": "lightbulb",
  "name": "MoonLight",
  "url": "mqtt://<broker>:1883",
  "username": "<user>",
  "password": "<pass>",
  "topics": {
    "getOn": "MoonLight/563cfe/on/get",
    "setOn": "MoonLight/563cfe/on/set",
    "getBrightness": "MoonLight/563cfe/brightness/get",
    "setBrightness": "MoonLight/563cfe/brightness/set",
    "getHSV": "MoonLight/563cfe/hsv/get",
    "setHSV": "MoonLight/563cfe/hsv/set"
  },
  "onValue": "true",
  "offValue": "false"
}
```

Home Assistant adopts the device two ways, both zero-config:
- **MQTT auto-discovery**: with `haDiscovery` on (opt-in; off by default) and a broker set, the device announces itself on `homeassistant/light/MoonLight_<mac6>/config` and HA auto-creates a wired entity with **on/off + brightness** (the config declares `brightness` only; color isn't in it, so the entity has no color control). Retained across reboots. Color/palette stays on the separate `hsv/set` topic above, not this entity. Off by default because the WLED `/json` shim already gives HA a richer light (color + palette + sensors) over mDNS with no broker, leaving both on lists the device twice; enable this only for broker-only / cross-subnet setups.
- **WLED integration**: HA's built-in WLED integration discovers the device over the WLED `/json` API MoonLight already serves; on/off + brightness work with no broker.

Both can be on at once. Setup walkthrough (including exposing HA to Apple Home via HA's HomeKit Bridge, no Homebridge needed) in the [Home Assistant recipe](../../how-to/home-automation.md#adopt-in-home-assistant).

## File Manager, details

The panel is a lazy folder **tree** (each folder loads its children on first expand) plus an inline text editor. Dot-prefixed entries (the `.config` persistence dir) are hidden unless `show hidden` is on.

- Click a folder's row to select it and toggle its expansion (▸/▾); click a selected file to open the editor.
- The toolbar acts on the selected node: **＋ folder** creates a folder inside it, **＋ file** creates an empty file (click it to edit), **🗑 delete** removes the selected file, or a folder and everything inside it (press-twice to confirm), **⟳** refreshes.
- **Drag files from the desktop** onto a folder (or the tree) to upload them, the body streams straight to the file (any size, binary-safe; capped only by a sanity limit and the free space, which it reports if short); a per-file **⤓** streams it back to the desktop.
- The editor loads a file's text, pretty-prints JSON on open, and saves atomically; a binary file (contains a NUL) loads read-only (use ⤓ to fetch it intact). Upload and download both stream, so neither truncates.
- Create / delete are HTTP calls (`POST` / `DELETE /api/dir?path=`), not controls, the path rides the request, so nothing is stored on the device per op.

Last-modified dates (needs an NTP time source + LittleFS mtime), binary/large + folder upload, folder-as-zip download, and `.ml` syntax highlighting are backlogged ([backlog-core § File Manager follow-ups](../../work/future/backlog-core.md#file-manager-follow-ups)).
