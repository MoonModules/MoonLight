# Driving LED panels with a receiving card

You bought a panel receiving card, most likely a **ColorLight** one, which is the family MoonLight supports today. This page takes you from a box of parts to a lit wall, on an ESP32 or from a desktop.

> New here? Start with **[Install & first light](../gettingstarted.md)**, then **[How MoonLight works](../tutorials/how-projectmm-works.md)**. What follows assumes you can find a card and change a control.

---

## The short version

If you have the card wired and MoonLight running, this is the whole sequence. Every step has its own section below.

1. **Layouts** → describe your wall ([§5.4](#54-describe-the-wall))
2. **Effects** → pick an effect, so there is something to send
3. **Drivers** → press **+** → choose **PanelCard** ([§5.5](#55-add-the-driver))
4. **`interface`** → desktop only: `en0` (macOS), `eth0` (Linux), or any part of the adapter name on Windows. Leave blank on an ESP32 ([§6](#6-from-a-desktop))
5. **`format`** and **`firmware`** → match your card ([§7](#7-card-firmware-and-the-flicker))
6. **brightness** → turn up BOTH the global `brightness` on the Drivers card and the PanelCard's own `localBrightness`; either one left low leaves the wall dark

The status line on the PanelCard card tells you where you are: it should read a link speed and a packet rate. Anything else is in [§8](#8-when-it-does-not-light-up).

**PanelCard missing from the driver list?** It is compiled only into firmwares for boards with Ethernet (S3, P4, S31), so a classic-ESP32 build does not offer it. Only the S31 is gigabit; the S3 and P4 are 100 Mbit and want a gigabit switch in between ([§2](#the-one-hardware-fact-that-decides-everything)). Check the Firmware page for the variant you are running.

---

## 1. What you actually bought

An LED wall is normally driven by two boxes. A **sending card** (a PCI-E board in a PC, or a standalone unit) takes video in and puts a specialised signal on Ethernet. A **receiving card** sits in each cabinet, decodes that signal, and drives the panels over HUB75 ribbon cables. The card you bought is a receiving card.

**MoonLight takes the sending card's place.** The board renders the effect and emits the frames itself, so there is no PC in the installation and no sending card to buy.

That has one consequence worth understanding before you wire anything: these frames are **raw Ethernet**, below IP. No address, no port, no DHCP. The cable between your controller and the card is not a network in the usual sense; it is a private link carrying pixel data, and nothing else should be on it.

Supported: **ColorLight 5A-75** (5A-75B and 5A-75E use the same wire format).

---

## 2. What you need

| Part | Notes |
|---|---|
| ColorLight 5A-75 receiving card | The one this guide is about |
| HUB75 panels | Any size; you tell MoonLight the geometry later |
| 5 V power supply | Sized from the panels' own rating: see the note on power in [§5.3](#53-wire-it) |
| Cat5e or Cat6 cable | Controller → the card's **input** port |
| A controller | An ESP32 board ([§5](#5-on-an-esp32)) or a desktop/Pi ([§6](#6-from-a-desktop)) |
| A gigabit switch | Only if your controller is a **P4 or an S3**, whose Ethernet is 100 Mbit: the switch lets the card negotiate a gigabit link on its own side and re-times the frames toward it. It does **not** make the controller faster: the 100 Mbit leg and its wire time ([§2](#the-one-hardware-fact-that-decides-everything)) remain, so a large wall still wants an S31. An **S31 is gigabit already** and connects straight to the card. |
| A USB gigabit Ethernet dongle | Recommended on Windows. Not for MoonLight, which drives these cards fine from a built-in port, but for **LEDUpgrade**: if it cannot find the card through your built-in adapter, a dongle is the known way through. See [§7](#7-card-firmware-and-the-flicker). |

### The one hardware fact that decides everything

**These cards want a 1 Gbit link.** Not for bandwidth, since a 256×256 wall at 40 fps is only about 65 Mbit/s, which 100 Mbit would carry comfortably. It is about **wire time**.

The cards have no buffering and no flow control. They latch the image when the sync frame arrives, so an entire frame has to land inside the gap between frames. At gigabit a 256×256 frame is ~1.6 ms on the wire; at 100 Mbit the identical bytes take ~16 ms, which overruns the budget and breaks the timing the latch depends on.

The failure mode is the confusing part: **nothing errors**. The link is up, frames go out, and the panels tear, show wrong rows, or never latch. That is why MoonLight reads the *negotiated* speed and warns you, rather than letting a slow link look like a format bug. It still sends, since a small wall on 100 Mbit is often fine, but if your picture is unstable, check this first.

The cards can also be picky about negotiating a gigabit link with a 100 Mbit controller. A gigabit switch in between is the remedy: the card negotiates gigabit with the switch, the switch buffers, and the controller's slower link stops being the card's problem. This applies to the **P4 and the S3**.
An **S31 is gigabit on its own** and connects directly.

---

## 3. What you will find here

| Section | What it covers |
|---|---|
| [4. Set the panels up in LED Vision](#4-set-the-panels-up-in-led-vision) | Telling the card what panels it drives, before anything renders |
| [5. On an ESP32](#5-on-an-esp32) | Board, flash, wiring, layout, driver |
| [6. From a desktop](#6-from-a-desktop) | The same, on Windows, Linux, a Pi or a Mac, plus the raw-Ethernet permission each one needs |
| [7. Card firmware, and the flicker](#7-card-firmware-and-the-flicker) | The v13 defect, and the LED Upgrade 4.0 downgrade that clears it |
| [8. When it does not light up](#8-when-it-does-not-light-up) | Symptom to cause |

The two halves are independent: an ESP32 and a desktop drive the same card the same way, and neither is a prerequisite for the other. Pick whichever hardware you have.

---

## 4. Set the panels up in LED Vision

The receiving card has to know what it is driving before MoonLight sends it anything: how big each panel is, how many there are, and which driver IC they use. That configuration lives **on the card**, written once with ColorLight's own **[LEDVision](https://en.colorlightinside.com/product/download/380)**, and it is why MoonLight itself needs no panel wiring settings at all (see [§5.4](#54-describe-the-wall)).

**Which version.** An **8.x** build is what people running these cards in this scene actually use:
this project's own wall is set up with **8.8**, and the walkthrough linked below uses **8.5**. Newer releases exist, and whether they are equally suitable here has not been established, so the safe advice is to take an 8.x build and only move if you have a reason to.

> **Worth watching first:** [Setting up a Colorlight Card with FPP v6.3 and LED Vision 8.5](https://www.youtube.com/watch?v=L4lHbwUszAs)
> walks through the whole card-and-panel setup on video. It drives the card from FPP rather than
> MoonLight, but everything up to the sender is the same job, and seeing it done is worth more than
> any written step list. The steps below cover the same ground in short form.

> The steps below are written from how LEDVision generally works, not from a verified run on this
> project's bench. Labels and menu paths move between versions; treat the shape as right and the
> exact wording as approximate.

1. Connect the card to the machine's Ethernet port, and power the panels and the card.
2. Open LEDVision and go to the **receiving-card** setup (usually `Settings` then a receiving-card
   or `Screen` panel; some versions ask for a password, commonly `168`).
3. Load the panel definition. Either pick your panel from the built-in module list, or load the
   `.rcfgx` / `.rcvx` file the panel supplier provided, which is the reliable route for a panel that is not a well-known model.
4. Set the **cabinet** size: how many pixels one card drives, across and down.
5. Set the **panel arrangement**: how the HUB75 ribbons chain, and which physical panel is first.
   This is the step that makes the card, not MoonLight, responsible for panel order.
6. **Send to the receiving card**, then **Save** so the configuration survives a power cycle. Saving
   is a separate action from sending in most versions, and skipping it is the usual reason a wall comes back wrong after being unplugged.

When this is right, a test pattern from LEDVision fills the wall correctly. Get to that point before introducing MoonLight: it separates "the panels are wired and configured" from "the sender works".

---

## 5. On an ESP32

### 5.1 Pick a board

| Board | Link | Verdict |
|---|---|---|
| **ESP32-S31** (Function-CoreBoard-1) | **1 Gbit** (YT8531) | The right board. On-chip EMAC at gigabit is exactly what these cards want. |
| **ESP32-P4** (Waveshare P4-NANO) | 100 Mbit (IP101) | Works for a small wall. A gigabit switch in between is reported to help. Use the **`-eth-wifi`** firmware: the Ethernet port is carrying panel data, so WiFi is how you reach the UI. |
| **ESP32-S3** (DevKitC-1, N16R8 or N8R8) | 100 Mbit over SPI (W5500 module) | The board most people already own, and a real way to try a small panel before buying an S31. The slowest of the three: budget for a gigabit switch in between. |

Other ESP32 variants do not ship panel-card support: their Ethernet is 100 Mbit at best, and most have none at all.

The S3 needs a **W5500 Ethernet module** wired to its SPI pins, which the S3 firmwares already support; its pins come from the board entry in `deviceModels.json`. Because W5500 is 100 Mbit and sits behind SPI, it is the configuration most likely to need the gigabit switch described in [§2](#the-one-hardware-fact-that-decides-everything).

### 5.2 Flash it

Use the [web installer](https://moonmodules.org/projectMM/install/), or from a checkout:

```sh
uv run moondeck/build/flash_esp32.py --firmware <esp32s31|esp32p4rev1-eth-wifi|esp32s3-n16r8> --port <port>
```

Panel-card support is compiled in per firmware, and it is already on for the boards in the table above.

### 5.3 Wire it

1. **Controller → card.** Ethernet cable from the board to the card's *input* (some cards have two RJ45 ports; the second is for daisy-chaining to the next cabinet).
2. **Card → panels.** HUB75 ribbons from the card's outputs to the panels, in the order you intend to address them.
3. **Power.** 5 V to the panels *and* to the card.

**On powering panels from the same supply as the board.** This works, and plenty of small setups run
that way. What it costs you is headroom.

HUB75 panels draw far less than a naive count suggests, and the reason is **multiplexing**: the panel lights one group of scan rows at a time, cycling fast enough to look continuous, so at 1/16 or 1/32 scan only a fraction of the LEDs are on at any instant. This is why these panels have a reputation for modest consumption. Size the supply from the rating on your panel's own datasheet rather than from pixels multiplied by LED current.

The failure when you do run short is not a clean one: the 5 V rail sags, and a sagging rail shows up as flicker, color shifts, or the controller resetting mid-frame. None of those look like a power problem, which is why they cost an evening. A wall that is stable at 30% brightness and misbehaves at 100% is telling you this is the problem, not the network.

Nothing needs an IP address: the panel link is below IP entirely.

**That is also how you reach the UI.** The board's Ethernet port is now carrying panel data, so
WiFi is what serves the web interface. Leave WiFi configured as normal; it is unrelated to the panel link and the two do not interfere. On the P4 this decides which firmware to flash, because the `-eth` variant has no WiFi compiled in at all: use `-eth-wifi`. The S31 and S3 firmwares carry both already.

### 5.4 Describe the wall

The driver has **no geometry controls**. The wall's shape lives in the Layout, once, so that everything else (effects, modifiers, the preview) sees the same picture.

**A plain Grid is usually all you need.** Two 128x64 panels stacked is a 128x128 grid, and that is
the whole configuration. The reason it is that simple is worth knowing: the driver reads only the wall's width and height and sends the image row by row. Which physical panel a row lands on, and in what order the HUB75 ribbons chain, was already settled on the card in [§4](#4-set-the-panels-up-in-led-vision). The card owns panel arrangement; MoonLight owns the picture.

That is also why this needs none of the physical detail you may have filled in elsewhere. An output page that asks for scan rate, address lines and chain order is describing panels driven *directly*, where the software has to generate the HUB75 timing itself. Through a receiving card, none of that is the sender's business: the card generates the timing, and the sender hands it an image. That holds for any sender, [FPP](https://github.com/FalconChristmas/fpp) included, which reaches these cards over Ethernet exactly as MoonLight does.

**When you need the Panels layout instead.** It exists for walls where MoonLight, not a card, owns
the ordering: addressable panels wired as one long pixel strip, where the strip snakes from panel to panel and the layout has to undo that. Its controls are about **wiring order**, which a HUB75 ribbon does not have.

| Control | Meaning |
|---|---|
| `horizontalPanels` / `verticalPanels` | How many panels across and down (1-32 each) |
| `panelWidth` / `panelHeight` | One panel's pixels (default 16x16) |
| `wiringOrder`, `X++`, `Y++`, `snake` | How pixels run *within* a panel |
| `wiringOrderP`, `X++P`, `Y++P`, `snakeP` | How the panels themselves are ordered |

**Do HUB75 panels snake?** Not in the sense these controls mean. A HUB75 panel is addressed by row
and column over the ribbon, so its internal pixel order is fixed by the panel's own driver ICs and is not something a layout re-maps. A *chain* of panels can be arranged in any order, including a serpentine one, but that is configured on the card, not here. So if you are driving panels through a receiving card and every other row looks reversed, the setting to revisit is in LEDVision.

### 5.5 Add the driver

Add a **Panel Card** driver under Drivers, and set:

| Control | Set it to |
|---|---|
| `format` | `ColorLight 5A-75` |
| `interface` | **Leave blank**: an ESP32 has one MAC, so this is ignored |
| `fps` | 40 is a good start (1–120) |
| `start` / `count` | Leave at defaults unless you are splitting one picture across several controllers |

### 5.6 What you should see

The driver's status line tells you what the wire is doing, and the three states are worth recognizing:

| Status | Meaning |
|---|---|
| `1000 Mbit - 5200 packets/s` | Working. At the default `v12 and older` the rate is (rows + 2) × fps: one packet per row plus one brightness and one sync per frame, so 128 rows at 40 fps is 5,200. On `v13 and newer` both extras go out twice, (rows + 4) × fps = 5,280. A row wider than 497 pixels splits into several packets, so a wide wall sends more than one per row. |
| `100 Mbit - …` (warning) | Sending, but see [§2](#the-one-hardware-fact-that-decides-everything): fine for a small wall, tearing on a large one. |
| `no ethernet link` | Cable, card power, or the wrong port on the card. |

If the status is healthy and the panels are dark, jump to [§7](#7-card-firmware-and-the-flicker).

---

## 6. From a desktop

A PC, a Mac or a Raspberry Pi can drive the same card. Reasons to want this: far more compute for heavy effects, and a machine you already own.

The steps are the same as [§5](#5-on-an-esp32), same Layout and same driver, with **two differences**:

- **`interface` matters.** A desktop has several NICs and the frames must leave the right one. Which spelling to use is per-OS, below.
- **Raw Ethernet needs permission.** Sending below IP is privileged on every desktop OS. Without it, MoonLight does not fail silently: the driver warns and *records* frames instead of sending them, which is also how the tests run with no hardware.

Pick your OS.

### 6.1 Windows: needs Npcap

Windows has **no** built-in way for an application to put a raw Ethernet frame on the wire. That is an OS restriction, not a MoonLight limitation, and it is why Wireshark bundles a driver and why ColorLight's own LEDVision needs one.

**Install [Npcap](https://npcap.com/)** (free; it is also installed if you already have Wireshark). Legacy WinPcap 4.1.3 also works, and is what this driver was developed and measured against; Npcap offers the same API and is the maintained choice on a new machine.

MoonLight loads it *at run time*, so the application installs and runs fine without either; you simply cannot bind an interface until one is present, and the driver says so.

For `interface`, type **any distinctive part of the adapter's name**, case-insensitive: `Realtek`, `Intel`, `Ethernet`. Windows names its capture devices `\Device\NPF_{…GUID…}`, which is neither memorable nor short enough for the field, so MoonLight matches your text against the adapter description instead. The full device name also works if you have it.

> If binding fails with Npcap installed, re-run its installer and check whether *"Restrict Npcap driver's access to Administrators only"* was selected. If so, run MoonLight as Administrator.

### 6.2 Linux, including Raspberry Pi

Raw frames go out over `AF_PACKET`, which needs `CAP_NET_RAW`.

Either run as root, or grant the capability once so it does not need root again:

```sh
sudo setcap cap_net_raw+ep ./MoonLight
```

For `interface`, use the kernel's name exactly: `eth0`, `enp3s0`. `ip link` lists them.

### 6.3 macOS

Raw frames go out over BPF (`/dev/bpf*`), which is root-only by default.

Run MoonLight with `sudo`, or install Wireshark's **ChmodBPF** helper, which grants your user access to the BPF devices at boot and is the tidier option if you do this regularly.

For `interface`, use the BSD name exactly: `en0`, `en7`. `ifconfig` lists them.

> On an Apple Silicon or Intel Mac the built-in port is usually gigabit; a USB-C dongle may not be. Check the driver's status line rather than assuming.

### 6.4 Then, on any desktop

Set up the layout and the **Panel Card** driver exactly as in [§5.4](#54-describe-the-wall) and [§5.5](#55-add-the-driver), with `interface` filled in per your OS. The status line means the same things.

---

## 7. Card firmware, and the flicker

A card's firmware has a version of its own, separate from the hardware revision printed on the board. It matters twice: once because one generation is defective, and once because MoonLight has to know which generation it is talking to.

### The v13 flicker

Cards running **firmware v13** on **v8.x hardware** flicker in time with network activity. This is a defect in the card, not in the sender: it shows up identically under MoonLight, [FPP](https://github.com/FalconChristmas/fpp) and ColorLight's own LEDVision, and nothing about how the frames are sent avoids it. The fix is to put older firmware on the card.

To be clear about what is and is not wrong: MoonLight drives a v13 card perfectly well. It binds, sends at the full frame rate, and the picture is correct. The flicker is the *only* reason to move off v13, and it is the card doing it. Since the trigger is network activity and driving a wall means constant network activity, there is no sending-side setting that avoids it. Batching, frame rate and packet count have all been tried; the defect is downstream of all of them.

Two different faults look like "flicker", and only one of them is this. Tell them apart before spending an evening on the wrong one:

| What you see | What it is |
|---|---|
| Flicker follows the Ethernet activity LED, and is there even on a still, dim image | The v13 defect. Downgrade the card. |
| Flicker grows with brightness and with how much of the wall is lit | Power. The panels draw more than the supply holds. A downgrade changes nothing. |

The second row is worth taking seriously, because MoonLight sends a full frame every tick no matter what the effect is doing. The packet rate is identical for a black wall and a busy one, so flicker that tracks *content* is not coming from the network.

### Reading and changing the version

**Use [LEDUpgrade](https://en.colorlightinside.com/product/download/383) 4.0 and firmware 11.09.**
That is the proven combination, and the easiest one, because 11.09 ships inside LEDUpgrade 4.0: it is in the preset list, so there is no firmware file to find. Treat any other pairing as a detour.

**Why not 5.0.** Version 5.0 ships no pre-v12 firmware at all, so it cannot do this downgrade from
its preset list however long you fight it.

**Why 11.09 rather than something older.** Anything before v12 clears the flicker, but older is not
automatically safer: cards on 11.08 were reported strobing white, which 11.09 fixes. 11.09 is the newest build on the safe side of the defect, so it carries the most fixes while carrying none of the flicker.

> **On Windows, if LEDUpgrade cannot find the card.** A built-in Ethernet port can be held by
> something else in the stack, and Hyper-V's virtual switch is the usual culprit: it binds the
> adapter, so a tool that needs raw layer-2 access reaches nothing even though the port looks
> ordinary and your normal networking works. The reliable way through is a **USB gigabit Ethernet
> dongle**, which Hyper-V is not bridging. This is about the card-flashing step: MoonLight's own
> sending works from a built-in port.

1. Connect the card **directly** to the machine, no switch in between.
2. **Close everything else that talks to the card**, MoonLight included. A card being streamed at will not answer, and two ColorLight tools at once (LEDVision and LEDUpgrade) interfere.
3. `Send Mode` set to the network-card mode, then choose your adapter. Restart LEDUpgrade afterwards: it binds the adapter at startup.
4. **Detect Receiver Cards.** It reports something like `5A 13.17 (v8.0)`, meaning firmware 13.17 on v8.0 hardware.
5. **Readback Firmware** to back up what is on the card before you replace it.
6. `Upgrade Firmware`, preset, `4in1`, `normal`, then **`normal-11.09`**. Stay in the `normal` series: `PWM` and `shixin` are for different panel driver ICs.
7. **Power-cycle the card.** It goes on running the old firmware until you do, which is the step most often missed.

### Then tell MoonLight what it is talking to

Set the driver's `firmware` control to match the card:

| Setting | For |
|---|---|
| `v12 and older` | **The default**, and a downgraded card. Brightness and sync go out once. |
| `v13 and newer` | A stock card. Both go out twice, which is the copy this firmware acts on. |

The default is the downgraded generation on purpose: this page's own advice is to move a v13 card off it, so the setting is already right when you finish rather than being one last unexplained step.

The mismatch is not subtle in one direction: leave a downgraded card on `v13 and newer` and it receives a second sync, treats it as another latch, aborts the refresh already running, and the wall updates once every few seconds.

---

## 8. When it does not light up

| Symptom | Look at |
|---|---|
| `no ethernet link` | On a desktop, check `interface` first: a string that matches no adapter fails the bind and reports exactly this, with nothing to distinguish it from an unplugged cable. Then cable seated, card powered, and plugged into the card's **input** port. On a 100 Mbit controller, try a gigabit switch in between. |
| Status healthy, panels dark | The Layout, not the driver. If the wall is 0 lights, or the driver's window (`start`/`count`) selects none, there is nothing to send. |
| Image tears or rolls | Link speed ([§2](#the-one-hardware-fact-that-decides-everything)). Check the status line says 1000 Mbit. |
| A new frame only every few seconds | `firmware` ([§7](#7-card-firmware-and-the-flicker)) is set to `v13 and newer` on a card running v12 or older. The duplicate sync latches twice and aborts the refresh. |
| Flicker in time with network activity | The card's own v13 firmware defect ([§7](#7-card-firmware-and-the-flicker)), not the sender. Downgrade the card. |
| Every other row or column mirrored | With a receiving card, this is the card's own panel configuration: revisit it in LEDVision ([§4](#4-set-the-panels-up-in-led-vision)). The layout's `snake` / `snakeP` apply only to walls wired as a pixel strip. |
| Panels in the wrong places | Same: the chain order is set on the card. |
| Right image, wrong colors | `lightPreset` on the driver, which is where channel order and RGBW synthesis live for every driver. There is no separate color-order control here. |
| Works on ESP32, not on desktop | Permission ([§6](#6-from-a-desktop)). The driver shows a warning that says so. |
| **PanelCard is not in the Drivers list** | The driver is compiled only into firmwares for boards with Ethernet (S3, P4, S31), so a classic-ESP32 build does not offer it. Only the S31 is gigabit; the S3 and P4 are 100 Mbit. Check the **Firmware** page for the variant and version you are actually running: an OTA upgrade keeps the installed variant, so a device first flashed with a different one keeps that one. Re-flash from the [web installer](https://moonmodules.org/projectMM/install/) to change variant. |
| **Link is up and packets are on the wire, but the wall stays dark** | The frames are leaving correctly, so the fault is downstream of the send. Check, in order: global **brightness** *and* the driver's own `localBrightness`, both of which must be up; a **layout** actually configured and an **effect** selected, since an empty layer sends valid black frames; and `format` / `firmware` matching the card ([§7](#7-card-firmware-and-the-flicker)). If all of that is right, remove the PanelCard driver and add it again: a re-add re-runs the bind and the geometry from scratch. |

---

## Where to go next

- **[Setting up a Colorlight Card with FPP v6.3 and LED Vision 8.5](https://www.youtube.com/watch?v=L4lHbwUszAs)**: a video walkthrough of the card and panel side. A different sender, the same cards and the same LEDVision work.
- **[Drivers](../moonmodules/light/drivers.md#panelcard)**: the Panel Card control reference.
- **[Layouts](../moonmodules/light/layouts.md#panels)**: the Panels layout in full.
- **[Effects](../moonmodules/light/effects.md)** and **[live scripting](../moonmodules/light/moonlive.md)**: a wall is a big canvas, and scripted effects are the fastest way to fill it.
