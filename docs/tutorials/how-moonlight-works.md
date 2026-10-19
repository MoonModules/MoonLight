# How MoonLight works

You have lights running. Here is what you were looking at.

There is only **one idea** in MoonLight, and everything else follows from it. Ten minutes here and the rest of the interface stops being a wall of settings and becomes a place you know your way around.

> New here? Start with **[Install MoonLight](../moonwalk.md#install)** and come back once something is lit.

## The screen: list, preview, controls

![The MoonLight interface: navigation on the left, live preview in the middle, module cards on the right](../assets/gettingstarted/02-01-UI-large.png)

The interface runs in your browser, served by the device itself: no app, no account, no cloud. It has three regions, left to right. **The module list** holds every part of the device, from System at the top to the lights at the bottom. **The 3D preview** shows the lights in their real shape, as they are this instant. **The controls** are each module's settings, and the lights react as you drag a slider.

The layout follows the window. On a narrower screen the controls take the width and the preview becomes a floating thumbnail. Narrower still, everything stacks into one column, which works on a phone beside the lights.

![Medium width, preview as a floating thumbnail](../assets/gettingstarted/02-02-UI-mid.png)

![Small width, single column](../assets/gettingstarted/02-03-UI-small.png)

**The 3D preview** rotates with a drag and zooms with a scroll. Each dot is one light at its real position, in the color it shows this instant. The numbers give each light's index, handy when wiring or mapping a layout. The preview is a view of the device: it never slows the lights, and on a slow connection it sends fewer updates, then fewer points, rather than stalling. How it streams: [PreviewDriver](../moonmodules/light/drivers.md#preview).

![The 3D preview, lights numbered](../assets/gettingstarted/02-04-UI-Preview.png)

What a device adds on a freshly flashed board, and the same interface on a computer:

<video src="../assets/moontube/02-first-look-esp32.webm" controls playsinline width="720" title="Each part of a freshly flashed board and what a device adds: an LED driver on a pin, the I2C bus, the pins, a microphone, and updates over the air"></video>

<video src="../assets/moontube/02-first-look-desktop.webm" controls playsinline width="720" title="A tour of the interface: every module, and what sits inside each one."></video>

---

## 1. Everything is a card

Every single thing in MoonLight, the WiFi settings, an effect, an LED driver, the file manager, is a **MoonModule**. And every MoonModule is a **card**.

There is no second kind of thing. Learn to read one card and you can read them all.

![A Layouts card with a Grid card nested inside it](../assets/gettingstarted/02-08-UI-Layouts.png)

Look at that picture again, because every card has the same parts:

| What you see | What it means |
|---|---|
| **Layouts** ⚙️ | The name, and an emoji hinting at what it does |
| 🕐 **250K fps** | How fast this module runs, live |
| 🧠 **112B** | How much memory it uses, live |
| **⏻** | Turn it off without deleting it |
| The line under the name | The module's help, once the ⓘ in the status bar turns help on |
| **`{ }`** | Open its live state, for [issue reports](../reference/logging-an-issue.md) |
| `status`, `width`, `height` … | The settings, one per row |
| The **Grid** box inside | A child card, cards nest |
| **+ add module** | Add a child here |

Two of those are unusual and worth pausing on.

**The timing and memory are per module, and they are live.** Most software hides this. MoonLight shows you exactly what each part costs, so when something feels slow you can see which card is responsible instead of guessing.

**The nesting is the structure.** A child card is drawn *inside* its parent's border. When you see `Grid` inside `Layouts`, that is not decoration: the Grid belongs to Layouts. The box is the truth.

---

## 2. Two families of card

Cards come in two kinds, and knowing which is which tells you where to look for anything.

### Core: the device itself

Identity, network, files, firmware. This half knows *nothing* about lights: it would be identical on a machine driving motors or synthesising audio. These cards are fixed, you configure them, you don't add or remove them.

![The System card, showing device name, chip, memory and uptime](../assets/gettingstarted/02-05-UI-System.png)

| Card | What it's for |
|---|---|
| **System** | Device name, chip, memory, uptime, why it last rebooted |
| **Network** | WiFi and Ethernet, plus MQTT and finding other devices |
| **File Manager** | Browse, edit and upload files on the device |
| **Firmware** | Your version, and updates over the air |
| **Services** | Hardware that isn't lighting, a microphone, a sensor |
| **Control** | Drive controls from MIDI, a remote, or another device |

### Light: what you build with

Lights, colors, and getting the bytes out. These cards are yours: add them, remove them, rearrange them freely.

| Card | What it's for |
|---|---|
| **Layouts** | Where the lights physically ARE |
| **Effects** | What color they are, over time |
| **Drivers** | How the colors reach the lights |

That split is why the menu is ordered the way it is: the device first, then the three light cards.

**Lights are one use among many.** Everything in the core half, the modules, the live controls, the 3D view, the web interface and the networking, knows nothing about LEDs. The light cards are one domain built on top of it, and another domain could reuse all the same machinery. [FastLED-MM](https://github.com/MoonModules/FastLED-MM) is one, driving its LEDs with [FastLED](https://github.com/FastLED/FastLED), on hold until MoonLight ships as a library.

---

## 3. The light pipeline

Those three light cards are not a random list. They are a **pipeline**, and they run in order, every frame:

```text
   Layouts    ──→    Effects    ──→    Drivers
    where            what color        how it gets out
```

<!-- IMAGE PLACEHOLDER: a friendly left-to-right diagram of the three stages.
     Stage 1 shows a grid of grey dots (positions only); stage 2 the same grid with color flowing across it; stage 3 an arrow leaving toward a real LED strip and a network cable. This is the single most valuable new image on the page, the pipeline is the concept everything else hangs off. -->

**1. A Layout decides where the lights are.** A 16×16 grid, a ring of 60, two rows of 24, a shape you draw yourself. It answers one question: *how many lights, and where is each one?*

![Layouts](../assets/light/Layouts.png)

**2. An Effect paints them.** It asks the layout for the coordinates and writes colors, every frame, forever. Effects live inside a **Layer**. Two effects in one Layer write into one buffer in order, each over what the one before it left. That suits a sparse effect over a wash. Blending happens between Layers, as in an image editor. Each Layer carries a **blendMode** and an **opacity**, and the drivers composite the Layers bottom to top. An effect is a function run once per frame, reading its controls each time, so a control moved while it runs shows at once.

![A plasma effect running](../assets/light/effects/PlasmaEffect.gif)

**3. A Driver sends the result somewhere.** Out a GPIO pin to a strip, across the network as ArtNet or DDP, or to the preview in your browser.

![Drivers](../assets/light/Drivers.png)

### Why this order matters

Because the stages are separate, **changing one doesn't break the others**.

Resize your panel from 16×16 to 32×32 and every effect keeps running: it asks the layout a new question and gets new coordinates back. That is why the same effect works on a grid, a ring, or a wall of 12,288 lights without being rewritten.

<video src="../assets/moontube/05-layouts.webm" controls playsinline width="720" title="Resizing a grid while it renders, then the other layout shapes the same effect paints into."></video>

**Modifiers** sit between layout and effect and bend the mapping: mirror it, fold it, tile it. The effect itself is untouched, so mirror, rotate and multiply work on *any* effect without each effect carrying them. An effect written for one strip can drive a whole wall without ever knowing it.

<video src="../assets/moontube/06-layers.webm" controls playsinline width="720" title="An effect in a layer with its controls driven while it runs, then a modifier reshaping it, and a second layer blending over the first."></video>

---

## 4. Everything applies live

**Settings need no save button, and nothing needs a reboot.**

Move a slider and the next frame uses it. Change the WiFi credentials, add an effect, resize a grid, all of it takes effect immediately, on a running device.

<video src="../assets/moontube/03-second-look.webm" controls playsinline width="720" title="Every control a card carries, pressed in turn: each one applies on the next frame."></video>

**Files are the one exception, and only because typing is different.** A script you are editing is saved when you click away, press Ctrl/Cmd+S, or press Save. A half-typed line should not be compiled onto your fixture mid-word. The moment it is saved it recompiles and swaps in live, same as everything else.

This is a deliberate design rule, not a convenience, and you can trust it: if something needs a reboot before it works, that is a bug worth [reporting](../reference/logging-an-issue.md).

Your settings save themselves and survive a power cycle.

---

## 5. The emoji on every card

Each card and every row of the picker carries a few emoji. They are a filter, not decoration. The picker's chip row narrows a long list to the effects that listen to music, or the layouts that build a volume. So an emoji only exists where it groups several modules, and a module that fits no group carries none.

Three come from what the module IS, and the interface adds them for you:

| | |
|---|---|
| 🔥 effect · 💎 modifier · 🚥 layout · ☸️ driver · 🛰️ service · 🥞 layer · ⚙️ generic | what kind of card it is |
| 📏 1D · 🟦 2D · 🧊 3D | the shape it works in: a line, a picture, a volume |

The rest the module declares about itself:

| | |
|---|---|
| 💫 MoonLight · 🌙 MoonModules · 🐙 WLED · ⚡️ FastLED | where it came from |
| 🦅 | a named contributor, credited on the module |
| 🎵 volume · 🎶 frequency | it listens: one note reacts to how LOUD the room is, two to WHICH notes are playing |
| 🖼️ | it shows a picture made elsewhere: from the network, the Picture service or a recorded show |
| 🎯 | it aims moving heads |
| 👾 | pixel art: the games and sprites |
| 🧬 | a simulation: the picture emerges from cells evolving off their own last frame, rather than being drawn |

And a last group, shown together at the end of the row, saying which **power functions** the effect is built on. These are the kernels of [the shared library](../moonmodules/light/power-functions.md), so they group effects by what they are made of, and by what that makes them look like:

| | |
|---|---|
| 🖌️ | a shader: every pixel computed from its own position, the way a screen shader works |
| ✨ | particles: sparks that are born, move under forces and die |
| 💨 | transport: light is CARRIED and fades rather than redrawn, so the effect has a memory of where it has been |
| 🌫️ | a noise field: texture sampled from a field rather than drawn, the cloud and smoke family |
| 🎡 | polar: composed around a center rather than across a grid, which is what suits a round fixture |
| 📹 | motion-tracking aware: it follows people or objects moving in the room *(reserved, nothing carries it yet)* |

A module can carry several: `💫🎶` is a MoonLight effect that reacts to frequency.

---

## Where to go next

Now that you know what the cards are, go build something:

- **[Effects](../moonmodules/light/effects.md)**: the catalog, with pictures
- **[Layouts](../moonmodules/light/layouts.md)**: grids, rings and custom shapes
- **[Modifiers](../moonmodules/light/modifiers.md)**: mirroring, folding, tiling
- **[Drivers](../moonmodules/light/drivers.md)**: LED strips, ArtNet, DMX
- **[Live scripting](../moonmodules/light/moonlive.md)**: write your own effect in the browser; it compiles on the device and runs as native code
