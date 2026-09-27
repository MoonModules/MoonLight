# Plan: sweep every page through the documentation standards

**The gate**: a page is done when `vale <page>` reports nothing on the whole file, and the sweep is done when `vale docs/ src/ CLAUDE.md README.md` exits clean. That day, `.github/workflows/prose.yml` drops `filter_mode: added` and checks whole files, and `moondeck/check/check_prose.py` plus `hook_prose.py` are deleted, since their only job was the added-lines scope.

**Code is in scope.** A `///` becomes a generated page and a `//` is read beside the line it explains, so the standards govern both. `.vale.ini` holds `[src/**/*.{h,hpp,cpp,c,inc}]`, and `check_prose.py` feeds Vale the same suffixes plus `.py`, `.js` and the MoonLive scripts.

**A header is done when three things pass, not one.** Vale is necessary and not sufficient: the comment budget, the `///` on every public member and the `@card` line are structure rather than prose, and Vale never sees them.

| Gate | What it owns |
|---|---|
| `vale <file>` | em-dash, spelling, `e.g.`, sentence length, weasels, self-reference |
| `check_docgen` | the comment budget, a `///` on every public member, the `@card` image |
| by grep | a heading inside a comment, a relative `.md` link in a `///`, an unbackticked tag |

The third row is three traps the standards state and no tool enforces. Each silently loses content from the generated page, so each is a Vale rule worth writing: `CommentHeading`, `DoxygenRelativeLink` and `DoxygenRawTag`. Until they exist the sweep greps for them, which is why they are listed rather than assumed.

**How a header gets linted, since it is not obvious.** `[formats]` is the wrong tool: it maps an extension onto another *parser*, and C has none. The mechanism is a **View**, `.vale/styles/config/views/CComments.yml`, which runs a tree-sitter query over the file's syntax tree and extracts the comment nodes. Vale ships grammars for C, C++ and eighteen other languages, so the engine was there all along.

Three things that cost an afternoon, recorded so nobody repeats them:

- **`type: md` on the scope is load-bearing.** Vale's own `Python.yml` carries it. Without it the extracted text is never parsed and the run reports nothing.
- **`[formats]` is a SECTION, not a key.** Every key after it belongs to it until the next header, so placing it above `Vocab` swallowed that key into the format map. It now sits at the end of the file. `vale ls-config` is what shows this.
- **Vale's JSON omits files with no alerts.** A clean file and an unparsed one both print `{}`, which is how a broken setup reads as success. Always confirm against a file that MUST fail: a directory run reporting 364 alerts is proof, a single clean file is not.

`check_prose.py` now carries that confirmation as a **control**: it lints a fixture holding an em-dash and a British spelling, and fails when the fixture produces nothing. A missing View prints `view 'CComments' not found` and exits 0, so without the control every header reads as clean. The gate that reports zero has to prove it can report one.

**Regenerate the worklist**, do not edit it by hand: `vale --no-exit --output=line docs/ src/ CLAUDE.md README.md | cut -d: -f1 | sort | uniq -c | sort -rn`. A hand-kept list drifts the day someone forgets it. `src/` is in the command because it is in the gate: the two must name the same tree or the sweep finishes against the wrong one.

**The method**, from the two pages done so far: run Vale on the whole file, place the page in one Diátaxis cell and move what belongs elsewhere, cut what the code or another page already states, then diff the old page's vocabulary against the new to prove no fact was dropped rather than moved. The Linux tutorial lost four facts on the first pass and the diff found them.

## What now enforces the standards

Four things hold the standards after this sweep rather than letting them erode.

