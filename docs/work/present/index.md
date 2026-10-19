# Present: what we are building

One file per plan being built now, as `Plan-YYYYMMDD - <title>.md`.

A plan is written in plan mode, approved before work starts, and edited as reality changes. **Its life ends at its PR**: the plan becomes the PR description and the file is deleted in that same PR ([CLAUDE.md, Branch](../../../CLAUDE.md#main-and-branch)). The merged PR is then the design record, and it carries the diff the plan describes.

A file here that no longer has work behind it is either finished, and belongs in its PR, or abandoned, and belongs deleted.

## Next (2026-10-13)

1. **[Boards known by name](Plan-20261009%20-%20Boards%20known%20by%20name.md)**, not started. A setting that points at another board resolves its name through MoonLight's own Devices discovery before mDNS, and cards name their peers. It also gives the Teensy board names, since its lwIP has no mDNS lookup: a `.local` name never resolves there, and a driver waits on it rather than saying so.
2. **[Layer compositing](Plan-20261011%20-%20Layer%20compositing%2C%20masks%20and%20transitions.md), step 3: transitions.** Steps 1 and 2 shipped in #137. The old effect fades out while the new one fades in, with one ramp type shared with the boot soft start; a Discord request.
3. **Downloads on the tick path.** Every upload now moves through the transfer slots; a flash download still writes its whole file on one tick, bounded per chunk by the write deadline. Moving downloads onto the slots too needs a look at how many files the UI reads at once, since two slots answer a third request with 503. Firmware uploads drain whole by design and feed the watchdog.

| Plan | State |
|---|---|
| Teensy 4.1, State by subscription, MoonLive Coord3D and RGB | shipped in #137 |
| Native HUB75 output | shipped |
| Network submodules | shipped, partly verified |
| Audio codec | in PR 129, M3 unverified |
| ESP32-C3 and the Athom bulb | open, waiting on the bulb |
| Input mapping, Gamepads and MIDI desks, Presets as state documents, Code ratchets | partial |
