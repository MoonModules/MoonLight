# MoonLight web installer

This directory holds the source for the **custom installer page** (driven by `install-orchestrator.js`, not ESP Web Tools) at <https://moonmodules.org/MoonLight/install/>.

End users land here, pick a channel + device, click Install.
The browser flashes the device over USB (Web Serial → ESP32) and runs Improv-Serial provisioning.
It then pushes the picked device model's config over the **same serial port** as state documents (**"Improv = REST over serial"**: `APPLY_OP` frames, one per top-level container).
One orchestrator does all of it, with no ESP Web Tools dependency.
Pushing over serial rather than HTTP is what makes the deployed HTTPS installer work.
A browser blocks an HTTPS page from POSTing to a plain-`http://` device (mixed content), and serial bypasses the network entirely.

## What's in this directory

- [`index.html`](index.html): the installer page.
  Imports the shared [`install-picker.js`](../../src/ui/install-picker.js) module and rewrites the picker's GitHub-release URLs to same-origin Pages URLs before handing them to the custom orchestrator (Web Serial is CORS-bound).
- [`install-orchestrator.js`](install-orchestrator.js): owns the SerialPort across flash → reboot → Improv provision → config push.
  Replaces the ESP Web Tools install button, whose `state-changed` event (EWT 10.x) fires inside a shadow DOM the host page cannot see.
  Owning the whole flow lets the orchestrator write its own vendor RPC frames.
  After provisioning it pushes the picked device model's `state` over serial as APPLY_OP documents, one `{"<Root>": …}` per top-level container (see `stateFrames` in [`improv-frame.js`](improv-frame.js)).
  Each is the body `PATCH /api/state` takes, applied by the same device-side engine.
  An **Apply device defaults** checkbox gates that push.
  It ticks itself with **Erase chip first**, since a clean slate wants defaults, and starts unticked otherwise, so re-flashing a configured device keeps its modules and controls.
  The board's `txPower` brown-out cap goes first, through its own `SET_TX_POWER` RPC **before** provisioning, since a weak-power board fails its first association without it.
  A device that doesn't speak Improv back (the typed-IP or eth-only path) gets no serial push.
  Its defaults are applied later from MoonDeck on the LAN, over plain HTTP REST with no mixed content.
- [`devices.js`](devices.js): the *Your devices* list.
  Stores devices the user provisioned from the installer so they can be revisited, erased or forgotten (Visit / Erase / Forget).
  The device-model defaults are applied during the install over serial, so this list has no "inject" button.
  MoonDeck on the LAN re-applies a model to a running device.
- [`deviceModels.json`](deviceModels.json): the device-model catalog (name → firmware variants + the `state` document).
  The installer slices it into the APPLY_OP documents; MoonDeck and the picker also read it.
  Schema below.
- [`favicon.png`](favicon.png): moon-man, same as the device UI.
- [`README.md`](README.md): this file.

## Picture board picker

The board picker is a visual card grid driven by each board's `image` and `url` catalog fields, plus its `supported` and `planned` capability chips.
Both fields are optional (see the schema below): a card without an `image` shows no photo, and one without a `url` shows no product link.
It reuses the installer's flash machinery unchanged.
The grid drives the shared [`install-picker.js`](../../src/ui/install-picker.js) through a hidden board `<select>`, so firmware narrowing and flashing are identical to a plain dropdown pick.
Board images are a Pages-only asset (staged from `docs/assets/deviceModels/`), never flashed.

The picker is a collapsed row like the other fields.
Clicking it expands the searchable card grid, and picking a board collapses it back to a labeled summary with a thumbnail:

| Collapsed | Expanded |
|---|---|
| ![Board picker collapsed](../assets/ui/installer-board-picker-collapsed.png) | ![Board picker expanded](../assets/ui/installer-board-picker-expanded.png) |

## Catalog schema (`deviceModels.json`)

A flat JSON array of catalog entries.
Each entry is the single source of truth for one piece of hardware and what to set up on it at install time.
Two clients consume it identically.
The web installer (`install-orchestrator.js`) sends the entry's `state` as APPLY_OP documents over serial, and MoonDeck (`moondeck/moondeck.py`) sends it in one `PATCH /api/state` on the LAN.
So **adding another module or control needs no client change**: both send the same document, and only the transport differs.