- **[Diátaxis](https://diataxis.fr/)**, followed as written. Every page is one of tutorial, how-to, reference or explanation, and the test for a page is the cell it sits in. The nav splits how-to guides from tutorials, and reference pages (MoonCloud, the privacy policy, firmware variants) sit under Reference.
- **Vale**, with the rules as YAML under `.vale/styles/MoonLight/`, one file per rule. Seven rules: em-dash, American spelling, `e.g.` to `such as`, sentence length, negated headings, weasel words, self-reference. It reads `docs/` and `src/` alike, the headers through a tree-sitter View that extracts their comments. `.github/workflows/prose.yml` annotates the errors a PR adds, with `fail_level: none`, because the unswept tree carries findings no PR put there. `check_prose.py` is the gate that fails, on added lines alone, and it goes when the sweep ends.
- **`--strict` on the docs build**, in CI and in the commit gate. A dead link or anchor fails the build. It caught 153 of them during the folder restructure.
- **`check_docgen`**, over every header under `src/` and every catalog card. It owns what Vale cannot see: the comment budget, a `///` on every public member, the card's image and its limits.

`.github/workflows/prose.yml` fires on `.h`, `.cpp` and `.vale/**` as well as `.md`. A header-only PR ran no prose job before that, which left the larger half of the tree unguarded by the one check that outlives this branch.

## Done (whole file clean)

- `docs/tutorials/installing-on-linux.md`
- `docs/contributing/documentation-standards.md`
- `docs/contributing/coding-standards.md`

## Also cleared this branch, by deletion rather than rewrite

- `docs/adr/`: 17 records, each already stated as current behavior in `architecture.md` or the standards.
- 92 shipped plans in `docs/work/past/plans/`, whose content the code, the tests and the merged PR carry.
- `docs/work/past/lessons.md` lost its branch diaries (20,510 to 15,733 words). **Its goal is removal**: each surviving lesson is either a constraint that belongs in the `.h` it guards, a rule that belongs in the standards, or history that belongs in git. Three method lessons already moved to `testing.md` and CLAUDE.md that way.

## Two pages with a decided shape, pending

- `docs/reference/performance.md`: its 234 rows of dated bench numbers overlap `docs/metrics/repo-health.md`, which is generated per commit. The measurements go to `metrics/`, the analysis stays; a hand-kept number next to a generated one is the drift the one-home rule forbids.
- `docs/explanation/architecture/index.md`: explanation, kept whole; reserved for a separate rework.

## Remaining: the code side

Headers carry more than twice the prose findings of every `.md` page together.

**Vale**: **3225 alerts in 226 files** across `src/`.

| Alerts | Rule | | Alerts | Directory |
|---|---|---|---|---|
| 2635 | em-dash | | 1224 | `src/core` |
| 489 | weasel | | 998 | `src/platform` |
| 100 | spelling | | 947 | `src/light` |
| 1 | sentence length | | 364 | `src/light/effects` |
| | | | 3 | `src/light/drivers` |

**`check_docgen`**: **2040 findings**, over every `.h` and `.cpp` under `src/` except the vendored ones. A root rather than a list of directories, because a list is a tolerance wearing different clothes: each directory it omits is silently exempt, and nobody notices a new one appearing.

Findings split by whether the file publishes a page, which is what decides severity: **1196 errors** in headers and catalog pages, **844 warnings** in implementation files. Only the errors fail the gate, so the remaining `.cpp` work cannot hold a header's commit hostage. The split is a staging device and names its own removal: when the warning column reaches zero it goes and everything blocks, the way Vale's config promotes a page to error as the sweep finishes it.

| Errors | Warnings | Area |
|---:|---:|---|
| 288 | 140 | `core/system.md` |
| 248 | 0 | `light/layouts.md` |
| 240 | 0 | `light/power-functions.md` |
| 163 | 0 | `light/modifiers.md` |
| 132 | 0 | `core/services.md` |
| 71 | 664 | tests, which no card covers |
| 2 | 40 | unassigned: files no summary page owns |

`ScratchBuffer.h` still renders no `@moreinfo` appendix on its generated page, and the cause is not yet identified. Its group is closed correctly and it is not the empty-group shape the checker now reports, so the two known causes are both ruled out. Worth one session with the Doxygen XML rather than another guess: three wrong theories died to the same mistake of reasoning from one example.

Two `@defgroup` ids are still duplicated, `moonlive_asm_riscv` and `moonlive_asm_xtensa`, each declared in a per-ISA header. Doxygen merges same-named groups, so one absorbs the other's page, which is the fault that took `drivers_PinList.md` off the site until the core parser's group was renamed. Nothing reports it yet.

Seven areas are at **0**: `light/drivers.md`, `light/effects.md`, `light/moonlive.md`, `light/supporting.md`, `core/supporting.md`, `core/ui.md` and `platform/index.md`. The live ranking is [docgen.md](../../reference/metrics/docgen.md), which the check writes.

Red by design. The way to green is solving each file, never widening a tolerance, and once green it stays green because nothing new is exempt by construction.

**Unenforced, counted by grep**: 30 headings inside a comment, 6 relative `.md` links in a `///`.

### One file, one pass

A file visited twice costs twice, and the second visit re-reads everything the first already loaded. So every gate runs against one file before moving to the next, and the file is read once.

**Top down, never finding by finding.** The unit of work is the declaration and what a reader needs to know about it, not the line a counter flagged. Read the file's structure, decide what each seam says, and write the comment that says it. A sweep driven by the finding list instead patches flagged lines one at a time, re-counts, and patches again: the count falls slowly, the prose stays shaped by whatever the previous author wrote, and promoting a `//` to a `///` *raises* the count, because a documented member is measured where an undocumented one is not. That is the bottom-up trap, and the finding list is a check on the result rather than a worklist to burn down.

1. **Read the findings first, not the file.** `check_docgen` names every rule it breaks and `vale <file>` names every line. Together they say what the edit must contain, so the file is opened knowing the whole job.
2. **Dump the comment runs once.** One pass prints every multi-line `//` and `///` run with the line it precedes. That output is the working set: do not re-read the file per finding.
3. **Settle the `///` first.** The class comment, its `@moreinfo` appendix, the `@card` line, and a one-line `///` on every public member the check names. This is the reader's view of the file, and it decides what is left to say.
4. **Then the `//`.** Each one is read against the `///` now above it: a block repeating what the doc comment says is deleted, and only a `//` carrying a constraint the `///` does not is kept and collapsed to its one line. Most are the first kind, which is why this order is cheaper than the reverse.
5. **Apply the [Writing](../../contributing/documentation-standards.md#writing) rules while writing, not after.** Every sentence in steps 3 and 4 is written under them: present tense, positive form, factual tone, one thought per sentence, the textbook name, no self-reference, no em-dash, American spelling. Writing the sentence twice is the multi-pass this procedure exists to avoid.
6. **Then read the file once as a reader would.** Three rules need the whole file rather than one comment, so they cannot bind in step 5: **say each fact once** (the class comment and a member `///` six screens apart, both stating it), **the class comment leads with what the reader needs first** after the depth moved to `@moreinfo`, and **a list holds one kind of thing, most important first**. This is a read, not a rewrite: it catches what the per-comment passes structurally cannot see.
7. **Write it as one edit.** Every phase lands in a single scripted pass, with each replacement asserted unique. A second edit means the first was incomplete.
8. **Verify once.** `check_docgen` and `vale` on that file. Both silent, or go back to step 7 with what they now say.
9. **Build only when the batch ends**, never per file: comments cannot break a build, and the compile is the expensive step.

**The shape of a swept header**, which the three rules below enforce between them:

| Position | Spelling | Budget |
|---|---|---|
| The file's first comment | `///`, a `@defgroup` block or the class comment | 10 lines, plus an `@moreinfo` appendix of 10 per `## ` section |
| Every class or struct | `///` | 10 lines, plus an `@moreinfo` appendix of 10 per `## ` section |
| Every public member | `///` | **one line** |
| A note beside code | `//` | **one line**, and only where a reader of the source needs it |

`@moreinfo` is the one home for depth, and only a lead carries one: the file's, or a class's. On a member it is depth in the wrong place, because the generated page shows a member's single line as its summary. That rule was briefly relaxed during the platform sweep and put a four-line block on every function in the file; the fix was to delete 230 appendix lines and move the handful of load-bearing findings up into the file lead.

A header that opens with `//` generates nothing: Doxygen reads `//` as a note to the next reader of the source. Four of the six platform headers opened that way, so their pages were a bare member list with no statement of what the file is for.

**Backlogged: a declaration-only header's functions never reach its generated page.** moxygen renders a `@defgroup`'s functions when they are `inline` definitions (`ParallelSlots.h`, `Hub75Slots.h`) and not when they are bare declarations. `platform.h` is 202 declarations against 3 definitions, so its page carries the 18 handle structs and none of `millis`, `alloc`, `gpioRead`, `ethSendRaw` or `hub75Init`: 245 functions, each with a `///` line nobody reads. The eight `Detail: technical` links on [the platform page](../../moonmodules/platform/index.md) therefore promise the interface and deliver the types it passes around, and they cannot carry per-section anchors until the sections exist. Doxygen's own XML is correct (one `func` section, 246 entries), so the fix is in the rendering: a moxygen template or upgrade, or rendering that section ourselves from the XML the generator already parses. Worth deciding alongside whether moxygen still earns its place against Doxygen's own HTML or Breathe into Sphinx, since the workarounds around it now number seven.

**Backlogged: require every `@moreinfo` section to be referenced.** `check_docgen` verifies that an `@xref{anchor}` names a heading in the same file, which catches a renamed heading silently dropping a link. The reverse rule, that every section carries at least one `@xref` pointing at it, is not enforced: 101 headers carry `@moreinfo` subsections and one uses `@xref`, so it would land roughly 300 findings on prose that is correct as written. Worth revisiting once the sweep has settled how often a reader is meant to arrive at a subsection through a link rather than by scrolling.

**What the edit must fix, all of it, in that one pass:**

- Every `//` run over one line, and every `///` run over one line that is not the class comment.
- A `///` on every public member the check names.
- The class `///` at 10 lines, each `@moreinfo` `## ` section at 10, every sentence in a comment at 30 words.
- No hard wrap: one line per sentence, in the class comment and the appendix alike.
- One `@card <Name>.gif` on the class `///`, since the catalog rule is a gif for effects, modifiers and layouts.
- Every em-dash, `e.g.`, British spelling and weasel Vale reports.
- No heading inside a comment, no relative `.md` link in a `///` (Doxygen drops it), no unbackticked tag.

**And the prose itself, which no check measures.** Vale catches the mechanical half: a spelling, a dash, a weasel. The [Writing](../../contributing/documentation-standards.md#writing) section is the other half, and a header that passes every check can still fail it. What it asks for, applied to a comment:

- **Present tense, positive form.** A comment narrating what the code no longer does is history, and git holds that. A real constraint stays; a bare absence goes.
- **A rule states a test**, and how it came to be broken belongs in the commit that fixed it. This is what most over-budget comments turn out to be: a bench story where one sentence would do.
- **Say it, then stop: about 40 words.** Past that a reader skims, and a skimmed comment is not followed.
- **A sentence is one thought.** Past twenty words it is usually two, joined by a comma that a full stop should have been. The 30-word cap enforces the count; this says what to do about it.
- **The text never refers to itself.** "As described above" and "see the section below" are the author stepping in front of the content.
- **One parenthetical per sentence**, and the textbook name for a thing rather than ours.

Reading the section before the first file of a batch is cheaper than rewriting the batch: these are the rules a sweep most often satisfies mechanically and fails in substance.

**Order within the sweep**: worst file first, by `check_docgen` count. The biggest files carry the patterns the rest repeat, so the shape settles early.

## Remaining: the docs side

**1133 alerts in 27 files.** The count fell from 2521 because `docs/work/` is exempt: past and future are quoted or inherited material, and a plan ends at its PR rather than being read twice.

| Alerts | Page |
|---|---|
| 114 | `docs/moonmodules/light/power-functions.md` |
| 92 | `docs/reference/performance.md` |
| 91 | `docs/how-to/home-automation.md` |
| 83 | `docs/tutorials/build-your-own-moonmodules.md` |
| 83 | `docs/reference/testing.md` |
| 74 | `docs/moonmodules/core/system.md` |
| 68 | `docs/moonmodules/light/MoonLiveEffect.md` |
| 67 | `docs/how-to/building.md` |
| 63 | `docs/moonmodules/light/layouts.md` |
| 57 | `docs/gettingstarted.md` |
| 50 | `docs/reference/MIGRATING.md` |
| 41 | `docs/moonmodules/core/ui.md` |
| 27 | `docs/moonmodules/light/modifiers.md` |
| 26 | `docs/reference/hardware/gpio-usage.md` |
| 26 | `docs/moonmodules/core/services.md` |
| 25 | `docs/reference/hardware/esp32-s31-coreboard.md` |
| 24 | `docs/tutorials/how-projectmm-works.md` |
| 21 | `docs/tutorials/generative-effects.md` |
| 21 | `docs/moonmodules/light/supporting.md` |
| 20 | `docs/reference/hardware/mhc-wled-esp32-p4-shield.md` |
| 16 | `docs/moonmodules/core/control.md` |
| 15 | `docs/how-to/logging-an-issue.md` |
| 11 | `docs/how-to/panel-cards.md` |
| 10 | `docs/how-to/installing-to-desktop.md` |
| 4 | `docs/moonmodules/light/drivers.md` |
| 3 | `docs/moonmodules/light/effects.md` |
| 1 | `docs/moonmodules/light/index.md` |
