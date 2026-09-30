# Log an issue

Something not working? Tell us. You do not need to diagnose it, describe what you saw, and hand us the state of the device it happened on. The **`{ }`** link on every module card gives you that in one click, and it is the single most useful thing you can attach.

👉 **[Open an issue on GitHub](https://github.com/MoonModules/MoonLight/issues)**

Not sure it's a bug, or want to talk it through first?
**[Discord](https://discord.gg/TC8NSUSCdV)** is the right place for "is this
supposed to work like this?".

---

## Checklist

- Search the [existing issues](https://github.com/MoonModules/MoonLight/issues)
  first, it may already be reported, or already fixed
- Check the Firmware card: are you on the current version?
- For bugs, include the `{ }` output (below)
- For feature requests, describe the use case, no template needed

---

## Bug report template

### 1. Describe the problem

```text
Steps to reproduce:
1. ...
2. ...

Expected behavior:
[What should happen]

Actual behavior:
[What happens instead]
```

### 2. Attach the `{ }` output

⚠️ **This is the most useful diagnostic data.** It is a JSON snapshot of the module's live state, its type, every control value, its status line and its timing, exactly as it was when things went wrong.

**How to get it:**

1. Find the card that misbehaved, the effect that went dark, the driver that
   isn't lighting, the network card that won't connect
2. Click the **`{ }`** link in its title row, next to the `?`
3. Select all in the new tab (Ctrl/Cmd+A), copy, and paste it into the issue

**Which cards to include:**

1. **Always: System.** Names the chip, firmware variant, build, SDK version and
   the last boot reason, how we tell a board problem from a firmware one
2. **Always: Firmware.** The exact version and build you are running
3. **The affected card itself**, Effects, Drivers, Layouts, Network, whichever
   one went wrong
4. **If it is about lights:** the Layer card, which carries the geometry
5. **If it is about pins or wiring:** the Drivers card

Wrap each one in a fenced code block so the issue stays readable:

````markdown
API output:

- System
```text
{"name":"System","type":"SystemModule","controls":[{"name":"chip","value":"ESP32-S3"},...]}
```

- Effects
```text
{"name":"Effects","type":"Effects","children":[...]}
```
````

### 3. Add a photo

If it is something you can see, a fixture showing the wrong colors, a UI in a state that looks wrong, a photo or screenshot says it faster than a paragraph.

---

## If the device crashed or won't boot

The `{ }` link needs a device that still serves its interface. When it doesn't, tell us instead:

- **What the LEDs did**: nothing at all, a brief flash, a repeating pattern
- **What changed** just before it started, even if it seems unrelated: a setting,
  a script edit, a firmware update, a cable moved
- **Does it repeat?** Power-cycle it. A device that boots once and fails the next
  time is a different problem from one that never boots
- **The serial boot log**, if you have the device on USB and can run a monitor.
  If you can't, say so, we will not ask you to set up a toolchain to file a bug

One more thing worth checking yourself: `bootReason` on the System card. `PANIC`, `TASK_WDT` or `BROWNOUT` means the device crashed or lost power rather than merely misbehaving, and saying so in the first line of the report saves a round trip.

---

## Feature requests

Describe the use case, what you expect it to do, and what you have tried instead.
No template, no diagnostics, just tell us what you want to build.
