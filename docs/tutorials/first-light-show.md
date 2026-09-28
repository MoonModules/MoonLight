# Build your first light show

You have lights running and you know [what the cards are](how-projectmm-works.md). Now you build something deliberately, instead of accepting what the defaults gave you: a shape you chose, an effect on top of it, a second effect blended into the first, and the whole thing going out to real lights.

Everything here happens in the device's own web interface, live. Nothing is compiled, nothing is saved and applied, nothing reboots. You change a number and the lights change while you are still holding the mouse.

You need a device with MoonLight on it and some lights attached, real or previewed. A bare board with no strip works the whole way through: the 3D preview is what you will be watching anyway.

## 1. Say where the lights are

Open **Layouts**. A fresh device has a **Grid**, which is the shape most rigs start as: a width, a height and a depth of evenly spaced points.

Set **width** to 16 and **height** to 16. The preview reshapes as you type, and the light count under the card follows.

<video src="../assets/uiscenarios/05-layouts.webm" controls playsinline width="720" title="Resizing a grid while it renders, then the other layout shapes the same effect paints into."></video>


That number is the whole point of a layout. Every effect downstream asks the layout how many lights there are and where each one sits, so this one card decides what the rest of the pipeline is painting on. Nothing else in the tree stores a size, which is why changing it here never leaves something stale behind.

If your strip zig-zags back and forth along the rows, turn on **serpentine**. Watch a running effect while you toggle it: a wrong setting shows up as every other row drawn backwards, which is unmistakable once seen.

## 2. Paint something on it

Open **Effects**. Under the Layer, press **+ add module** and pick an effect. **Bouncing Balls** is a good first choice: it has motion you can read at a glance, so the controls show their effect immediately.

Now change **numBalls** while it runs. Then **grav**. The lights respond as the slider moves, because an effect is not a rendered animation the device plays back: it is a function being run once per frame, reading its controls each time.


Try a second effect. Press **+ add module** again and add **Ripples** beside the first.

Both now run into the same Layer, in order, each writing over what the one before it left. That is useful when the second effect draws sparsely (sparks over a wash), and it is not blending: two effects in one Layer share one buffer.

Blending happens between **layers**. Press **+ add module** on the Effects card to add a second Layer, give it its own effect, and the Layer card carries a **blendMode** and an **opacity**. The drivers composite the layers bottom to top, so lowering the top layer's opacity tints what is underneath instead of replacing it.


This is the same model an image editor uses, and it is worth a minute of play. Layers compose; you are not picking one effect from a list.

## 3. Reshape it, leaving the effect alone

Under the Layer, add a **modifier**: **Mirror**.

The effect did not change. The modifier sits between the effect and the lights and folds the coordinates on the way through, so a pattern that ran across the whole grid now runs across half and reflects.


That separation is why a modifier is worth having at all. Mirror, rotate and multiply are things you want on *any* effect, and writing them into each effect would be the same code many times over.

All of it on one wall, start to finish: an effect in a layer, with its controls driven while it runs. Then a modifier reshaping it, and a second layer doing the same and blending over the first.

<video src="../assets/uiscenarios/06-layers.webm" controls playsinline width="720" title="An effect, a modifier, and a second layer blended over the first."></video>

## 4. Send it somewhere real

Open **Drivers**. Set **brightness** first, and set it low: 20 is plenty on a bench, and a full-brightness panel at arm's length is genuinely unpleasant.

Then add the driver for your hardware:

- **LED strip on a pin**: add an **RmtLedDriver** (or a **ParallelLedDriver** for many strands at once), set **pins** to the GPIO your data line is soldered to, and set the light count to match the layout.
- **Over the network**: add a **NetworkSendDriver** for Art-Net, E1.31/sACN or DDP, and give it the destination address.
- **Nothing attached**: the **Preview** driver is already there. That is what has been feeding the 3D view all along.

The driver is the only part of the tree that knows about wires. Everything upstream of it produced colors for positions, which is why the same show runs out a GPIO pin, across the network to a panel card, and into your browser at the same time.

## 5. Keep it

Everything you did is already persisted. Controls save themselves a couple of seconds after the last change. Once that save lands a power cut costs you nothing; pull the plug mid-turn of a knob and you lose the last second or two of fiddling.

Reboot the device if you want to prove it. The tree comes back as you left it.

## What you built

A layout, a layer with two effects and a modifier, and a driver. That is the whole pipeline, and it is the same pipeline whether it is driving twelve lights on a desk or twelve thousand on a wall.

Two directions from here, and they go to different places:

- **[Making beautiful effects](generative-effects.md)** is about the effects themselves: why noise looks organic and how a field turns into something worth watching.
- **[Build your own MoonModules](build-your-own-moonmodules.md)** is about writing one in C++ when the catalog does not have what you want.
