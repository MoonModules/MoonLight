# Save and recall presets

A preset is a saved state you can bring back with one click: a look you liked, a geometry you wired, a hardware setup you got right.

They live on the Control card as a grid of 64 pads. Click one to apply it.

> **Two different things are called a preset.** The pads on the Control card are what people usually mean, and what follows is about those. The **fixture profile** under Drivers names which channel carries red or pan: see [FixtureProfiles](../moonmodules/light/supporting.md#fixtureprofiles).

## Save one

Right-click a pad, or press and hold it on a touchscreen. The pad editor opens.

Type a name, choose which container it saves, and press **save current state here**.

Any card saves itself as well: press its `{ }` button, name the preset, and press **save as preset**. It lands on the first free pad and holds that card alone, so applying it puts back that card and leaves its neighbors running.

## Edit, rename or delete one

A click applies a pad; everything else starts from the same editor. Right-click a pad that holds a preset, or press and hold it on a touchscreen. A pad's tooltip says the same.

- **Change what it holds:** apply it, change what you want on the cards, then open its editor and press **save current state over it**. The radio buttons choose which container the new contents carry.
- **Rename it:** edit **name** in the editor. The new name applies on Enter or when you leave the field.
- **Delete it:** press **delete preset** in the editor. The pad empties.
- **Move it:** drag it onto another pad, which swaps the two, or onto an empty one.

Each preset is also a file, `/.config/presets/<name>.json`, so the File Manager edits or deletes it directly, and the pad follows the file.

## What a preset holds

A preset is a state document: the containers it sets, by name, with the modules and controls inside, the same document [`PATCH /api/state`](../reference/integrating.md#setting-everything-at-once) applies.

Saving from a pad captures one of four containers whole, chosen as a radio button:

| Capture | What it holds | What it is |
|---|---|---|
| **Effects** | the layer, its effects and modifiers | a look |
| **Layouts** | where the lights are | a geometry |
| **Drivers** | pins, brightness, outputs | a hardware setup |
| **Services** | sensors and bridges | a service configuration |

One container per save is what makes a look **portable**: an Effects preset applies on any board, because it carries no pin map. Add Drivers to it and it becomes a device snapshot tied to one rig's wiring, which is a different and much less shareable thing.

A preset written by hand, or added from elsewhere, can hold less than a container. This one sets only the palette and leaves the brightness, the pins and the running look alone:

```json
{ "Drivers": { "palette": "Ocean" } }
```

## Applying one

Click the pad. The look, geometry or setup replaces what was there.

**A saved container is a restore, not an overlay.** It carries `"$patch": "replace"`, so a preset carrying more modules than the device has adds them, and one describing fewer removes what it omits. That is what makes a pad reliable: what you saved is what comes back, rather than what you saved merged with whatever drifted since.

**One active preset per container**, so a layout preset and a look stay lit together. Applying a new look replaces only the look, and a palette preset holds the Drivers pad next to it.

## From the gallery

The **Gallery** under the pads lists what people shared in [MoonLight-Gallery](https://github.com/MoonModules/MoonLight-Gallery), most liked first. Your browser fetches it, so the device needs no internet of its own, and an entry shows as soon as it is accepted.

- **add to a pad** puts a preset on the first free pad, with any script it needs from the gallery.
- **try now** applies it without keeping it.
- **install** puts a script into `/moonlive`.
- **fill empty pads with the most liked** adds the best-liked looks and palettes until the grid is full. Presets that set pins or geometry are left out, since they belong to one rig, and your own pads stay where they are.

An entry made on a newer MoonLight than yours says so: update first.

To share one, press a card's `{ }` button, copy the JSON, and open an issue in the gallery as its [Sharing your own](https://github.com/MoonModules/MoonLight-Gallery#sharing-your-own) describes.

## Arranging the pads

Drag a pad to move it. A pad is a **position, not a list entry**: slot 14 stays slot 14 whether or not anything sits in it, and deleting slot 3 does not shuffle slot 4 into its place.

The order persists, stamped into each preset's own file, so a preset folder copied to another device brings its layout along.

## Where they live

One file per preset, at `/.config/presets/<name>.json`, holding the document and its pad as `"$slot"`. A name may use printable characters but no `/`, `\` or `.`, up to 31 characters.

They ride along in a [backup](backup-and-restore.md), which is how a rig's looks move to another device.

A preset file added, edited or removed in the File Manager shows on the pads at once.

## In Home Assistant

Only **Effects** presets travel to Home Assistant, where they appear as the light entity's effect list over MQTT, or in the native preset dropdown through the WLED integration.

Layouts and Drivers presets are deliberately excluded: they rewire pins and geometry, and should not be reachable from something that believes it is choosing a color scheme. Setting that up is in [Home automation](home-automation.md).

## When a preset refuses to apply

The card names the first failure and where it is, such as `no such control at Effects.Layer.Noise.speed`:

- **It names a container, a module type or a role this device does not have.** Found before anything changes, so nothing does.
- **A control inside it is unknown here, or a value is out of range.** Found as it is written, so what came before the failure stays applied.
- **It was saved by an older MoonLight.** Listed but not applied: a [backup and restore](backup-and-restore.md) converts it.
- **The file is malformed.** The live tree is untouched.
