# Backup and restore

A backup is every setting a device holds in one `.json` bundle: the configuration of every module, your [presets](presets.md) and your MoonLive scripts. Taking one and putting it back, on the same device or another: [Back up and restore a device](../moonwalk.md#backup-restore).

<video src="../assets/moontube/16-restore-a-backup.webm" controls playsinline width="720" title="A backup from before the config files changed shape, restored through the File Manager: the report of what was renamed, then the effect and the brightness back as they were saved."></video>

## What a backup holds

The browser walks the whole filesystem. Every file is byte-verified against the directory listing as it is read, with three outcomes. A file shorter than listed aborts the backup, because silent truncation is worse than no backup at all. A file longer than listed is binary rather than text, so it is skipped and named. A file that cannot be read is skipped and named too.

One thing is left out on purpose: `/.hls`, the streaming encoder's scratch output, which is rewritten every second and would fill the report with skipped binaries.

## Restore or clone

Choosing a bundle asks what it is for. **Restore** puts a device back as the backup was, its identity included: the same device after an erase or a failed update. **Clone** makes a device another one like the backup, as a cloned disk image gets its own machine identity: the device keeps what makes it itself and takes everything else.

| Kept by a clone | Why it differs per device |
|---|---|
| `deviceName` | its network name: the mDNS hostname, the access point and the DHCP hostname all derive from it |
| WiFi's known networks | each with its password, which stays on the device it was typed into |
| Ethernet's static IP settings | two devices claiming one address both drop off the network |
| the desktop's stored hardware address | the default device name derives from it |

What differs per device beyond its identity, such as each pole's Art-Net universe start, is set after the clone, by hand or through [the HTTP API](integrating.md).

## Many devices alike

A device's setup is two parts, and each already has its tool. The **board** is its device model: the [installer](https://moonmodules.org/MoonLight/install/) applies a model's pins, drivers and layout while it flashes. The **show** is everything else: clone it from a backup of the first device, or apply it as a [preset](presets.md), a state document a click recalls and the gallery shares. Flash each device with its model, then clone or apply the show onto it.

## What a restore reports

The report lists what needed an eye:

- **Renamed and mapped**: a file, module type or control that changed name since the backup was taken, and what it became.
- **Values to review**: something the mapping could not decide on its own, usually because it depends on the chip or the wiring.
- **No longer present**: a module type or control this firmware does not have.

Most of a restore applies live, module by module, as each file lands. Two things do not, and the dialog names both:

- **Network settings** apply at boot, because bringing an interface up is not a re-runnable operation. The dialog offers the restart, after which a restored device joins the network the backup names and a cloned one rejoins its own. Its file is written **last** for that reason: joining another network mid-restore would cut off the writes still to come.
- **The web server's port**, which binds at boot.

## Why the browser does the work

The device never runs migration code. Its loader is robust by design: an absent key keeps the control's default, a stale value clamps to the new bounds, and an unknown key is ignored.

So the rename maps live in the browser, in [migrate.js](https://github.com/MoonModules/MoonLight/blob/main/src/ui/migrate.js), and apply to the bundle *before* it is uploaded. Each firmware release embeds the map as of that release, which is what lets any older backup restore correctly onto it with no version stamps anywhere.

The breaks a map cannot express are written down instead, in [MIGRATING](MIGRATING.md).
