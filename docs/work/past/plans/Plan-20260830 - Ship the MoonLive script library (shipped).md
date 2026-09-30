# Plan: Ship the MoonLive script library

## Context

The 27 scripts in `moonlive/` are the standard library of the MoonLive engine: 17 effects, 7
layouts, 3 modifiers, compile-tested against the engine on every build by `unit_MoonLiveScripts`.
They ship in the repository and reach no device. Adding one means opening GitHub, copying text,
pasting it into the script editor and saving, which is nothing like adding a baked-in effect.

`docs/moonmodules/light/MoonLiveEffect.md` already tells the user "a device keeps its own copies
under `/moonlive/`". Nothing puts them there. This plan makes that sentence true.

The library is expected to grow to roughly **ten times its current size**, and that expectation,
not today's 22 KB, is what the design has to survive.

## The shape: flash the catalog, fetch the contents

**The device carries the NAMES of every factory script and the content of none.** The picker lists
the whole library, a name the device does not hold yet is marked, and selecting one downloads it to
the filesystem. From then on it is an ordinary local file.

Two things make this the right shape.

The measurement: a name costs about 12 bytes and a script about 800. At 270 scripts the catalog is
**3 KB of flash** while the contents are 220 KB. Flash then scales with how many scripts exist, and
the filesystem with how many are actually used.

The observation, and it is the stronger half: **a device uses a handful.** One layout describes the
rig it is wired to and the others are meaningless on it. Shipping 270 scripts to a device that will
run three is the waste the whole design exists to avoid, and no compression rate fixes it.

### Alternatives considered and rejected

- **Embed every script in the firmware image.** Correct at 22 KB, wrong at 220: a tenth of the app
  slot on every device for content most owners never open, paid twice because the scripts are then
  copied to the filesystem as well.
- **A separate LittleFS partition, flashed with the scripts.** Cannot be delivered: `esp_https_ota`
  writes the app partition only, so a data partition never reaches a device over the air. Worse, a
  device keeps its existing partition table until a full serial flash (`esp32dev.csv` records
  this), so every board in the field would need a cable before it could even hold the partition.
- **One downloadable bundle for the whole library.** Version-locked and a single request, but
  all-or-nothing: the device pulls 220 KB to use three scripts, which is the waste again.
- **Compressing what ships.** The device links no inflater at all (the UI is gzipped for the
  BROWSER to expand). Adding one costs ~3 KB of code and a 32 KB window buffer to save 15 KB on a
  320 KB device.

## What it needs

### The catalog, generated at build time

A CMake step globs `moonlive/**` and generates a header of names, mirroring `embed_ui.cmake`'s place
in the build. It **globs rather than lists**, so a script added to the repo reaches devices with no
other edit, and it emits an iterable table rather than one constant per script, so no code ever
names a script.

Names only. A one-line description each would cost 19 KB at 270 scripts, so descriptions stay in the
files, where a user reads them after downloading.

### The browser fetches, not the device

**The device never talks to GitHub.** The UI reads the script from GitHub and posts it to the
device's own file endpoint, which is the mechanism WLED-MM's arti-fx already uses (`artifx.js`:
`downloadGHFile` fetches `raw.githubusercontent.com` and hands the text to `uploadFileWithText`).

That choice deletes an entire platform seam. A device-side fetch needed TLS with a certificate
bundle on ESP32, and on desktop it needed a TLS library the build does not link, which meant
shelling out to `curl`. Doing it in the browser needs neither: one JavaScript path serves every
platform, because the UI is the same everywhere.

It also removes the internet requirement from the device. Only the machine looking at the UI needs
a connection, so **a rig on an isolated network still receives scripts** as long as the laptop
driving it can reach GitHub. That is a better fit for a venue than requiring the device itself to be
online.

The upload half already exists and is already hardened: `POST /api/file?path=` streams the body
through `fsWriteStream`, rejects `..`, checks free space, and triggers a live re-prepare on success.
Nothing new is needed on the device at all.

`raw.githubusercontent.com` sends `access-control-allow-origin: *`, so the device-hosted UI can read
it directly with no proxy (verified). Release ASSETS are served from
`release-assets.githubusercontent.com`, which sends no CORS header and therefore cannot be read this
way, which is a further reason the content comes per file from the repo rather than as a bundle.

### Where a script comes from

```text
https://raw.githubusercontent.com/MoonModules/MoonLight/<tag>/moonlive/<role>/<name>
```

`<tag>` is the firmware's own release tag, falling back to `main` for a development build. **Pinned
on purpose**: a script fetched from `main` may use an engine builtin the running firmware lacks, and
that failure arrives as a compile error the user can do nothing about. Pinning means a script always
matches the engine that will run it.

One direct URL per script, so there is no GitHub API call, no rate limit and no JSON to parse: the
catalog is already on the device, so nothing needs listing.

### Two directories, and the split is the point

| | Directory | Written by | Listed by |
|--|--|--|--|
| Factory | `/.moonlive` | the UI, when a factory script is first picked | the picker, always |
| User | `/moonlive` | the user, through the editor and File Manager | the picker, always |

`/.moonlive` is dot-prefixed, so the File Manager hides it unless `hidden=1`, the same convention
`/.config` uses. Factory scripts do not clutter the file tree and are not somewhere a user edits by
accident, but they are plain readable text for anyone who goes looking, which is the point of a
library you learn from.

The script editor saves to `/moonlive`, never to `/.moonlive`, and `/moonlive` resolves first. So
**editing a factory script forks it**: nothing moves, the edit is written as a second file of the
same name in the user directory, and that one wins from then on. The downloaded original stays
untouched where it landed.

