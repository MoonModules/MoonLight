# Back up and restore a device

Every setting a device holds, saved to one file on your computer, and put back on the same device or a different one.

Worth doing before a firmware update, before rewiring, and once a rig you care about is working the way you want.

## Take a backup

Open the **File Manager** panel and press **Backup (⤓)**.

The browser downloads one `.json` bundle named for the device and the date. It walks the whole filesystem, so it holds the configuration of every module, your saved [presets](presets.md), and your MoonLive scripts.

**Keep the file private. It contains the WiFi password.**

Every file is byte-verified against the directory listing as it is read, with three outcomes. A file shorter than listed aborts the backup, because silent truncation is worse than no backup at all. A file longer than listed is binary rather than text, so it is skipped and named. A file that cannot be read is skipped and named too.

One thing is left out on purpose: `/.hls`, the streaming encoder's scratch output, which is rewritten every second and would fill the report with skipped binaries.

## Put it back

Press **Restore (⟲)**, choose the bundle, and confirm twice. The second press is deliberate: restoring overwrites the device's files.

Then read the report. It lists what needed an eye:

- **Renamed and mapped**: a file, module type or control that changed name since the backup was taken, and what it became.
- **Values to review**: something the mapping could not decide on its own, usually because it depends on the chip or the wiring.
- **No longer present**: a module type or control this firmware does not have.

Most of a restore applies live, module by module, as each file lands. Two things do not, and the dialog names both:

- **Network settings** apply at boot, because bringing an interface up is not a re-runnable operation. The dialog offers the restart, after which the device joins the network the backup names. Its file is written **last** for that reason: joining another network mid-restore would cut off the writes still to come.
- **The web server's port**, which binds at boot.

## Why the browser does the work

The device never runs migration code. Its loader is robust by design: an absent key keeps the control's default, a stale value clamps to the new bounds, and an unknown key is ignored.

So the rename maps live in the browser, in [migrate.js](https://github.com/MoonModules/MoonLight/blob/main/src/ui/migrate.js), and apply to the bundle *before* it is uploaded. Each firmware release embeds the map as of that release, which is what lets any older backup restore correctly onto it with no version stamps anywhere.

The breaks a map cannot express are written down instead, in [MIGRATING](../reference/MIGRATING.md).

## Restoring onto a blank device

A backup restores onto a freshly erased device, which is what makes an erase safe to do.

Join the device's `MM-XXXX` access point, open `http://4.3.2.1`, restore there, and take the offered restart. The bundle carries the WiFi credentials, so the device comes back on your network by itself.

## From firmware older than the Backup button

The [installer page](https://moonmodules.org/MoonLight/install/) offers the same backup as a bookmarklet, so a device too old to have the button can still be captured before you update it.

## Moving a setup to another device

A restore is how you clone a rig. Take a backup from the working device, restore onto the new one, and read the report.

One thing to check afterwards: anything that names a pin. The configuration carries the pins the old board used, and if the new device is wired differently, the lights will be wrong in a way that looks like a broken effect. The [device name](../explanation/architecture/mooncore.md#device-name-one-identity-every-network-name-derives-from-it) comes across too, so give the clone its own.
