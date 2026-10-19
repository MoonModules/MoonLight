# Troubleshooting

Start from the symptom. Each entry says what to check first and where the detailed page is, because the fastest fix is usually eliminating the innocent half rather than guessing at the guilty one.

## A first run on a computer or in a container

| Symptom | Look at |
|---|---|
| The download is flagged and will not open | Keeping it, on Windows: the choice hides behind the **`⋯`** and the **Delete** dropdown. See [Windows flags the download](#windows-flags-the-download). |
| A blue **"Windows protected your PC"** appears when you run it | The file reached you without the download prompt. Click **More info**, then **Run anyway**. |
| It opened and closed immediately | Run it from a terminal so the error stays on screen. |
| The browser shows nothing at `localhost:8080` | Check the console window is still open: closing it stops MoonLight. If another program uses port 8080, start with `--port 8081`. |
| Another machine cannot reach it | Use the `HTTP server ->` address from the log, not `localhost`. Windows Firewall prompts on first run; allow it on your private network. |
| Settings do not survive a restart | The log says `cannot use ... persistence disabled` and names the directory it tried. |
| The installer fails saying a file is in use | A copy of MoonLight is running that it could not stop. Close its console window and run the setup again. |
| The container exits at once | [When the container exits at once](installing-on-linux.md#when-the-container-exits-at-once): usually an overlay root filesystem. |

## Windows flags the download

Microsoft Defender SmartScreen judges a download by its **reputation**, built from how many people downloaded that exact file from a publisher it recognizes. MoonLight is not code-signed and every build is a new file, so its reputation is always zero and **every new build** gets the warning. It is about the certificate, not the contents; code signing is on the backlog.

Occasionally Defender removes the file outright, naming something like `Trojan:Win32/Wacatac.C!ml`. The `!ml` suffix means a **machine-learning guess**, not a match against known malware: an unsigned, zero-reputation executable that opens audio capture devices fits a pattern the model weighs. The contents are checkable rather than a matter of trust. The binary is compiled from this repository's source in public CI, and the one vendored file behind the audio support (`miniaudio.h`) is byte-identical to its upstream release.

- **The download fails** with *"Couldn't download - Download error"*: Defender stops it mid-transfer and there is no file to rescue. [Install from the zip](../moonwalk.md#install-windows-zip), a different file with a different fingerprint.
- **The file landed and was quarantined**: delete it in **Windows Security → Virus & threat protection → Protection history**, then download again from the [releases page](https://github.com/MoonModules/MoonLight/releases). Restoring it there works too, for a file you downloaded yourself moments before.

Either way the durable route is reporting it as a false positive at [microsoft.com/wdsi/filesubmission](https://www.microsoft.com/wdsi/filesubmission). Microsoft typically clears these within days, for everyone.

## The lights are dark

Work through [Diagnose dark or flickering lights](../moonwalk.md#diagnose-lights): the preview first, since it says which half to look at, then the brightness, the driver, the light count and each card's status.

## Stray pixels, or wrong colors on lights that should be off

Almost always electrical rather than firmware, on a 3.3 V board driving 5 V WS2812 directly.

The diagnosis path, which eliminates the firmware before anyone reaches for a soldering iron, is [LED signal integrity](../explanation/led-signal-integrity.md).

## Colors are wrong everywhere

Red where you expect green usually means the channel order does not match the strip. Set it on the driver: the [fixture profile](../moonmodules/light/supporting.md#fixtureprofiles) names which channel carries which color.

Whole-panel color shifts on a receiving card are a different thing, covered in [panel cards](hardware/panel-cards.md).

## Every other row is backwards

The strip zig-zags and the layout does not know. Turn on **serpentine** on the Grid layout.

## The device is missing from the network

- **It never joined.** An unprovisioned device opens its own access point named `MM-XXXX`: [join your WiFi through it](../moonwalk.md#join-access-point).
- **It lost its network and its access point stays closed** (`opens` is `first setup only`). Switch it on and off four times, each within a few seconds: its access point opens for that boot without a password, and every setting stays. Join it and set the new network on the WiFi card. To start again from scratch, **factory reset** on the System card deletes every setting.
- **It joined but you cannot find it.** Try the IP from your router's client list before the `.local` name: mDNS fails on plenty of networks that route fine. A device with no mDNS responder is reachable by address all along.
- **It was working and stopped.** Check the device is powered and the access point is not sitting between two networks. A repeater on one radio halves its throughput and drops the client side under load.

## The interface is slow, or the connection indicator flickers

The preview streams a full frame per update, and a large grid over a marginal link is the usual cause. Turn the preview off and see whether the interface recovers.

The device drops preview frames rather than blocking the render loop, so this costs smoothness rather than correctness.

## An update failed

The device stays in [MoonBase](../moonwalk.md#update-firmware) rather than pretending to have worked, and its page offers you a retry. You cannot brick a device this way: MoonBase is never overwritten by an app update.

## The device rebooted on its own

Check the power first. A board browning out under load looks exactly like a software crash, and a supply that cannot hold current while lights draw is the more common cause.

A failed boot is a crash, a brownout, or a supply that cuts out within a minute of starting. After two in a row the device starts in safe mode: its LEDs stay dark, the preview runs, and the System card says why. Lower the brightness, or undo the last setting or script, then restart the device from the top bar.
Unplugging a device twice within a minute of starting counts the same way, so leave it on for a minute and restart it.

If the power is solid, the crash log is on the device: the System card shows the last reset reason, and [logging an issue](logging-an-issue.md) says what to collect.

## Settings vanished after a reboot

Config persists a couple of seconds after the last change, so a power cut within that window loses the last edit and nothing else.

If more than that vanished, the filesystem may have been erased by an install with **Erase chip first** ticked. A [backup](backup-and-restore.md) restores it.

## When none of this helps

[Log an issue](logging-an-issue.md). You do not need to diagnose it; the page says what to include so somebody else can.
