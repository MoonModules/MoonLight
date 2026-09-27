# Write your first script

An effect you write yourself, typed into the browser, running as native machine code on the device seconds later. No toolchain, no rebuild, no reflash, and no reboot.

This is MoonLive. The reference for the language is [MoonLive](../moonmodules/light/moonlive.md), and what follows is the shortest path to seeing your own code drive real lights.

You need a device with lights or the 3D preview, and [a light show already running](first-light-show.md) so there is something to replace.

## 1. Add a scripted effect

Open **Effects**, press **+ add module** under the Layer, and add **MoonLive**.

It renders nothing and says `no script — set the script name`. That is deliberate: a fresh module compiling a default would mean every new one lights up the same, and you would be editing someone else's code before writing your own.

## 2. Write four lines

In the **script** box, type a name ending in `.mle`: `mine.mle`. The card opens an editor.

Type this:

```c
class MyEffect {
  void tick() {
    fill(0, 0, 40);
  }
}
```

Click away from the editor, or press Ctrl/Cmd+S. The lights turn dim blue.

<video src="../assets/uiscenarios/97-write-an-effect.webm" autoplay loop muted playsinline width="720" title="Adding a MoonLive effect, picking a script, and typing one of your own"></video>


That is the whole loop. `tick()` runs once per frame, `fill(r, g, b)` writes every light, and the numbers are 0 to 255.

## 3. Make it move

Change the body to read:

```c
class MyEffect {
  void tick() {
    fill(beatsin(30, 0, 100), 0, 40);
  }
}
```

Save. The blue now pulses, because `beatsin(bpm, low, high)` returns a value sweeping between the bounds at the tempo you named. Every effect that breathes is doing some version of this.

## 4. Give yourself a knob

A number typed into a script is a decision you have to re-edit. A control is one you can turn while watching:

```c
class MyEffect {
  byte speed = 30;

  void defineControls() {
    addControl("speed", speed, 1, 120);
  }

  void tick() {
    fill(beatsin(speed, 0, 100), 0, 40);
  }
}
```

Save, and a **speed** slider appears on the card. Drag it: the pulse follows.

The slider is a real control, the same kind a compiled effect declares. It persists across a reboot and it is reachable over the API, because nothing about it is special-cased for scripts.

## 5. Paint per light

`fill` writes every light the same. To make a pattern, write them one at a time:

```c
class MyEffect {
  byte speed = 30;

  void defineControls() {
    addControl("speed", speed, 1, 120);
  }

  void tick() {
    for (int i = 0; i < lightCount; i = i + 1) {
      setRGB(i, i * 4 + beatsin(speed, 0, 255), 80, 120);
    }
  }
}
```

Save. Color now varies along the strand and drifts over time.

`setRGB(index, r, g, b)` is the per-light write, `lightCount` is how many the layout placed, and the `for` is ordinary. A value past 255 wraps, which is what makes `i * 4` sweep through hues rather than saturating.

## When the compile fails

Break it on purpose: delete a closing brace and save.

The lights go dark, the card shows the parse error, and the device keeps running. A bad script costs you a message, not a reboot, which is the point of editing code on a device that might be mounted three meters up a wall.

Fix the brace, save, and it comes back.

## What you have

A working effect, written in a browser, running compiled on the device. It survives a reboot, it has a control, and it is a file you can copy to another device.

Where to go next:

- **[MoonLive](../moonmodules/light/moonlive.md)** is the language: every function, the types, and the shipped library to read. The same engine writes layouts and modifiers, not only effects.
- **[Making beautiful effects](generative-effects.md)** is the ideas half: what to write once you can write anything.
