# How MoonLight works

You have lights running. This page explains what you were actually looking at.

There is really only **one idea** in MoonLight, and everything else follows from it. Ten minutes here and the rest of the interface stops being a wall of settings and becomes a place you know your way around.

> New here? Start with **[Install & first light](../gettingstarted.md)** and come
> back once something is lit.

![The MoonLight interface: navigation on the left, live preview in the middle, module cards on the right](../assets/gettingstarted/02-01-UI-large.png)

<video src="../assets/uiscenarios/02-first-look-desktop.webm" controls playsinline width="720" title="A tour of the interface: every module, and what sits inside each one."></video>

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
| **?** | Open this module's documentation |
| **`{ }`** | Open its live state, for [issue reports](../how-to/logging-an-issue.md) |
| `status`, `width`, `height` … | The settings, one per row |
| The **Grid** box inside | A child card, cards nest |
| **+ add module** | Add a child here |

Two of those are unusual and worth pausing on.

**The timing and memory are per module, and they are live.** Most software hides
this. MoonLight shows you exactly what each part costs, so when something feels slow you can see which card is responsible instead of guessing.

**The nesting is the structure.** A child card is drawn *inside* its parent's
border. When you see `Grid` inside `Layouts`, that is not decoration, the Grid really does belong to Layouts. The box is the truth.

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
| **Drivers** | How the colors actually reach the lights |

That split is why the menu is ordered the way it is: the device first, then the three light cards.

---

## 3. The light pipeline

Those three light cards are not a random list. They are a **pipeline**, and they run in order, every frame:

```text
   Layouts    ──→    Effects    ──→    Drivers
    where            what color        how it gets out
```

<!-- IMAGE PLACEHOLDER: a friendly left-to-right diagram of the three stages.
     Stage 1 shows a grid of grey dots (positions only); stage 2 the same grid with color flowing across it; stage 3 an arrow leaving toward a real LED strip and a network cable. This is the single most valuable new image on the page, the pipeline is the concept everything else hangs off. -->

**1. A Layout decides where the lights are.**
A 16×16 grid, a ring of 60, two rows of 24, a shape you draw yourself. It answers one question: *how many lights, and where is each one?*

![Layouts](../assets/light/Layouts.png)

**2. An Effect paints them.**
It asks the layout for the coordinates and writes colors, every frame, forever.
Effects live inside a **Layer**, and a Layer can hold several, stacked and blended like layers in an image editor.

![A plasma effect running](../assets/light/effects/PlasmaEffect.gif)

**3. A Driver sends the result somewhere.**
Out a GPIO pin to a strip, across the network as ArtNet or DDP, or to the preview in your browser.

![Drivers](../assets/light/Drivers.png)

### Why this order matters

Because the stages are separate, **changing one doesn't break the others**.

Resize your panel from 16×16 to 32×32 and every effect keeps running, it simply asks the layout a new question and gets new coordinates back. That is why the same effect works on a grid, a ring, or a wall of 12,288 lights without being rewritten.

**Modifiers** sit between layout and effect and bend the mapping, mirror it,
fold it, tile it. An effect written for one strip can drive a whole wall without ever knowing it.

<!-- IMAGE PLACEHOLDER: before/after of one effect with a mirror modifier
     applied, the same frame, un-mirrored and mirrored, side by side. Makes "a modifier folds the mapping" obvious in one glance. -->

---

## 4. Everything applies live

**Settings need no save button, and nothing needs a reboot.**

Move a slider and the next frame uses it. Change the WiFi credentials, add an effect, resize a grid, all of it takes effect immediately, on a running device.

<video src="../assets/uiscenarios/03-second-look.webm" controls playsinline width="720" title="Every control a card carries, pressed in turn: each one applies on the next frame."></video>

**Files are the one exception, and only because typing is different.** A script
you are editing is saved when you click away, press Ctrl/Cmd+S, or press Save, a half-typed line should not be compiled onto your fixture mid-word. The moment it is saved it recompiles and swaps in live, same as everything else.

This is a deliberate design rule, not a convenience, and you can trust it: if something needs a reboot before it works, that is a bug worth [reporting](../how-to/logging-an-issue.md).

Your settings save themselves and survive a power cycle.

---

## 5. The emoji on every card

Each card and every row of the picker carries a few emoji. They are a filter, not decoration: the picker's chip row lets you narrow a long list to the effects that listen to music, or the layouts that build a volume. So an emoji only exists where it groups several modules, and a module that fits no group carries none.

Three come from what the module IS, and the interface adds them for you:

| | |
|---|---|
| 🔥 effect · 💎 modifier · 🚥 layout · ☸️ driver · 🛰️ service · 🥞 layer · ⚙️ generic | what kind of card it is |
| 📏 1D · 🟦 2D · 🧊 3D | the shape it works in: a line, a picture, a volume |

The rest the module declares about itself:

| | |
|---|---|
| 💫 MoonLight / MoonLight · 🌙 MoonModules · 🐙 WLED · ⚡️ FastLED | where it came from |
| 🦅 | a named contributor, credited on the module |
| 🎵 volume · 🎶 frequency | it listens: one note reacts to how LOUD the room is, two to WHICH notes are playing |
| 📡 | it takes its picture from the network |
| 🎯 | it aims moving heads |
| 👾 | pixel art: the games and sprites |
| 🧬 | a simulation: the picture emerges from cells evolving off their own last frame, rather than being drawn |

And a last group, shown together at the end of the row, saying which **power functions** the effect is built on. These are the kernels of [the shared library](../moonmodules/light/power-functions.md), so they group effects by what they are made of, and by what that makes them look like:

| | |
|---|---|
| 🖌️ | a shader: every pixel computed from its own position, the way a screen shader works |
| ✨ | particles: sparks that are born, move under forces and die |
| 🌊 | a fluid: a medium that works out its own motion, so a vortex forms and travels because the equations say so |
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
- **[Live scripting](../moonmodules/light/moonlive.md)**: write your own
  effect in the browser; it compiles on the device and runs as native code
