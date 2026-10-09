# Getting started

New to ESP32 or flashing firmware? You don't need to be. MoonLight installs straight from your web browser: no software to download, no command line. In a few minutes you'll have lights running and the device on your network, and the device's own web interface open in your browser ready to play with.

**Chapter 1** gets MoonLight onto your device.
**Chapter 2** is a tour of the interface you land in afterwards, so you know what every part does and where to start building your own light show.

**You need:** an ESP32 board, a USB cable that carries data (not charge-only), and a **Chromium-based browser** on a computer: Google Chrome, Microsoft Edge or Opera.
The installer uses the Web Serial API, which Safari and Firefox don't support.

> Want the bigger picture of what MoonLight is first? See the [project overview](../README.md).

MoonLight is provided as is, without warranty: like any software it has bugs, and you use it at your own risk.

---

## Chapter 1: install MoonLight

### 1. Open the installer and plug in

Open the **[web installer](https://moonmodules.org/MoonLight/install/)** in Chrome or Edge, then plug your ESP32 into a USB port.

![The web installer](assets/gettingstarted/01-01-installer-start.png)

<video src="assets/moontube/01-install-esp32.webm" autoplay loop muted controls playsinline width="720" title="The installer: picking a release, a firmware, and a board from the gallery"></video>

### 2. Pick the USB port

Click **USB Port → Pick a port…**. Your browser shows a small list of connected devices: choose the one that appeared when you plugged in the ESP32. (Not sure which? Unplug, look at the list, plug back in: the new entry is your device.)

![Selecting the USB port](assets/gettingstarted/01-02-select-port.png)

**Windows users, dialog says "No serial ports available"?** Windows doesn't ship drivers for the USB-serial chips most ESP32 boards use (WCH CH340, Silicon Labs CP2102).
A one-time install fixes it for every future flash: the steps and the download link are in [building.md § Windows: USB-serial drivers](how-to/building.md#windows-usb-serial-drivers).
macOS and Linux ship these drivers built in.

**Mac users, no ESP32 in the list?** A recent Mac asks whether a new USB accessory may connect, and a board that was refused stays blocked.
In System Settings, Privacy & Security, set "Allow accessories to connect" to ask for new accessories, then plug the board in again and allow it.

Once a port is chosen, the installer recognizes the chip and tells you how many devices match it, so you know you're on the right track.

![Port selected, chip detected](assets/gettingstarted/01-03-port-selected.png)

### 3. Pick your device

Choose your device from the **Device** picker. Each card shows a picture, the chip, and what the device can do (LEDs, WiFi, a button, a microphone…).
Click **details** on any card to see exactly what it is and a link to its product page.

![Picking a device](assets/gettingstarted/01-04-pick-device.png)

![A device card with its details](assets/gettingstarted/01-05-device-details.png)

The little colored pills are the device's capabilities, and the color tells you how ready each one is:

- 🟢 **Green**, set up and working the moment you install. This capability is supported *and* already wired into the device's configuration.
- 🟡 **Yellow**, the firmware supports it, but it isn't pre-configured. It works once you add and set up the matching module yourself in the UI (Chapter 2).
- 🟠 **Amber**, planned. The hardware has it, but there's no module for it yet: it's on the to-do list. Want to help? Building one is our usual loop: read the product page and datasheet, pin the behavior as tests, then write the code to pass them ([see how we work](../CLAUDE.md#principles)).

So a green pill works out of the box, a yellow one works with a bit of setup, and an amber one is coming later.

The setup panel then shows how your device is configured out of the box, the modules and settings applied automatically when you install.

![Device setup](assets/gettingstarted/01-06-device-setup.png)

Nothing is locked in: once the device is running you can change any of it later in the UI (that's what Chapter 2 is all about).

Leave **Release** and **Firmware** at their suggested values: the newest stable build, and the firmware that matches your device.
Tick **Erase chip first** only to start clean or to switch firmware.
It is also needed when a 4 MB classic board (esp32 / wrover / eth) runs a release before v4.0 and you update it.
That last update must erase, since its partition layout changed ([MIGRATING](reference/MIGRATING.md)).
If the device already holds config you care about, back it up first ("Back up a device's config first" on the installer page).
Erasing wipes WiFi credentials and all settings, and the backup brings them back after the flash; its report lists anything it could not carry.

### 4. Click Install

The installer erases (if you asked it to) and writes the firmware. It takes under a minute.

![Erasing](assets/gettingstarted/01-07-erasing.png)
![Installing](assets/gettingstarted/01-08-installing.png)

### 5. Get it on your network

What happens next depends on your device:

- **WiFi:** enter your network name and password when prompted, then **Connect**.
  Or click **Skip** and set WiFi up from the device itself. It opens its own `MM-XXXX` network, and joining that with a phone or laptop opens a sign-in screen on its WiFi card. Pick your network there.
  Restoring a config backup? Skip this step too. Join the `MM-XXXX` network: the device's UI opens by itself as a sign-in screen, or at `http://4.3.2.1`. Restore the backup in the File Manager (⟲), and take the offered restart. The bundle carries the WiFi credentials, so the device joins your network by itself.

  ![Entering WiFi credentials](assets/gettingstarted/01-09-wifi-credentials.png)

- **Ethernet:** plug in the cable: it connects on its own, no password needed.

### 6. Open your device

When it's online, the installer shows a link: your device's address on your network. Click it.

![Device is online over WiFi](assets/gettingstarted/01-10-online-wifi.png)

You'll see this same "Device is online!" box however your device connected: over Ethernet, or when it rejoins a network it already knows.

![Online over Ethernet](assets/gettingstarted/01-11-online-ethernet.png)
![Online on an address it already had](assets/gettingstarted/01-12-online-existing-ip.png)

That's it: MoonLight is installed and on your network. The link opens the device's own web interface, served straight from the ESP32. Let's look around.

---

## Chapter 2: Your MoonLight interface

Everything below runs **in your browser, live from the device**. There's no app, no account and no cloud: the ESP32 itself serves the interface, and every change you make takes effect on the lights immediately.
Open the link from step 6 and follow along; you can't break anything by exploring.

Here is what a device adds to the interface, on a board that was flashed a moment earlier:

<video src="assets/moontube/02-first-look-esp32.webm" autoplay loop muted controls playsinline width="720" title="Each part of a freshly flashed board and what a device adds: an LED driver on a pin, the I2C bus, the pins, a microphone, and updates over the air"></video>

### The layout: list, preview, controls

![The full MoonLight interface](assets/gettingstarted/02-01-UI-large.png)

Three regions, left to right:

- **The module list** (left): every part of your device, from system info at the top to your light setup at the bottom. Click a name to jump to it.
- **The 3D preview** (center): a live picture of your lights in their real shape, updating as the effects run. This is what your physical LEDs are doing, right now.
- **The controls** (right): the settings for each module. Drag a slider or pick an option and the lights react instantly.

Every module header carries a **⏻ power button**, which turns that module on or off. Bright (accent-colored) means on; dimmed means off.
A switched-off module stops running and stays in place with all its settings, so flicking it back on picks up right where it left off. It's the quick way to mute an effect or an output for a moment without deleting anything.

You'll also spot two little read-outs in each header. **🕒** is how fast that module runs: its loop speed, which a click flips between fps and microseconds. **🧠** is how much memory it uses. They let you see at a glance what each part is costing.

The interface adapts to your window. On a narrower screen the controls take the full width and the preview tucks into a floating thumbnail you can move around:

![Medium width, preview as a floating thumbnail](assets/gettingstarted/02-02-UI-mid.png)

Narrower still, it stacks into a single scrollable column, so it works on a phone, standing next to your lights:

![Small width, single column](assets/gettingstarted/02-03-UI-small.png)

### The 3D preview

![The 3D preview, lights numbered](assets/gettingstarted/02-04-UI-Preview.png)

Drag to rotate, scroll to zoom. Each dot is one light at its real position, lit with the color it's showing this instant. Turn on the numbers to see each light's index, handy when you're wiring or mapping a layout.
The preview is a *view* of the device: it never slows the lights down, and on a slow connection it eases off (fewer updates, then fewer points) rather than stalling.

> More on how the preview streams from the device: [PreviewDriver](moonmodules/light/moxygen/PreviewDriver.md).

### Every card works the same way

Learning one card teaches you all of them. Each carries the same five buttons in its corner, and the mode selector decides how many controls you see at all.

<video src="assets/moontube/03-second-look.webm" autoplay loop muted controls playsinline width="720" title="The five buttons on every card, each pressed: power, replace, delete, help and the card's own JSON"></video>

⏻ turns a module off, ✎ swaps it for another type, × deletes it, and ? opens that module's page in the documentation. { } opens the card's own JSON, which is what to paste into an issue when something misbehaves.

### The system modules

The top of the list is your device's "about" section: read-outs and connection settings. You rarely need to touch these, but they're the first place to look if something seems off.

**System**: who this device is and how it's doing: its name, the device model, uptime, frame rate, and live memory / storage bars.
You may also see an **Audio** module here. Devices with a built-in mic come with it set up, and on any device you can add it yourself. It's how audio-reactive effects hear the music.
Audio is the first of many: any sensor or input, from hardware or over the network, lives here as its own module, and we're adding more all the time.

![The System module](assets/gettingstarted/02-05-UI-System.png)

> [SystemModule](moonmodules/core/system.md#system) · [Audio](moonmodules/core/services.md#audio)

**Firmware**: which build you're running, and where you update it. The **Install** button here does an over-the-air update straight from the device, no USB cable needed once it's on your network.

![The Firmware module](assets/gettingstarted/02-06-UI-Firmware.png)

**Updating from an older build?** Skim the [migration notes](reference/MIGRATING.md) first. Most updates need nothing, since the device keeps your settings.
A breaking change is listed there with the one action it costs you, usually re-setting or re-adding a control.

> [FirmwareUpdateModule](moonmodules/core/system.md#firmware-update)

**Network**: your connection, WiFi or Ethernet, its signal strength, and the address others reach it at. The **Devices** section underneath finds other MoonLight devices on the same network, so a roomful of them can discover each other.

![The Network module](assets/gettingstarted/02-07-UI-Network.png)

> [NetworkModule](moonmodules/core/system.md#network) · [DevicesModule](moonmodules/core/system.md#devices)

> **Lights are one use among many.** Everything above (the modules, the live controls, the 3D view, the web UI, the networking) is a general-purpose engine that knows nothing about LEDs.
> The light show below is one *domain* built on top of it; you could build a different one and reuse all the same machinery.
> [FastLED-MM](https://github.com/MoonModules/FastLED-MM) is an example, driving its LEDs with [FastLED](https://github.com/FastLED/FastLED) (on hold until MoonLight ships as a reusable library).

### Control it from your phone with WLED Native

The device's own web UI works on a phone. For quick on/off and brightness from your pocket there's a nicer option: **WLED Native**, the open-source mobile app for the WLED ecosystem.
MoonLight speaks the WLED JSON API and announces itself on the network as a WLED device does. So the app finds your MoonLight controllers automatically, with no setup and no pairing.
Each one shows up as a card with a power toggle and a brightness slider, so a roomful of controllers is a scroll and a tap away.

![MoonLight devices discovered in WLED Native](assets/core/WLED%20Native%20discovers%20MoonLight.jpeg){ width="300" }

Get it free for your phone:

- **iPhone / iPad:** [WLED Native on the App Store](https://apps.apple.com/us/app/wled-native/id6446207239)
- **Android:** [WLED Native on Google Play](https://play.google.com/store/apps/details?id=ca.cgagnier.wlednativeandroid)

WLED Native is by **Christophe Gagnier ([@Moustachauve](https://github.com/Moustachauve))**, who wrote both the [Android](https://github.com/Moustachauve/WLED-Android) and [iOS](https://github.com/Moustachauve/WLED-iOS) apps.
Their open source is what let us work out exactly what those apps read, so a MoonLight device appears in them without either side needing to know about the other.

For the full picture and controls, the device's web interface is always there at `http://<devicename>.local`; WLED Native is the fast everyday remote alongside it.

### Bring it into your smart home with Home Assistant

Want your lights in the same dashboard as the rest of your house, and in automations, voice assistants and Apple Home? MoonLight adopts into **Home Assistant** like any other light.
Point the device at your HA setup and it appears as a light entity with on/off and brightness, alongside a floor of other devices.

![MoonLight devices as lights in a Home Assistant dashboard](assets/core/ha-integration.png){ width="600" }

There are two ways in: zeroconf, where HA finds the device on its own, or MQTT auto-discovery, for a broker-only or cross-subnet setup. From there you can bridge the entity into Apple Home too.
The step-by-step, including installing HA and the MQTT broker if you don't have them, is in the [home automation guide](how-to/home-automation.md).

### Building a light show: layouts → layers → drivers

The bottom three modules are where the fun is. They form a simple pipeline: a **layout** says where your lights are, **layers** decide what colors play on them, and **drivers** send the result out to the real world. Add modules with the dashed **+ add module** button under each one.

**Layouts**: the shape of your lights. The default **Grid** is a width × height (× depth) of pixels; change the numbers and the preview reshapes instantly. Turn on **serpentine** if your strip zig-zags back and forth.

![The Layouts module](assets/gettingstarted/02-08-UI-Layouts.png)

> [Layouts](moonmodules/light/supporting.md)

**Effects**: what plays on the lights. Add an **effect** (a moving pattern), stack several to blend them, and reshape them with **modifiers** (mirror, rotate, and more). Each effect has its own controls, such as speed and color mode, which you tweak live.

![The Effects module](assets/gettingstarted/02-09-UI-Layers.png)

> [Effects](moonmodules/light/supporting.md) · [Layer](moonmodules/light/supporting.md)

**Drivers**: where the colors go. Set overall **brightness** and color order, then add an output. That's real LED strips on a pin, or the frame sent over the network (ArtNet, E1.31/sACN, DDP) to other devices or lighting software.

![The Drivers module](assets/gettingstarted/02-10-UI-Drivers.png)

> [Drivers](moonmodules/light/supporting.md) · [NetworkSendDriver](moonmodules/light/moxygen/NetworkSendDriver.md)

That's the whole picture: **layout → layers → drivers**, previewed in 3D, all tuned live in your browser. Pick an effect, drag a slider, watch the lights, then keep going.

---

### If your device shows MoonBase

**MoonBase** is a small recovery image built into your device. If a firmware update is interrupted, or an installed firmware does not start, your device boots MoonBase instead of going dark, and its page offers you three ways out:

- **Boot the app** puts you straight back if the firmware is still fine. Try this first: it changes nothing on the device.
- **From a file** installs a firmware you have already downloaded. Get the `firmware-...bin` matching your device from the [releases page](https://github.com/MoonModules/MoonLight/releases).
- **From a URL** downloads and installs in one step. The releases page gives you a link to each file; paste it here and your device fetches it directly.

Installing takes a few minutes, and the page reports its progress as it downloads. Your device reboots into the new firmware on its own when it finishes.

Two things worth knowing. A failed install **stays** in MoonBase rather than pretending to have worked, so you can try again.
And you cannot break a device this way: an update never overwrites MoonBase. It is still there for the next attempt, including after a power cut in the middle of one.

Off your network, MoonBase opens the device's own `MM-XXXX` access point. Joining it opens MoonBase's page by itself as a sign-in screen, or at **4.3.2.1** there. Once it knows your WiFi, the access point carries your WiFi's password.

MoonBase shows its own version on its page, and your device's Firmware card shows which MoonBase it carries. When it does not match the app, the card warns, and its MoonBase tab installs the matching one over the network, so keeping the recovery image current needs no cable.

That update runs from the app, because only the running app can write the partition MoonBase lives in. So it is a way to keep MoonBase fresh, not a way back from a device that will not start. If the app cannot run, or MoonBase itself will not boot, that still takes a cable.
Your device checks the image first, refusing anything whose magic bytes, chip or description say it is not a MoonBase image for this chip.

---

### The same thing on your computer

MoonLight runs on macOS, Windows and Linux as well, with no board attached. The installer offers it from the same page: pick **This computer** instead of a USB port, and the Install button becomes a Download.

<video src="assets/moontube/01-install-desktop.webm" autoplay loop muted controls playsinline width="720" title="The installer pointed at this computer: picking a release, then downloading the app"></video>

What you get is the same interface, the same modules and the same effects. A computer has no LED pins, so it previews the frame and sends it over the network instead of driving a strip. It also has the memory for a far larger grid than a board does.

<video src="assets/moontube/02-first-look-desktop.webm" autoplay loop muted controls playsinline width="720" title="The tour on a computer: every module and the tabs inside it, on a 256 by 256 grid"></video>

It's useful for trying an idea before you wire anything, or for developing effects without a flash cycle. A machine with real processing power can also drive a board over the network.

### Where to go next

- **Understand the pipeline**, how layouts, layers, effects, modifiers and drivers fit together: [architecture overview](explanation/architecture/moonlight.md#the-pipeline).
- **Manage several devices, build, and flash from one console** with MoonDeck, our developer tool: [MoonDeck guide](../moondeck/MoonDeck.md).
- **Build from source** or target Teensy / Raspberry Pi: [building.md](how-to/building.md).

Stuck, or something didn't work? Open an [issue](https://github.com/MoonModules/MoonLight/issues), and tell us what device you used and where it stopped.
