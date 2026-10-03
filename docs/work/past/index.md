# Past: what we built

Shipped plans and dated records.

**A shipped plan is a working document, not testimony.** Whatever its PR already carries is duplication and can be trimmed or deleted. What earns keeping is the part no PR and no commit holds: the alternatives considered and rejected, and the reason the chosen shape won.

`plans/` holds the plans that shipped before the current rule, when a plan was archived rather than folded into its PR. They map onto merged PRs, and the mapping is the next thing to write here. From now on a plan ends at its PR ([CLAUDE.md, Branch](../../../CLAUDE.md#branch)), so this folder stops growing.

The prior-version inventories beside it state what was true at a moment and stay unrewritten. Release notes live on [the GitHub release](https://github.com/MoonModules/MoonLight/releases) itself, which is their one home: a copy here drifts from what shipped, and the links in one pinned to an older tag went stale exactly that way.

## The record beside the plans

- [lessons.md](lessons.md): hard-won debugging lessons and gotchas, a bug, its cause and the fix, recorded with the code that proved them. A lesson that hardened into a *rule* lives in [CLAUDE.md](../../../CLAUDE.md) or [coding-standards.md](../../contributing/coding-standards.md) instead.

The surveys of v1, v2 and MoonLight that once sat here are gone. They were written to decide what to harvest into v3, that decision shipped, and each described code that lives in its own repository and in git.

The LED-driver and shift-register design analyses moved to [future](../future/index.md), where the other research documents sit: they were written to decide what to build, and that is what future holds.

## Plans and their PRs

21 plans, each mapped to the merged pull request that carries the same design plus the diff that implemented it. The mapping is derived from title words and merge date, so it is a starting point rather than a record: a `?` means no confident match, and any row is worth checking before it is trusted. 4 need a hand.

| Date | Plan | PR |
|---|---|---|
| 2026-08-13 | [MoonLive on a stack machine: the frame is where values live](plans/Plan-20260813%20-%20MoonLive%20on%20a%20stack%20machine%20%E2%80%94%20the%20frame%20is%20where%20values%20live%20%28shipped%29.md) | [#63](https://github.com/MoonModules/MoonLight/pull/63) |
| 2026-08-17 | [MoonLive scripts are classes](plans/Plan-20260817%20-%20MoonLive%20scripts%20are%20classes%20%28shipped%29.md) | [#65](https://github.com/MoonModules/MoonLight/pull/65) |
| 2026-08-18 | [A file editor control and a filesystem change seam](plans/Plan-20260818%20-%20A%20file%20editor%20control%20and%20a%20filesystem%20change%20seam%20%28shipped%29.md) | [#67](https://github.com/MoonModules/MoonLight/pull/67) |
| 2026-08-21 | [MoonLive on Windows](plans/Plan-20260821%20-%20MoonLive%20on%20Windows%20%28x86_64%20host-JIT%20backend%29%20%28shipped%29.md) | [#73](https://github.com/MoonModules/MoonLight/pull/73) |
| 2026-08-21 | [Particles in MoonLive, plus a fade builtin](plans/Plan-20260821%20-%20Particles%20in%20MoonLive%2C%20plus%20a%20fade%20builtin%20%28shipped%29.md) | [#71](https://github.com/MoonModules/MoonLight/pull/71) |
| 2026-08-22 | [Raw L2 Ethernet on Windows](plans/Plan-20260822%20-%20Raw%20L2%20Ethernet%20on%20Windows%20%28shipped%29.md) | [#73](https://github.com/MoonModules/MoonLight/pull/73) |
| 2026-08-23 | [A Windows installer, and settings that persist](plans/Plan-20260823%20-%20A%20Windows%20installer%2C%20and%20settings%20that%20persist%20%28shipped%29.md) | [#74](https://github.com/MoonModules/MoonLight/pull/74) |
| 2026-08-23 | [Five types for MoonLive scripts](plans/Plan-20260823%20-%20Five%20types%20for%20MoonLive%20scripts%20%28shipped%29.md) | [#77](https://github.com/MoonModules/MoonLight/pull/77) |
| 2026-08-24 | [NDI output](plans/Plan-20260824%20-%20NDI%20output%20%28shipped%29.md) | [#80](https://github.com/MoonModules/MoonLight/pull/80) |
| 2026-08-25 | [A lossy channel for the preview](plans/Plan-20260825%20-%20A%20lossy%20channel%20for%20the%20preview%20%28shipped%29.md) | [#81](https://github.com/MoonModules/MoonLight/pull/81) |
| 2026-08-25 | [Client-driven preview adaptation](plans/Plan-20260825%20-%20Client-driven%20preview%20adaptation%20%28superseded%29.md) | [#81](https://github.com/MoonModules/MoonLight/pull/81) |
| 2026-08-25 | [Lean preview transport](plans/Plan-20260825%20-%20Lean%20preview%20transport%20%28shipped%29.md) | [#81](https://github.com/MoonModules/MoonLight/pull/81) |
| 2026-08-27 | [HLS on ESP32-P4](plans/Plan-20260827%20-%20HLS%20on%20ESP32-P4%20%28shipped%29.md) | [#78](https://github.com/MoonModules/MoonLight/pull/78) |
| 2026-08-27 | [HLS streaming driver](plans/Plan-20260827%20-%20HLS%20streaming%20driver%20%28shipped%29.md) | [#85](https://github.com/MoonModules/MoonLight/pull/85) |
| 2026-08-27 | [Raw-L2 interface dropdown](plans/Plan-20260827%20-%20Raw-L2%20interface%20dropdown%20%28shipped%29.md) | [#85](https://github.com/MoonModules/MoonLight/pull/85) |
| 2026-08-27 | [Sprites and flying toasters](plans/Plan-20260827%20-%20Sprites%20and%20flying%20toasters%20%28shipped%29.md) | [#86](https://github.com/MoonModules/MoonLight/pull/86) |
| 2026-08-30 | [Ship the MoonLive script library](plans/Plan-20260830%20-%20Ship%20the%20MoonLive%20script%20library%20%28shipped%29.md) | [#89](https://github.com/MoonModules/MoonLight/pull/89) |
| 2026-08-31 | [Scripts declare dimensions and tags](plans/Plan-20260831%20-%20Scripts%20declare%20dimensions%20and%20tags%20%28shipped%29.md) | [#89](https://github.com/MoonModules/MoonLight/pull/89) |
| 2026-09-22 | [MoonLight, from v5.0.0 to the rename](plans/Plan-20260922%20-%20MoonLight%2C%20from%20v5.0.0%20to%20the%20rename%20%28shipped%29.md) | [#119](https://github.com/MoonModules/MoonLight/pull/119) |
| 2026-09-22 | [RTSP video out on the P4 and the desktop](plans/Plan-20260922%20-%20RTSP%20video%20out%20on%20the%20P4%20and%20the%20desktop%20%28shipped%29.md) | [#110](https://github.com/MoonModules/MoonLight/pull/110) |
| 2026-10-02 | [One way to send to many receivers](plans/Plan-20261002%20-%20One%20way%20to%20send%20to%20many%20receivers%20%28shipped%29.md) | [#123](https://github.com/MoonModules/MoonLight/pull/123) |
