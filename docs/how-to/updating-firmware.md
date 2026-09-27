# Update the firmware

Install a newer MoonLight on a device that is already running one. Over the network, from the device's own interface, with no cable.

Your settings survive: a firmware update replaces the program, not the configuration. The one thing it costs is a reboot, which is what makes it different from every other change in MoonLight.

## The normal route

Open the device's **Firmware** card under System.

It shows the version it runs, the build it came from, and which partition it lives in. Pick the image and start the install. The card reports progress as it downloads and writes, then the device reboots into the new firmware on its own.

Where to get an image:

- **`latest`** is the rolling build from `main`: every merged change, published continuously. What to use if you want the newest fixes and can live with the occasional rough edge.
- **A tagged release** is a version somebody decided was worth naming. Slower moving, and what to use on a rig that has to keep working.

Both are on the [releases page](https://github.com/MoonModules/projectMM/releases), one file per firmware variant. The filename names the variant, and the variant must match your device: an `esp32s3-n16r8` image on a classic ESP32 refuses to boot.

The card checks for both channels and tells you when one is newer, and a stable release always wins. A device already on a `-dev` build is the only one offered the moving channel, so a stable device is never nudged toward an unreleased build.

## On a device that carries MoonBase

Some variants carry [MoonBase](../explanation/architecture/moonbase.md), a small recovery image in the factory slot. The 4 MB classic, `esp32-16mb` and the S3-Zero use it today.

The card looks slightly different there. It gains an **image** selector, choosing whether the version shown and the install performed apply to the app or to MoonBase itself, and a **Restart in MoonBase** button.

The install runs the same way from your side, behind one "updating firmware" overlay. Underneath, the device stages the URL, reboots into MoonBase, lets MoonBase write the app slot, and reboots back. A board cannot rewrite the partition it is executing from, so the two images install each other.

What that buys you is the failure case. A power cut in the middle leaves the device in MoonBase rather than holding half an app, and MoonBase is a working page you can retry from over the network. A failed install stays there visibly instead of pretending to have worked.

## Updating MoonBase itself

The same card installs a newer MoonBase, writing the factory slot while the app runs. If the card marks the carried version outdated, that is the fix, and it needs no cable.

This is the one write with a window where the device holds no recovery image. The app keeps running throughout, so the answer to a failure is to try again.

## When the device boots into MoonBase

It means the app did not start, or an update was interrupted. MoonBase serves its own page and offers three ways out:

- **Boot the app**, which changes nothing and is worth trying first: it boots only an image that validates.
- **From a file**, installing a `firmware-....bin` you already downloaded.
- **From a URL**, fetching and installing in one step. Keep it under 255 characters: it crosses into MoonBase through a fixed-size slot, and a longer one is refused rather than truncated.

If the device is not on your network, MoonBase opens its own access point and answers at **4.3.2.1** once you join it.

## Before a risky update

Take a backup. The Firmware card does not touch your configuration, but a variant change or an erase does, and [restoring](backup-and-restore.md) takes a minute where reconfiguring a rig takes an evening.

## Serial, when the network cannot help

A device that will not boot far enough to serve a page needs the [web installer](https://moonmodules.org/projectMM/install/) and a USB cable. That is the same path as a first install, and it is covered in [Install & first light](../gettingstarted.md).