```json
{
  "name": "MM testbench S3",
  "chip": "ESP32-S3",
  "firmwares": ["esp32s3-n16r8"],
  "state": {
    "System": { "deviceModel": "MM testbench S3" },
    "Services": { "$patch": "replace",
      "Audio": { "type": "AudioService", "wsPin": 4, "sdPin": 5, "sckPin": 6 } },
    "Drivers": { "$patch": "replace",
      "RmtLed": { "type": "RmtLedDriver", "pins": "18", "loopbackTxPin": 13, "loopbackRxPin": 12 } },
    "Layouts": { "Grid": { "type": "GridLayout", "width": 8, "height": 8 } },
    "Effects": { "Layer": { "type": "Layer", "$patch": "replace",
      "AudioSpectrum": { "type": "AudioSpectrumEffect" },
      "RandomMap": { "type": "RandomMapModifier" } } }
  }
}
```

Each entry carries a **state document**, the same format `PATCH /api/state` takes ([Setting everything at once](../docs/reference/integrating.md#setting-everything-at-once)).
A root key is a top-level module (`System`, `Drivers`, `Effects`, `Layouts`, `Network`, `Services`), whose type is not written.
A member keyed by its module name with a `type` is a child module; scalars and arrays are controls.
The `deviceModel` identity is part of it: `System.deviceModel`.

**`"$patch": "replace"`** (optional, on a container): applying a state document is a merge, so an entry's children land *alongside* the container's current children.
`replace` makes the entry's children the only ones, and the device keeps code-wired children such as `WiFi`.
Used when an entry wants its own effects to replace the defaults rather than stack with them (the testbench above swaps the default `NoiseEffect` for `AudioSpectrumEffect` + `RandomMapModifier`).

**Drivers goes last.** The installer sends the documents in the entry's key order with `Drivers` last.
Starting an output prints on UART0, which a classic board shares with Improv, and LED pins 1 or 3 end serial reception.

**LED drivers are catalog-added, not boot-wired.** The only driver the firmware creates at boot is `Preview`, which needs the HTTP-server broadcaster the catalog can't supply.
Every other driver (`RmtLedDriver`, `LcdLedDriver`, `ParlioLedDriver`, `NetworkSendDriver`) is added per board through its `state` document, under the `Drivers` container.
So a device carries only the outputs its board has, rather than every driver the chip can run.
The default LED driver per chip: **classic ESP32 → `RmtLedDriver`**, **S3 → `RmtLedDriver`**, **P4 → `ParlioLedDriver`** (1–8 lanes).
On the S3, LCD needs the full 8-lane bus (see the [LcdLedDriver spec](../moonmodules/light/moxygen/LcdLedDriver.md)), and 1..8-pin LCD is a future extension.

The `RmtLed` `controls` block presets the loopback self-test pins, so a bench operator flips the `loopbackTest` switch with no re-typing.
**`loopbackTxPin` and `loopbackRxPin` are the test jumper (tx→rx), separate from the operational `pins`.** On the S3 the strip runs on `pins`=18 while the loopback transmits on 13 and captures on 12.
Without `loopbackTxPin` the test falls back to transmitting on `pins[0]`, which works when the jumper is on the LED pin.
The testbench's jumper is on a dedicated pin, which is why the override is stored.
`loopbackTest` is left off, since presetting pins doesn't run the blocking test.
The sibling `MM testbench ESP32-16MB` adds `RmtLedDriver` (`pins`=18, loopback tx=4/rx=5); the `…P4` adds `ParlioLedDriver` (`pins`=20–27, `ledsPerPin`=64, loopback tx=33/rx=32).
Only the S3 bench has a mic wired, so only it carries `AudioService`.
Each entry declares only what is on that board.

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | identifier **and** display label (no key/label split) |
| `chip` | yes | the MCU family, for the picker's chip filter |
| `firmwares` | yes | the firmware variants flashable on this hardware; **the first entry is the default** the picker pre-selects (reorder the array to change the default) |
| `image` | no | board photo for the picker: a local path under `assets/deviceModels/`, named `<firmware>-<board>` with the chip written dashed (e.g. `assets/deviceModels/esp32-pico-quinled-dig-next-2.jpg`, `esp32-s3-n8r8-lightcrafter-16.jpg`; P4 boards use `esp32-p4-`). Host our own copy, never a vendor hotlink: see [§ Board images & links](#board-images--links) below |
| `url` | no | product-page link the picker shows next to the board (the vendor's own page, e.g. `https://quinled.info/quinled-dig2go/`). A remote URL is fine here: it's a click-through link, not an asset the installer fetches |
| `supported` | no | short capability labels the firmware drives on this board (e.g. `["LEDs", "WiFi", "Ethernet"]`), rendered as solid chips on the picker card |
| `planned` | no | short labels for peripherals the board physically has but no module drives *yet* (e.g. `["IR receiver", "Onboard button"]`), the backlog seed for future spec + test work. Rendered as dashed "(soon)" chips on the picker card |
| `flashBaud` | no | esptool flash baud for this board, overriding the audience default (installer 460800, CLI/MoonDeck 921600). Set it only when a board's USB bridge needs a non-default rate, such as a flaky CH340 pinned to `460800`. One of the standard rates (`115200`/`230400`/`460800`/`921600`); `check_devices.py` validates it |
| `state` | yes | the state document that sets the board up |

Each child module is an object member `{ type, <controls>…, <children>… }`:

| Member | Meaning |
|---|---|
| `type` | factory type to create (e.g. `RmtLedDriver`, `AudioService`, `AudioSpectrumEffect`) |
| the member key | the module's name, which the controls and children address |
| scalars, arrays | the controls to set; a number may be written `"0x18"`, the form a datasheet gives an address in |
| `$patch` | `"replace"` on a container: its current children are removed first |
| object members with a `type` | child modules, to any depth |

Each document is a merge, so applying an entry twice changes nothing.
Each top-level container is sent as its own document (the largest is 236 bytes, within the device's 512-byte reassembly buffer), so a container stays small.

**Injection is opportunistic and partial.** A module's `controls` (and the units list itself) carry **only what is known and hardware-fixed** (vendor-soldered mic pins, board-fixed Ethernet pins).
A bare board whose LED or mic pins the *user* wires omits them; the user adds the module and sets the pins manually later.
Inject nothing you don't know.
This is the MCU/Board/Device provenance rule from [MoonInstaller, config provenance](../docs/explanation/architecture/mooninstaller.md#config-provenance-mcu-devicemodel): default a pin only at the level that fixes it.
The `MM testbench S3` entry above adds an `AudioService` with the verified INMP441 mic pins (WS=4/SD=5/SCK=6, matching the bench wiring in [`AudioService.h`](../../src/core/services/AudioService.h)).
It also adds an `RmtLedDriver` (LEDs on `pins`=18, loopback jumper tx=13→rx=12), a known-hardware Device on the maintainer's desk, so the inject is testable end-to-end.
The `ESP32-16MB` sibling adds `RmtLedDriver` (LEDs=18, loopback tx=4/rx=5); the `P4` sibling adds `ParlioLedDriver` (LEDs=20–27, loopback tx=33/rx=32).
Only the S3 has a mic wired.
Each carries only what is physically present (LCD is not preset on the S3 testbench: its 8-lane bus would clash with the mic pins 4/5/6).

**Specific boards/devices: spec 'n test first.** A real product entry grows a peripheral or pin unit only once that hardware has its own spec and a test pinning it.
The spec carries the product-page link and grabbed images for installer selection and pin layout: the project's *Specs before code*, applied to catalog hardware.
So vendor entries (the QuinLED Dig-2-Go, the Serg shields, …) carry only their **`System` unit (with the `deviceModel` control) plus the default LED driver** until spec'n'tested for more.
Whether the Dig-2-Go's *onboard* mic is supported is an open spec'n'test question, for example, so its entry adds no `AudioService`.
The per-board capability loop behind this is recorded in [lessons.md § catalog-driven installer branch](../docs/work/past/lessons.md#lessons-from-the-catalog-driven-installer-branch-3-layer-device-model).

### Board images & links

The `image` (board photo, eventually pin-annotated) and `url` (product page) are the picker's visual layer **and** the inputs to the per-board capability loop above.

- **`image`: host our own copy, never hotlink a vendor URL.** Three reasons.
  (1) A hotlink breaks whenever a vendor reshuffles their CDN, and the installer must keep working same-origin and offline, the constraint that bundles firmware manifests into Pages.
  (2) Pin-annotated overlays need a *derived* image rather than the raw shot.
  (3) Third-party product photos are the vendor's copyright, and redistributing them needs permission.
  So: use the `url` to *find* the reference photo, then either shoot/redraw our own **or** check a local copy into [`docs/assets/deviceModels/`](../assets/deviceModels/) **with permission**, named for the board's slug.
  The catalog stores a local path, never a remote image URL.
- **`url`: a remote link is fine.** It's a click-through to the vendor's own page, not an asset the installer fetches.
- **Deploy** stages only the *referenced* images into `install/assets/deviceModels/`, not the whole `docs/assets/deviceModels/` library, which also holds photos for boards outside the catalog.
  So an `image: "assets/deviceModels/<slug>.jpg"` resolves same-origin from `/install/deviceModels.json`.

**One entry type, no Board/Device split.** A "Device", a finished rig like the `MM testbench S3` (board plus a wired mic), is an entry with *more* of `state` filled in than a bare "Board".
Same schema; there is no separate `devices.json`.

**`extends` (reserved, unresolved).** The carrier/shield pattern is literal extension: a Serg shield is a D1-Mini32 board *plus* the shield's pins.
So a future optional **`extends: "<parent entry name>"`** lets an entry inherit another's `state`.
Levels nest (MCU→carrier→device), a child overrides its parent at the same `{module, control}`, and modules merge by name.
The resolver is a client-side pre-pass to be built at the first real shared-base collision; every entry is self-contained, so it isn't implemented yet.
Don't author duplicated pin blocks expecting `extends` to dedupe them until it ships.

**One format with the HTTP API.** A catalog entry's `state` is the document `PATCH /api/state` takes, and the same document the presets and the backup bundle use.
`moondeck/scenario/run_live_scenario.py` runs scenario steps over HTTP against a live device, the same channel MoonDeck uses.

## What's *not* in this directory

- **The install-picker module** (`install-picker.js`) lives at [`src/ui/install-picker.js`](../../src/ui/install-picker.js) because the same file is also embedded into the device firmware UI (see [`docs/moonmodules/core/FirmwareUpdateModule.md`](../moonmodules/core/FirmwareUpdateModule.md)).
  The release workflow copies it into the Pages root next to `index.html` on every deploy.

- **Per-release binaries and manifests.** The release workflow keeps the last five stable and five prerelease releases under `releases/<tag>/` on Pages.
  The install page's `toLocalUrl()` rewrites the picker's `releases/download/<tag>/<file>` URLs to `./releases/<tag>/<file>`.
  Self-hosting is necessary because GitHub release-asset URLs don't return CORS headers, so the browser can't fetch them cross-origin during a Web Serial flash.
  The on-device OTA path doesn't have this problem and reads the assets directly from GitHub.

## Local preview

Three recipes, in increasing fidelity to production.
Each runs the picker against the real GitHub Releases API (CORS-friendly) but differs in whether the install button can flash.

### Render-only (no flash)

Quickest.
In MoonDeck: **Desktop tab → Preview Installer**.
Or from the CLI:

```bash
uv run moondeck/run/preview_installer.py
# open http://localhost:8000/ in Chrome / Edge / Opera
```

Both forms stage `index.html` + `install-picker.js` into `build/install-preview/` and serve them.
Picker populates, dropdowns work, but clicking Install fails because the local server has no `releases/` tree.
Useful when iterating on the install page's HTML/CSS/JS: the `?nocache=1` query parameter forces the picker to bypass its 5-minute sessionStorage cache while you edit.

### End-to-end with CI-built firmware

The closest you can get without tagging.
Pulls the latest branch CI run's artifacts (the 4 firmware bundles produced by `release.yml`'s `build-esp32` matrix), stages them under `releases/v<branch-tag>/` so the install page's `toLocalUrl()` can find them.

```bash
DIST=/tmp/mm_install_ci
rm -rf "$DIST"; mkdir -p "$DIST/releases"

# Use the version from library.json as the pretend tag.
V=$(jq -r .version library.json)
TAG="v$V"
mkdir -p "$DIST/releases/$TAG"

# Pick the most recent successful release.yml run on this branch.
RUN_ID=$(gh run list --workflow=release.yml --branch=$(git branch --show-current) \
  --status=success --limit=1 --json databaseId --jq '.[0].databaseId')
[ -z "$RUN_ID" ] && { echo "no successful CI run on this branch"; exit 1; }
echo "Using run $RUN_ID"

# Download the 4 firmware artefacts, flatten, regenerate Pages-relative manifests.
for F in esp32 esp32-eth esp32s3-n16r8; do
  TMP=$(mktemp -d)
  gh run download "$RUN_ID" -n "esp32-$F" -D "$TMP"
  cp "$TMP"/*.bin "$DIST/releases/$TAG/"
  python3 moondeck/build/generate_manifest.py \
    --firmware "$F" --version "$V" \
    --release-url . \
    --flasher-args "$TMP/flasher-$F.json" \
    --out "$DIST/releases/$TAG/manifest-$F.json"
  rm -rf "$TMP"
done

# Drop the install page + shared picker module in place.
cp mooninstaller/index.html "$DIST"/
cp src/ui/install-picker.js "$DIST"/

cd "$DIST" && python3 -m http.server 8000
# open http://localhost:8000/
```

Caveats:

- The picker fetches releases from api.github.com (the real list), so it shows every published release.
  Only the one you staged locally (`releases/$TAG/`) flashes; others 404.
  Switch the picker to "Pick specific release" and choose the local tag to avoid this.
- 60 API requests/hour anonymous rate limit; sessionStorage caches for 5 minutes so dev iteration stays well under.

### Full release dry-run (RC tag)

When the local recipes pass, tag a release-candidate.
Per the plan-18 design, RC tags now also publish to Pages (no `-rc` gate) so beta testers can self-serve from the dropdown.
End users default to Stable, so the RC release is visible only to those who opt into the Pre-release channel.

```bash
# Bump library.json to "1.0.0-rcN", commit, tag v1.0.0-rcN, push tag.
# Workflow:
#   - 4 ESP32 builds + macOS build + Windows build run.
#   - release job stages cumulative content (last 5 stable + 5 prerelease)
#     under pages/install/releases/<tag>/ on Pages.
#   - Publishes a GitHub Release flagged "Pre-release".
#   - Deploys Pages with the new release available immediately.
```

Web Serial requires Chrome, Edge, or Opera on desktop.
Firefox and Safari don't ship the API.

## Deployment

`release.yml` does this automatically on every `v*` tag (stable + RC):

1. `build-esp32` matrix produces four firmware bundles + four `flasher_args.json` files.
   `build-macos` produces the macOS tarball; `build-windows` produces the Windows zip.
2. The `release` job:
   - Generates manifests in two flavours: absolute URLs (for GitHub release assets, used by the OTA picker) and relative URLs (for the Pages copy, used by the web installer).
   - Pulls the last 5 stable + 5 prerelease releases' assets via `gh release download` and stages them under `pages/install/releases/<tag>/`.
   - Publishes the GitHub Release (binaries + absolute manifests as assets).
   - Deploys Pages with `index.html` + `install-picker.js` + the staged `releases/` tree.

Manual setup, one-time per repo: **Settings → Pages → Source: GitHub Actions**.

No deploy-from-branch: the workflow is the only producer.
A separate `mooninstaller/`-only Pages deploy was considered and rejected: it would have to re-run the same cumulative-content dance, so a docs-only deploy buys nothing.
