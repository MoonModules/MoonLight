# MoonLight web installer

The source of the [web installer](https://moonmodules.org/MoonLight/install/). It flashes a board from the browser over USB, puts it on the network and applies the picked device model's settings, all over the same serial port.

- `index.html`, `install.js` and `install.css`: the page, with the picker shared with the device's Firmware card (`src/ui/install-picker.js`).
- `install-orchestrator.js` and `improv-frame.js`: the flash, Improv provisioning and the settings push. `teensy-writer.js` writes a Teensy.
- `devices.js`: the *Your devices* list.
- `deviceModels.json`: the device catalog below, which MoonDeck reads too.

How it works: [MoonInstaller](../docs/explanation/architecture/mooninstaller.md). Using it: [Install MoonLight](../docs/moonwalk.md#install). Changing it: [Try the web installer locally](../docs/moonwalk.md#preview-installer).

## The device catalog

One entry per piece of hardware, with the state document that sets it up: the document `PATCH /api/state` takes. Each field's meaning is in [`deviceModels.schema.json`](deviceModels.schema.json), which [`check_devices`](../moondeck/MoonDeck.md#check_devices) validates against. Which defaults an entry may carry is [config provenance](../docs/explanation/architecture/mooninstaller.md#config-provenance-mcu-devicemodel). What a schema cannot say:

- **It merges.** Applying an entry twice changes nothing. `"$patch": "replace"` makes a container's children the entry's only ones, at that level, and the device keeps the children its code adds, such as `WiFi`.
- **Drivers go last.** The installer sends one document per container in the entry's order, with `Drivers` last. Starting an output prints on UART0, which a classic board shares with Improv.
- **Loopback pins are the self-test jumper**, apart from the operating `pins`: `loopbackTxPin` and `loopbackRxPin`. Without them the test transmits on `pins[0]`.
- **Drivers come from the catalog.** The firmware creates only `Preview` at boot, so a device carries only the outputs its board has.
- **Only what the hardware fixes.** A bare board leaves out the pins its user wires. A vendor's entry carries its `System` and its default LED driver until a peripheral has its own spec and a test.
- **An image is our own copy**, checked in under `docs/assets/deviceModels/` with the vendor's permission, since the installer runs from its own site.

The picker draws a card per entry from its `image`, `description`, `url`, `supported` and `planned`:

| Collapsed | Expanded |
|---|---|
| ![Board picker collapsed](../docs/assets/ui/installer-board-picker-collapsed.png) | ![Board picker expanded](../docs/assets/ui/installer-board-picker-expanded.png) |
