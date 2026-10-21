# Backup and restore

A backup is every setting a device holds in one `.json` bundle: the configuration of every module, your [presets](presets.md) and your MoonLive scripts. Taking one and putting it back, on the same device or another: [Back up and restore a device](../moonwalk.md#backup-restore).

## What a backup holds

The browser walks the whole filesystem. Every file is byte-verified against the directory listing as it is read, with three outcomes. A file shorter than listed aborts the backup, because silent truncation is worse than no backup at all. A file longer than listed is binary rather than text, so it is skipped and named. A file that cannot be read is skipped and named too.

One thing is left out on purpose: `/.hls`, the streaming encoder's scratch output, which is rewritten every second and would fill the report with skipped binaries.

## What a restore reports

The report lists what needed an eye:

- **Renamed and mapped**: a file, module type or control that changed name since the backup was taken, and what it became.
- **Values to review**: something the mapping could not decide on its own, usually because it depends on the chip or the wiring.
- **No longer present**: a module type or control this firmware does not have.

Most of a restore applies live, module by module, as each file lands. Two things do not, and the dialog names both:

- **Network settings** apply at boot, because bringing an interface up is not a re-runnable operation. The dialog offers the restart, after which the device joins the network the backup names. Its file is written **last** for that reason: joining another network mid-restore would cut off the writes still to come.
- **The web server's port**, which binds at boot.

## Why the browser does the work

The device never runs migration code. Its loader is robust by design: an absent key keeps the control's default, a stale value clamps to the new bounds, and an unknown key is ignored.

So the rename maps live in the browser, in [migrate.js](https://github.com/MoonModules/MoonLight/blob/main/src/ui/migrate.js), and apply to the bundle *before* it is uploaded. Each firmware release embeds the map as of that release, which is what lets any older backup restore correctly onto it with no version stamps anywhere.

The breaks a map cannot express are written down instead, in [MIGRATING](MIGRATING.md).
