# Migrating a device to MoonLight over the air

A device running another firmware moves to MoonLight over WiFi, through that firmware's own update page, with no cable and no opening the case.
It keeps its WiFi network, and usually its address.

The file you upload is the migration image, a small program that runs once.
It writes MoonBase (MoonLight's installer) and MoonLight's partition table and restarts into MoonBase, all in about ten seconds, and from WLED it carries the WiFi network over.
MoonBase then installs MoonLight from the release.

## What can move

| From | Chip | Status |
|---|---|---|
| WLED | ESP32-C3 with 4 MB of flash | verified on WLED 0.15.0, other versions should work; the WiFi comes along |
| Another firmware, such as ESPHome or Tasmota | ESP32-C3 with 4 MB of flash | untested. It needs the old firmware to accept the upload, and the image to land clear of where MoonBase goes or find a second app slot to move into. MoonBase starts on its access point |

A migration image is built for one chip, so the chip decides which file fits.

## Steps, with WLED as the example

1. Check the chip: in WLED it is on the **Info** page.
2. Download the migration image for that chip from [the latest MoonLight release](https://github.com/MoonModules/MoonLight/releases): `shared-migrate-esp32c3.bin` for an ESP32-C3.
3. Upload it through the old firmware's update page.
   In WLED that is **Config → Security & Updates → Manual OTA Update**: choose the file and press **Update**.
   Keep the power on for the next ten seconds.
4. The device restarts as **MoonBase**, on the same WiFi.
   Open its address in a browser.
   If it is not on your network, it could not carry the WiFi over and opens an access point named `MM-…`: join it and set your WiFi there.
5. In MoonBase, pick the firmware of the release for the chip and press **Install**.
   MoonLight starts on the same WiFi.
6. Set the light up as on any device: the drivers, the layout, the effects.
   A bulb with five PWM channels, such as the Athom 12W, is the catalog's **ESP32-C3 RGBWW PWM bulb**.
   It takes the **PWM Light** driver with its five pins, the `RGBCCT` fixture, and a 1×1 grid.
   The device catalog, [`deviceModels.json`](https://github.com/MoonModules/MoonLight/blob/main/mooninstaller/deviceModels.json), holds the settings of every board MoonLight knows.

## What can stop it

Before it writes anything, the migration image checks that it can finish: enough flash, no flash encryption or secure boot, room to move itself out of the way.
When a check fails, it restarts the old firmware with nothing changed.
So a device that cannot move keeps working as it was:

- The old firmware refuses the upload. A newer WLED checks an upload's release name until its option to skip that check is ticked, and an update password or a locked update refuses it too.
- The migration image refuses the board, and the old firmware comes back.
- The WiFi is not where the image looks, and MoonBase opens its access point.

One moment needs care: for about 50 milliseconds in the middle, the image rewrites the partition table.
A power cut in exactly that window leaves the device needing a USB cable to recover, which on a bulb means opening it.
A power cut at any other moment comes back on its own: the image starts over, or MoonBase takes over.

Going back to the old firmware also needs a USB cable, since MoonBase installs MoonLight only.
