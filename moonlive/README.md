# MoonLive scripts

The script library, one file per script, grouped by the module that runs it.
Name one in a module's `script` control on a running device and it compiles to native code on the next tick.

The language is [MoonLive](../docs/moonmodules/light/moonlive.md).
Why it compiles rather than interprets is [the engine's design](../docs/explanation/architecture/moonlive.md).

## What is here

**A script's ROLE is its file extension**: `.mle` an effect, `.mll` a layout, `.mlm` a modifier, `.mls` a service, `.mlp` a palette. One
language, five names, the way GLSL uses `.vert`/`.frag` for one shading language. It is what a card
filters its picker on, so an effect card offers effects.

Stated in the name rather than worked out from the file's contents, and deliberately so.
The entry point a class defines already tells the engine which moment to call, but reusing that as the role would tie a UI filter to a language feature.
The day a modifier wants a per-frame `tick()`, every modifier would appear in effect pickers with nothing else changed.
The engine stays role-blind either way, so a class defining several moments is still legal.

| folder | run by | a script writes |
|---|---|---|
| `layouts/` | [MoonLive layout](../docs/moonmodules/light/layouts.md#moonlive) | where the lights physically are: `addLight(x, y, z)` |
| `effects/` | [MoonLive effect](../docs/moonmodules/light/effects.md#moonlive) | a color per light: `setRGB(index, r, g, b)`, or a whole shape with `line(x1, y1, x2, y2, r, g, b)` |
| `modifiers/` | [MoonLive modifier](../docs/moonmodules/light/modifiers.md#moonlive) | where one light lands: `setXYZ(xPos, yPos, zPos)` |
| `palettes/` | the `palette` control on [Drivers](../docs/moonmodules/light/supporting.md#drivers) | sixteen palette entries, once per frame |
| `services/` | [MoonLiveService](../docs/moonmodules/core/services.md#moonliveservice) | hardware reads that drive controls |

A folder may hold subfolders that group a collection, such as `effects/particles/` or `effects/hpwit/`. They exist only here: the catalog records each script's subfolder for its download. The device keeps every script in one flat folder, so a name is unique across all of them, and the build refuses a duplicate.

Each module ships one of these as its default, so the folder doubles as the reference for what a working script looks like.

`unit_MoonLiveScripts` compiles every file here, so a script that stops parsing when the language changes fails the build rather than waiting to be pasted into a device.

## How a script reaches a device

**The library, and how it reaches a device.** A device carries the NAMES of every library script and the text of none, so the picker offers the whole library while flash holds a few KB rather than a few hundred. A name the device does not have yet is marked with a cloud; picking it downloads that one script and it becomes an ordinary local file. A device therefore holds the handful it uses, which is the normal case: one layout describes the rig it is wired to and the rest are meaningless on it.

The browser does the downloading, not the device: it reads the script from GitHub and posts it to the device's own file endpoint. So the device needs no internet at any point, and a rig on an isolated network is served by whatever machine is looking at its UI. The script comes from the firmware's own release tag, so it always matches the engine that will run it.

**Sending one back.** A script you wrote or changed carries a **`↗`** button beside the editor. It opens GitHub with the script already filled in: a new script as a new file under `moonlive/`, a changed library script as an edit of the one that is there. GitHub forks the repository on your behalf when you propose it, so contributing needs a GitHub account and nothing else. The button appears only for a file in your own directory, since an untouched library copy is byte-identical to what is already upstream.

**`GET /api/scripts`** is what the picker reads: the library's names per role (`effects`, `layouts`, `modifiers`, `services`, `palettes`), the tag they are fetched from, and the directory a download lands in. The catalog is compiled into the firmware, generated from `moonlive/` at build time by `catalog_scripts.cmake`, so a script added to the repository reaches devices with no other change.

## Editing a shipped script forks it

A device keeps two copies of the library. The ones that ship live in `/.moonlive`; anything you edit
on the device is saved to `/moonlive`, and **your copy is the one that runs**. That is what makes an
edit reversible: the original is still there, so the card's delete button becomes a **revert arrow**
(↺) for a script you have changed, and pressing it brings the shipped version back without needing a
network.

The cost of that arrangement is that your copy also HIDES later updates to the shipped one, so the
card says which case it is in. Its status carries one of:

| status | what it means |
|---|---|
| (nothing) | the shipped script is running, unedited |
| `edited copy` | your version is running; the shipped one has not changed since you forked it |
| `edited copy, shipped one updated` | your version is running, and **the library has a newer one** |

The third is the one to act on: revert to take the new version (losing your changes), or keep yours
and ignore it. Nothing is decided for you, and nothing overwrites an edit.

Two details worth knowing.
Opening a shipped script and saving it unchanged creates no copy, so browsing the library leaves every script at the version it had.
The comparison runs against the version you forked from, recorded when the fork is made, so editing your own copy later keeps the comparison honest.
