# Save and recall presets

A preset is a saved state you can bring back with one click: a look you liked, a geometry you wired, a hardware setup you got right.

They live on the Control card as a grid of 64 pads. Click one to apply it.

> **Two different things are called a preset.** The pads on the Control card are what people usually mean, and what follows is about those. The **fixture profile** under Drivers names which channel carries red or pan: see [FixtureProfiles](../moonmodules/light/supporting.md#fixtureprofiles).

## Save one

Right-click a pad, or press and hold it on a touchscreen. The pad editor opens.

Type a name, choose what to capture, and press **save current state here**.

To overwrite, do the same on a pad that already holds one: the editor offers **save current state over it**, a rename, and **delete preset**.

## What a preset captures

Exactly one of four subtrees, chosen as a radio button rather than a set of checkboxes:

| Capture | What it holds | What it is |
|---|---|---|
| **Effects** | the layer, its effects and modifiers | a look |
| **Layouts** | where the lights are | a geometry |
| **Drivers** | pins, brightness, outputs | a hardware setup |
| **Services** | sensors and bridges | a service configuration |

One subtree, never a combination, and that is the whole model. It is what makes a look **portable**: an Effects preset applies on any board, because it carries no pin map. Add Drivers to it and it becomes a device snapshot tied to one rig's wiring, which is a different and much less shareable thing.

The combinations used to be expressible, and they were the hard part to explain and the hard part to display.

## Applying one

Click the pad. The look, geometry or setup replaces what was there.

**It is a restore, not an overlay.** A preset carrying more modules than the device has adds them; one describing fewer removes what it omits. That is what makes a pad reliable: what you saved is what comes back, rather than what you saved merged with whatever drifted since.

**One active preset per role**, so a layout preset and a look stay lit together. Applying a new look replaces only the look.

## Arranging the pads

Drag a pad to move it. A pad is a **position, not a list entry**: slot 14 stays slot 14 whether or not anything sits in it, and deleting slot 3 does not shuffle slot 4 into its place.

The order persists, stamped into each preset's own file, so a preset folder copied to another device brings its layout along.

## Where they live

One file per preset, at `/.config/presets/<name>.json`. A name may use printable characters but no `/`, `\` or `.`, up to 31 characters.

They ride along in a [backup](backup-and-restore.md), which is how a rig's looks move to another device.

Adding or removing preset files by hand in the File Manager has one catch. The pad grid rebuilds its list when the module next rescans: at startup, or after a save, rename or delete on the card. A file dropped in does not appear the instant it lands.

## In Home Assistant

Only **Effects** presets travel to Home Assistant, where they appear as the light entity's effect list over MQTT, or in the native preset dropdown through the WLED integration.

Layouts and Drivers presets are deliberately excluded: they rewire pins and geometry, and should not be reachable from something that believes it is choosing a color scheme. Setting that up is in [Home automation](home-automation.md).

## When a preset refuses to apply

The card says why rather than applying half of it:

- **It names a subtree this firmware does not have.** Refused whole.
- **It carries several roles**, having been written by an older build. Listed but not applied, with `re-save it` as the fix, so you can see it and decide rather than watch it vanish.
- **The file is malformed.** The live tree is untouched.
