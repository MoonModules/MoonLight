# Troubleshooting

Start from the symptom. Each entry says what to check first and where the detailed page is, because the fastest fix is usually eliminating the innocent half rather than guessing at the guilty one.

## The lights are dark

**Check the preview first.** If the 3D preview shows the effect running, the pipeline is fine and the problem is downstream: the driver, the wiring, or the power. If the preview is also dark, it is upstream: the layout, the effect, or the brightness.

Then, in order:

- **Brightness** on the Drivers card. Zero is dark and looks identical to broken.
- **Is a driver added at all?** A fresh device previews without one. Real lights need a driver naming the pin or the destination.
- **Does the light count match?** A layout of 256 and a driver of 16 lights the first sixteen and nothing else.
- **Module status.** A card with a problem says so on itself. A red line names the failure.

A scripted effect that fails to compile renders dark deliberately and shows the parse error on its card: see [when the compile fails](../tutorials/first-script.md#when-the-compile-fails).

## Stray pixels, or wrong colors on lights that should be off

Almost always electrical rather than firmware, on a 3.3 V board driving 5 V WS2812 directly.

The diagnosis path, which eliminates the firmware before anyone reaches for a soldering iron, is [LED signal integrity](led-signal-integrity.md).

## Colors are wrong everywhere

Red where you expect green usually means the channel order does not match the strip. Set it on the driver: the [fixture profile](../moonmodules/light/supporting.md#fixtureprofiles) names which channel carries which color.

Whole-panel color shifts on a receiving card are a different thing, covered in [panel cards](panel-cards.md).

## Every other row is backwards

The strip zig-zags and the layout does not know. Turn on **serpentine** on the Grid layout.

## The device is missing from the network

- **It never joined.** An unprovisioned device opens its own access point named `MM-XXXX`. Join it: the WiFi card opens by itself as a sign-in screen, or at `http://4.3.2.1`, to set the credentials.
- **It lost its network and its access point stays closed** (`opens` is `first setup only`). Switch it on and off four times, each within a few seconds: its access point opens for that boot without a password, and every setting stays. Join it and set the new network on the WiFi card. To start again from scratch, **factory reset** on the System card deletes every setting.
- **It joined but you cannot find it.** Try the IP from your router's client list before the `.local` name: mDNS fails on plenty of networks that route fine. A device with no mDNS responder is reachable by address all along.
- **It was working and stopped.** Check the device is powered and the access point is not sitting between two networks. A repeater on one radio halves its throughput and drops the client side under load.

## The interface is slow, or the connection indicator flickers

The preview streams a full frame per update, and a large grid over a marginal link is the usual cause. Turn the preview off and see whether the interface recovers.

The device drops preview frames rather than blocking the render loop, so this costs smoothness rather than correctness.

## An update failed

The device stays in [MoonBase](updating-firmware.md#when-the-device-boots-into-moonbase) rather than pretending to have worked, and its page offers you a retry. You cannot brick a device this way: MoonBase is never overwritten by an app update.

## The device rebooted on its own

Check the power first. A board browning out under load looks exactly like a software crash, and a supply that cannot hold current while lights draw is the more common cause.

After two crashes or brownouts in a row the device starts in safe mode: its LEDs stay dark, the preview runs, and the System card says why. Lower the brightness, or undo the last setting or script, then restart the device from the top bar.

If the power is solid, the crash log is on the device: the System card shows the last reset reason, and [logging an issue](logging-an-issue.md) says what to collect.

## Settings vanished after a reboot

Config persists a couple of seconds after the last change, so a power cut within that window loses the last edit and nothing else.

If more than that vanished, the filesystem may have been erased by an install with **Erase chip first** ticked. A [backup](backup-and-restore.md) restores it.

## When none of this helps

[Log an issue](logging-an-issue.md). You do not need to diagnose it; the page says what to include so somebody else can.
