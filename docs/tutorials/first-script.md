# Write your first script

An effect you write yourself, typed into the browser, running as native machine code on the device seconds later. No toolchain, no rebuild, no reflash and no reboot. These are the steps of [Write a MoonLive effect](../moonwalk.md#first-script) with the code.

You need a device with lights or the 3D preview, and [a light show already running](../moonwalk.md#first-light-show) so there is something to replace.

Add **MoonLive** under the Layer in **Effects**. It renders nothing and says `no script — set the script name`, on purpose. A fresh module compiling a default would light every new one up the same, and you would be editing someone else's code before writing your own.

## Four lines

In the **script** box, type a name ending in `.mle`: `mine.mle`. The card opens an editor. Type this:

```c
class MyEffect {
  void tick() {
    fill(0, 0, 40);
  }
}
```

Click away from the editor, or press Ctrl/Cmd+S. The lights turn dim blue. That is the whole loop: `tick()` runs once per frame, `fill(r, g, b)` writes every light, and the numbers are 0 to 255.

## Make it move

```c
class MyEffect {
  void tick() {
    fill(beatsin(30, 0, 100), 0, 40);
  }
}
```

Save. The color now pulses, because `beatsin(bpm, low, high)` returns a value sweeping between the bounds at the tempo you named. Every effect that breathes is doing some version of this.

## Give yourself a knob

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

Save, and a **speed** slider appears on the card. Drag it: the pulse follows. The slider is a real control, the same kind a compiled effect declares. It persists across a reboot and it is reachable over the API, because nothing about it is special-cased for scripts.

## Paint per light

`fill` writes every light the same. To make a pattern, write them one at a time:

```c
class MyEffect {
  byte speed = 30;

  void defineControls() {
    addControl("speed", speed, 1, 120);
  }

  void tick() {
    int n = width * height * depth;
    for (int i = 0; i < n; i = i + 1) {
      setRGB(i, i * 4 + beatsin(speed, 0, 255), 80, 120);
    }
  }
}
```

Save. Color now varies along the strand and drifts over time. `setRGB(index, r, g, b)` is the per-light write, `width * height * depth` is how many lights the layout placed, and the `for` is ordinary. A value past 255 wraps, which is what makes `i * 4` sweep through hues rather than saturating.

## When the compile fails

Break it on purpose: delete a closing brace and save. The lights go dark, the card shows the parse error, and the device keeps running. A bad script costs you a message, not a reboot, which is the point of editing code on a device that might be mounted three meters up a wall. Fix the brace, save, and it comes back.

## What you have

A working effect, written in a browser and running compiled on the device. It survives a reboot, it has a control, and it is a file you can copy to another device. [MoonLive](../moonmodules/light/moonlive.md) is the language: every function, the types, and the shipped library to read. [Making beautiful effects](../explanation/generative-effects.md) is what to write once you can write anything.
