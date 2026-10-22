# Past: what we built

Shipped plans and dated records.

**A shipped plan is a working document, not testimony.** Whatever its PR already carries is duplication and can be trimmed or deleted. What earns keeping is the part no PR and no commit holds: the alternatives considered and rejected, and the reason the chosen shape won.

`plans/` holds every plan that left `present`, each named with its status, `(shipped)`, `(superseded)`, `(cancelled)` or `(attempted, reverted)`. A plan moves here in its own PR ([CLAUDE.md, Branch](../../../CLAUDE.md#main-and-branch)).

The prior-version inventories beside it state what was true at a moment and stay unrewritten. Release notes live on [the GitHub release](https://github.com/MoonModules/MoonLight/releases) itself, which is their one home: a copy here drifts from what shipped, and the links in one pinned to an older tag went stale exactly that way.

## The record beside the plans

- [lessons.md](lessons.md): hard-won debugging lessons and gotchas, a bug, its cause and the fix, recorded with the code that proved them. A lesson that hardened into a *rule* lives in [CLAUDE.md](../../../CLAUDE.md) or [coding-standards.md](../../contributing/coding-standards.md) instead.

The surveys of v1, v2 and MoonLight that once sat here are gone. They were written to decide what to harvest into v3, that decision shipped, and each described code that lives in its own repository and in git.

The LED-driver and shift-register design analyses moved to [future](../future/index.md), where the other research documents sit: they were written to decide what to build, and that is what future holds.