That is the whole point of the split, and it is worth stating what it buys, because one directory
would be simpler: **revert works offline.** With a single directory an edit overwrites the only
copy, and getting the original back means deleting the file and downloading it again, which needs
internet at exactly the moment a rig is on site. With the split, **un-editing is deleting the
fork**, a local operation, after which the factory script resolves again. No version tracking, no
merge, no network.

The cost is that two files share a name. Only one is visible without `hidden=1`, and the rule is
one line (the user copy wins), but it is the thing to explain in the docs.

### The picker

One list. A script already on the device shows plain; one that is not shows with a marker (a cloud
glyph). Selecting a marked one fetches it, and it becomes plain. The user thinks of it as one
library, because it is one library.

A download that fails says so and leaves the control unset, rather than selecting a script that is
not there. **A script already downloaded keeps working forever offline**, and the device itself
never needs a connection at any point: the browser fetches, so a rig on an isolated network is
served by whatever laptop is looking at its UI.

## What happens on a firmware update

Mostly nothing, and deliberately.

**A missing script does nothing, visibly.** `compileScriptFile` frees the compiled code *first*,
before any validation returns, so a script that was renamed, emptied or deleted cannot leave the old
program running while the card reports an error. The comment records the failure it prevents: "The
card says 'script not found' and the fixture keeps rendering the script that is gone." The status is
`Severity::Error`, so it shows on the module's card in red.

| Case | Behavior |
|--|--|
| **A factory script is dropped from the catalog** | It leaves the picker. A module still naming it keeps its downloaded copy in `/.moonlive` and goes on working, because the file is local. |
| **A factory script changed upstream** | The device keeps what it downloaded. A newer version arrives only if the user deletes the local copy and picks it again. |
| **The user had edited it** | Their fork in `/moonlive` is never touched, and keeps winning. |

Note this is *more* stable than embedding would have been: a downloaded script is the user's file
and an update cannot take it away.

The card warning is the whole warning. A boot-time report naming every missing script across the
module tree and the saved presets was considered and rejected on cost: presets are JSON blobs on the
filesystem, so it would mean opening and parsing up to 64 of them, walking the live tree, and
persisting the previous firmware version to detect an update, all to repeat what the card already
says precisely, on the exact module.

## Steps

1. **The catalog.** A CMake step that globs `moonlive/**` into a generated header of names and
   roles. Mirrors `embed_ui.cmake`'s wiring in both build paths. *(Done.)*
2. **Resolve `/moonlive` first, then `/.moonlive`.** One change in `compileScriptFile`'s path
   construction. This is the whole fork mechanism.
3. **The picker.** List the catalog merged with both directories and mark what is not local. Picking
   a marked script fetches it in the BROWSER from `raw.githubusercontent.com` and posts it to
   `/api/file?path=/.moonlive/<name>`, then selects it. A failure reports and selects nothing.
4. **Revert to factory.** Beside a forked script, an action that deletes the fork so the factory
   copy resolves again. Arm-then-confirm, as the editor's delete already is.
5. **Docs.** The MoonLiveEffect card describes the two directories, the download and the fork rule;
   the tutorial covers picking a script on a fresh device.

## Tests

- **Unit:** a script resolves from `/.moonlive` when `/moonlive` lacks it; the user copy wins when
  both exist; a name in neither reports "script not found" with no code left running (pinning the
  existing free-first guarantee).
- **Unit:** the generated catalog holds every file in `moonlive/`, so a script added to the repo
  cannot silently fail to appear.
- **Unit:** a fetch failure leaves the control unset and the previous script untouched.
- **Scenario:** a module naming a downloaded script renders it after a reboot with no network.
- **The compile sweep already exists:** `unit_MoonLiveScripts` compiles every file in `moonlive/`,
  so a library script that stops parsing fails the build before it can ship.

## Verification

Desktop build plus `ctest`. On a bench board: flash, confirm the picker lists the whole library with
everything marked remote, select `plasma.mle`, confirm it downloads and runs, then reboot with the
network unplugged and confirm it still runs. Edit it, confirm the fork appears in `/moonlive`,
delete the fork, confirm the factory copy resolves again. The 4 MB classic ESP32 is the board that
matters, because it is the one where flash and filesystem are tight.

## Risks

- **The BROWSER needs internet the first time each script is picked**, though the device never does.
  A downloaded script then works offline forever. The failure is bounded to that first pick and is
  reported rather than silent.
- **A raw.githubusercontent outage blocks a first download.** No mitigation beyond the error
  message; the same dependency the firmware update already carries.
- **IRAM, not flash, is the resource to watch.** A compiled script lives in an `allocExec` block,
  which is IRAM on ESP32 and competes with WiFi; `ripples.mle` measures 2372 bytes. A user who
  discovers the library may run several scripted modules where before they ran none. Worth measuring
  on the 4 MB board with several scripted layers active.
- **A tag that does not exist upstream** (a local build with an unusual version) falls back to
  `main`; that fallback must be explicit, not a silent 404 the user reads as "the library is broken".

## What this plan does not do

- **No GitHub API.** The catalog is on the device, so no directory ever needs listing: no rate
  limit, no JSON parsing, no second host.
- **No bundle, no compression, no partition change.** See the rejected alternatives.
- **No new emoji category in the module picker.** That picker chooses module *types* and all three
  MoonLive modules already carry `📝`. The library appears in the script picker, a different control.
- **No global restore.** Reverting is per script, where its meaning is unambiguous.
