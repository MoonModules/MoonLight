# Documentation standards

How the project writes prose: docs, comments, and the pages generated from them. For developers, the people who write those. Code rules are in [coding-standards.md](coding-standards.md). The two meet at `///` comments, which are code by location and documentation by purpose: their syntax is a coding-standards rule, everything they say follows this page.

Every rule has one home. Another document links here rather than restating, because two copies become two different rules.

It starts with the four kinds of page and which one each of ours is. Then how a page is written, then the two pages every module has. It ends with comments: the smallest scale, and the one place code and prose meet.

## The hierarchy

A reader enters at the top and stops as soon as they have enough. A writer puts a fact at the shallowest level that fully owns it, and links to it from every level above. Purple is the four [Diátaxis](https://diataxis.fr/) types, green is written once in the source and generated from there, and gold sits outside the grid.

```mermaid
flowchart TB
    entry["<b>README.md</b> · <b>index.md</b><br/><i>what it is, what to do next</i>"]

    tut["<b>tutorial</b><br/>gettingstarted.md · tutorials/<br/><i>a lesson to follow</i>"]
    how["<b>how-to</b><br/>how-to/<br/><i>one task you already have</i>"]
    exp["<b>explanation</b><br/>explanation/<br/><i>why it is shaped this way</i>"]
    ref["<b>reference</b><br/>reference/ · moonmodules/<br/><i>facts, fast</i>"]

    mod["<b>moonmodules/</b><br/><i>reference: one row per module</i>"]
    mox["<b>moxygen/</b><br/><i>every member, generated</i>"]
    hdr["<b>src/**/*.h</b><br/><i>per-member detail lives here</i>"]

    rules["<b>the rules</b><br/>CLAUDE.md · contributing/<br/><i>for contributors</i>"]
    outside["<b>work/</b> · <b>friend-repos/</b><br/><i>planned, shipped, watched</i>"]

    entry --> tut --> how --> exp --> ref
    entry --> mod --> mox --> hdr
    entry -.-> rules
    rules -.-> outside

    classDef entryCell fill:#2d3561,stroke:#7b88c9,color:#fff
    classDef diataxis fill:#3d2d61,stroke:#a07bc9,color:#fff
    classDef generated fill:#1f4d3d,stroke:#5fb89a,color:#fff
    classDef outsideGrid fill:#4d3d1f,stroke:#c9a95f,color:#fff
    class entry entryCell
    class tut,how,exp,ref diataxis
    class mod,mox,hdr generated
    class rules,outside outsideGrid
```

Each level says what a thing is and links down for the rest. A fact stated above its home is a second copy that drifts.

### Zooming in on the green boxes

The three green levels above are one domain, one summary page, one group of headers. A reader drills down: pick the domain, pick the page whose subject matches the question, follow a card's `technical` link into the members. A writer walks the same path in reverse, and a documentation sweep follows it page by page, which is how progress is tracked.

```mermaid
flowchart LR
    core["<b>moonmodules/core/</b><br/><i>domain-neutral</i>"]
    light["<b>moonmodules/light/</b><br/><i>the light domain</i>"]
    plat["<b>moonmodules/platform/</b><br/><i>the hardware seam</i>"]

    csys["<b>system.md</b>"]
    csvc["<b>services.md</b>"]
    cui["<b>ui.md</b>"]
    csup["<b>supporting.md</b>"]

    leff["<b>effects.md</b>"]
    llay["<b>layouts.md</b>"]
    lmod["<b>modifiers.md</b>"]
    ldrv["<b>drivers.md</b>"]
    lpf["<b>power-functions.md</b>"]
    lml["<b>moonlive.md</b>"]
    lsup["<b>supporting.md</b>"]

    pidx["<b>index.md</b>"]

    hsys["<b>core/system/</b> · <b>core/module/</b><br/><i>the modules, and what a module is</i>"]
    hsvc["<b>core/services/</b><br/><i>10 headers</i>"]
    hctl["<b>core/util/</b><br/><i>the shared building blocks</i>"]
    hui["<b>core/system/</b> · <b>src/ui/</b><br/><i>the served app</i>"]

    heff["<b>light/effects/</b><br/><i>67 headers</i>"]
    hlay["<b>light/layouts/</b><br/><i>18 headers</i>"]
    hmod["<b>light/modifiers/</b><br/><i>12 headers</i>"]
    hdrv["<b>light/drivers/</b><br/><i>24 headers</i>"]
    hpf["<b>light/powerfunctions/</b><br/><i>the shared toolbox</i>"]
    hml["<b>core/moonlive/</b> · <b>light/moonlive/</b><br/><i>engine and bindings</i>"]
    hlyr["<b>light/layers/</b> · <b>light/util/</b><br/><i>the pieces they share</i>"]
    hplat["<b>platform/</b> · <b>platform/esp32/</b> · <b>platform/desktop/</b><br/><i>one interface, two implementations</i>"]

    core --> csys & csvc & cui & csup
    light --> leff & llay & lmod & ldrv & lpf & lml & lsup
    plat --> pidx

    csys --> hsys
    csvc --> hsvc
    cui --> hui
    csup --> hctl

    leff --> heff
    llay --> hlay
    lmod --> hmod
    ldrv --> hdrv
    lpf --> hpf
    lml --> hml
    lsup --> hlyr
    pidx --> hplat

    classDef domain fill:#2d3561,stroke:#7b88c9,color:#fff
    classDef page fill:#1f4d3d,stroke:#5fb89a,color:#fff
    classDef headers fill:#3d2d61,stroke:#a07bc9,color:#fff
    class core,light,plat domain
    class csys,csvc,cui,csup,leff,llay,lmod,ldrv,lpf,lml,lsup,pidx page
    class hsys,hsvc,hctl,hui,heff,hlay,hmod,hdrv,hpf,hml,hlyr,hplat headers
```

A page owns one subject, so a header belongs to the page whose subject it serves rather than to the folder it sits in. `MoonLiveEffect.h` is carded on `effects.md` beside the compiled effects. The pieces several pages share, such as `Layer`, `Buffer` and `MappingLUT`, are carded once on `supporting.md`.

The README is the strictest case. It is the page most often written as if it were the only one. It names a capability and links to the page that owns it, so a paragraph of detail there sits in the wrong place.

## What we document

Document a thing once, in the place closest to it, and link the rest. A fact the source states is never re-typed in prose, so the question "where does this belong?" has one answer, and so does "where do I find it?"

Every page serves one of four reader needs, and only one. This is [Diátaxis](https://diataxis.fr/), followed as written: the four come from two questions, whether the reader is **learning** or **working**, and whether they want to **do** something or **understand** it.

| | Doing | Understanding |
|---|---|---|
| **Learning** | **Tutorial**: a lesson to follow. `gettingstarted.md`, `tutorials/`. | **Explanation**: why it is shaped this way. `explanation/`. |
| **Working** | **How-to**: one task you already have. `how-to/`. | **Reference**: facts, fast. `reference/`, the generated technical pages, the catalog rows. |

**The folder under `docs/` is the type.** A page's path says which cell it sits in. So `how-to/building.md` is a how-to by location, and a reader never has to be told. The nav labels stay reader-facing ("Understanding MoonLight" over "Explanation"), because the type is a writer's tool.

The test for any page is the cell it sits in. A tutorial that stops to explain, or a reference that starts to teach, is two pages: move the other half to where it belongs.

Two kinds of page sit outside the grid on purpose. **The rules** ([CLAUDE.md](../../CLAUDE.md), [coding-standards.md](coding-standards.md), this page) live in `contributing/`, are for contributors, and bind every change. `legal/` sits outside it too. **Work not yet in the code** lives under `docs/work/`, and nothing there describes the system as it is. `future` is what does not exist, `present` is being built and deleted at its PR, `past` is what shipped.

Two scales below a page. **A module** has exactly one reference page written and one generated (see [Module pages](#module-pages)); **a line of code** carries its own reason in a comment (see [Comments](#comments)).

**The split that decides everything else.** A hand-written page says what the code cannot; a generated page IS the code. Nobody edits a generated page, because the next build overwrites it. So a fact about behavior belongs in the `.h` and reaches the reader through generation; a fact about intent, cost or sequence has no source to generate from, and is written.

**A shipped plan is a working document, not testimony.** It may be edited, trimmed, or deleted: whatever its PR already carries is duplication. Only a dated record stating what was true at a moment (release notes, an inventory quoted from another project) is kept unrewritten.

## Writing

- **A page is read start to finish by one reader**, a **user** or a **developer**. A user brings no coding and no hardware knowledge beyond plugging in a board; a developer brings C++, embedded and this codebase's shape. Where a page serves both, lead with the user and put the depth lower down.
- **The headings are the page's table of contents, and they read top to bottom.** A few lines say what the page is and one paragraph says how it is laid out. The sections then follow in the order a reader needs them. A title that makes sense only after reading the body is the order being wrong.
- **One tone of voice, everywhere: factual, no nonsense.** State what is true and what to do, addressing the reader as "you". Leave out enthusiasm, apology, and how we felt building it. Only the assumed knowledge changes between pages, never the voice.
- **No sentence whose job is tone.** Every sentence carries a fact the reader needs. Three shapes to cut: a second person used for effect rather than instruction, a flourish before any information arrives, and a rhetorical question the page then answers. Vale checks spelling, sentence length and weasel words, so this one is the writer's judgement and the reviewer's check.
- **Follow the [principles](../CLAUDE.md#principles).** Three bear on documentation directly:
    - **Minimalism**: every fact has one home; history lives in git.
        - **Present tense only.** "No X anymore" narrates a removal, which is history. Describe the path that exists today.
        - **Positive form only.** "Not", "never", "neither", "without", "un-" and "non-" are the alarm bells: a negation says everything a thing is not, which is no shape at all. A real constraint stays ("the DMA cannot read PSRAM at shift clock"); a bare absence goes.
    - **Industry standards**: the textbook name for a thing, so a reader recognizes it without being taught our vocabulary. A bespoke choice carries its one-line reason where it appears.
    - **Continuous improvement**: a doc describing what the code no longer does is a defect. Fix it in the change that opened the file, not in a sweep.
- **A page links down to detail, it does not absorb it.** Each level says what a thing is and sends the reader to the level that owns the detail. A fact stated above its home is a second copy that drifts. The test: if removing a paragraph costs nothing but a link, it was never this page's to hold. The ladder is in [The hierarchy](#the-hierarchy).
- **A list holds one kind of thing, most important first.** The heading rule, one level down: what a reader reaches for most often leads. A list mixing categories is really two lists.
- **A rule states a test.** Something a reader can hold a page against and get a yes or no. How the rule came to be broken is history, and goes in the commit that fixed it.
- **Say it, then stop: about 30 words.** One or two sentences a reader can act on, and a reason only when the point is surprising or has been got wrong before. Past that a reader skims, and a skimmed statement is not followed.
- **Write for the reader who will follow it**, not the one arguing with it. A trap that only bites whoever maintains the tooling belongs in the tooling.
- **Ask, do not argue.** A request states what you want and why, then stops. Quoting the other side's code back to them and pre-empting every objection is pressure, and earns a reply shorter than the message. Ask the one question that decides the rest.
- **A sentence is one thought.** Past twenty words it is usually two, joined by a comma or a colon that a full stop should have been. Instructions in particular: one step, one sentence, and the reader's eyes never lose the line.
- **The text never refers to itself.** "This page", "this recipe", "as described above", "in the following section": each one is the author stepping in front of the content. Say the thing; the reader knows where they are.
- **A diagram beats the paragraph that describes it.** Draw a structure, a flow or a hierarchy as a [Mermaid](https://mermaid.js.org/) diagram: it renders on the site and on GitHub, and it diffs as text. A screenshot does the same where the point is what a reader sees. Both follow the example rule: they replace prose rather than decorate it.
- **One example, only where the prose alone would be misread.** A code block earns its place by preventing a wrong reading; a second example is the author enjoying the subject.
- **A link's text says what it reaches.** "See the hot path rule" tells the reader whether to follow it; "see here" does not, and a bare filename only if the filename is the point.
- **One parenthetical per sentence.** A second qualification means the sentence carries two ideas: split it, or drop the weaker one.
- **Mechanism lives with the mechanism.** How a generator or script works belongs in that script. A page says what the reader must do.
- **American English spelling, everywhere**: identifiers, wire keys, comments, docs, UI strings. A grep for one dialect silently misses the other, and a drifting wire key breaks a contract with no compile error. A proper noun keeps its own spelling.
- **No em-dashes in prose.** Use a comma, colon, parentheses, or a full stop. A literal one in a UI string or test fixture stays.
- **No hard line wraps in markdown.** Let the editor soft-wrap, so a one-word edit is a one-word diff.
- **Convert as you touch.** Spelling and em-dash fixes ride the change that opens the file, never a repo-wide sweep. `check_prose.py` checks added lines only, for the same reason.

## Module pages

### Two surfaces per module

1. **A summary page**, hand-written, for the end user. One table row in its group's page, carrying the module's description, image, controls, and links. Catalog controls live here because they are runtime `controls_.add(...)` calls that no static tool sees.
2. **A technical page**, generated from the header's `///` comments by [`moondeck/docs/gen_api.py`](../moondeck/docs/gen_api.py).

`docs/moonmodules/` mirrors `src/`: a `core/` and a `light/` subtree, one flat page per group. The tree is `<core|light>/<type>/Module`, flat within a type, and **library is a tag, not a folder level**. In code and assets it rides in `tags()` and in docs in the page name, so `effects.md` splits to `effects_<library>.md` only when a section outgrows it. A blended-lineage module then needs no file move, and a mis-filing is a one-line edit. Generated pages are gitignored and rebuilt. Test inventories have their own generator, [`generate_test_docs.py`](../moondeck/docs/generate_test_docs.py), because unit tests are macros and scenarios are JSON.

**Reference a module generically** outside its own home: say "a modifier", not `FooModifier`. A module's home is its `.h`, its catalog card, its registration, and its tests. Naming it elsewhere multiplies rename cost.

**No per-module detail page.** Cross-file rationale that no single `.h` owns goes in a prose section under its group's summary page. Rationale shared by sibling modules lives once on their base class.

**Every generated page is reachable.** A technical page nothing links to is a page no reader arrives at. The summary pages are what make the generated tree navigable, and `check_docgen` reports any page with no link into it. The fix is a link from the group's summary page rather than a deletion. The page is generated from a header that exists, so an unreachable one means the summary is incomplete.

### The card

Two columns. **Module** leads with the image, then the name and description. **Details** carries the controls, then three links below a rule: **Tests**, **API** (the generated page), **Details** (the section below the cards). Those three always show, greyed when there is no target, so their position is learned once. Every other link is prose, made in the sentence that needs it.

Every card leads with an image: `.gif` on effects, modifiers and layouts, which show motion, `.png` elsewhere.

A control reads `` - `name`: what it does ``. The colon separates the name from its description, and the build styles the two differently.

| | Limit |
|---|---|
| Description | 600 characters, 400 on effects, modifiers and layouts |
| One control | 100 characters, 80 on effects, modifiers and layouts |
| Details table | 4 columns, 300 characters a cell |

There is no limit on the controls together. A module with twelve honest controls is not worse documented than one with three, and capping the sum only punished the richer module. The visual catalogs are tighter because the `.gif` carries the description, so the words are there for what the reader cannot see.

Over a limit the text **moves rather than shrinks**, and its home follows from what it is. Behavior of the module goes in the header's `///`, reaching the reader as the generated page. What a reader needs before choosing between siblings goes in a `## <Name>, details` section below the cards, which the build links from the row. Trimming to fit is the one wrong answer, for the reason [Comments](#comments) gives.

A details section continues the card, so it is written for the same reader. Implementation belongs in the `///`. The heading takes a comma, never an em-dash: the build matches `## <Name>, details` exactly, and [em-dashes are banned](#writing) everywhere.

The card rules are enforced by [`check_docgen.py`](../moondeck/check/check_docgen.py), pinned by [`test_check_docgen.py`](../../test/python/test_check_docgen.py). Where a card begins and ends is the build's own rule, shared: the renderer, `check_docgen` and `check_specs` all ask `split_blocks` rather than each deciding for itself. A check that silently stopped matching would report a clean run, so each rule is tested firing as well as staying quiet.

**Prose is Vale's**, wherever it sits: a page, a `///` block, a comment. One checker for what text says, so a rule has one home and one vocabulary.

A header reaches Vale through a **View**, `.vale/styles/config/views/CComments.yml`, which runs a tree-sitter query over the file's syntax tree and hands back the comment nodes alone. Vale has no native parser for C, so without the View it skips a header and reports nothing, which reads exactly like a clean file.

## Comments

- **Comments say WHY.** Restating what the line does is noise, and usually a naming failure: see [prefer naming over commenting](coding-standards.md#writing-a-line-of-code).
- **One line, above the code it explains.** A second line is the author still talking. A class `///` gets about ten lines, and each `## ` section of an `@moreinfo` appendix about ten; over that, cut. A file whose comments outnumber its code has stopped being a header.
- **Settle the `///` first, then the `//`.** The doc comment is what a reader sees on the generated page, so it is where the explanation belongs. A `//` block below one that repeats it is deleted rather than shortened, and most of them turn out to be exactly that. Working the other way round collapses a `//` into one careful line, then deletes it an hour later once the `///` above says the same thing.
- **Keep the constraint, cut the exposition.** A constraint cannot be recovered from the code: the latch is 300 us, the DMA cannot read PSRAM at shift clock. What was tried first, and why this pattern over another, goes in the commit message.
- **Removing a comment needs the same justification as removing code**: outdated, wrong, or it only restated the code. Never strip to hit a length target, and never delete a reason you cannot reconstruct.
- **Say each fact once.** A restatement for emphasis reads as new information and costs the reader a second pass to learn it is not.
- **A heading inside a comment means it is not a comment.** Needing signposts is the signal to cut. The one exception is `@moreinfo`, whose `##` sections become the generated page's own headings; inside a lead comment a heading wants to be a page, or wants to not exist.
- **MoonLive scripts get three comments, one line each**: what the script is, what each `addControl` knob does, what each function it defines does. Lifecycle functions need none. A script is read in the device's own editor, where prose buries the effect.

### The comment budget

A card is read across a row; a member comment is read beside the thing it describes, and the generated page shows its first sentence as the summary. So the budget is one line, and a deep dive goes after `@moreinfo`. In a header, which is where a member comment becomes a page row and where `@moreinfo` exists to receive the depth. An implementation file has neither, so the rule stops there rather than naming a move its reader cannot make.

**A header opens with `///`**, so its page says what the file is for rather than starting with a bare member list. A header of free functions, constants or several peer types leads with a `@defgroup` block. A header declaring one type leads with that type's comment, which is the file's documentation already, and takes no group around it. The two are exclusive. A group wrapping a lone class is a second lead saying the same thing twice, leaving an editor two homes to keep in step. Nested types are implementation detail and do not make a header a multi-type one. A provenance marker above the lead, an SPDX tag or an `// Author:` line, is machine-read or a credit rather than documentation, and passes through. A `//` block at the top generates nothing, because Doxygen reads `//` as a note to the next reader of the source.

**`@moreinfo` is an appendix, and only a lead carries one**: the file's, or a class's. A member gets one line, so an `@moreinfo` there is depth in the wrong place, and it belongs in the file lead's appendix or on the module's page. Relaxing that once put a four-line block on every function in a swept header and cost 230 lines to undo.

| | Limit |
|---|---|
| The file's `///` lead | 10 lines |
| Class `///` | 10 lines |
| An `@moreinfo` `## ` section | 10 lines, and the appendix only on a file or class lead |
| Any other `///` run | 1 line |
| One sentence in a comment | 30 words |
| A `//` run beside code | 1 line |
| One comment line | 250 characters, on the way down |
| One line of a file or class lead | 400 characters |
| A whole `///` run, lead or class comment | 2500 characters |
| A sentence in a comment | 1 line, never wrapped |
| Every public member | carries one |

**A lead's line is wider, and a run is capped as a block.** A lead is the summary that opens a header or a class. One of its lines carries what the whole thing is for: what it is, which hardware it runs on, where the shared body lives. Held to the member cap the driver headers each lost about a third of that, and what went was content. The run budget is what stops a lead sprawling instead. It also closes the cheapest way to satisfy a line cap: splitting one long line into two shorter ones leaves the text identical and every per-line rule passing.

**The line cap is a ratchet, not a target.** It sits at 250 and comes down as the tree does, which is one constant in `check_docgen.py` and nothing else. 120 was tried first and fought the other rules. Several short sentences sharing a line is exactly the compactness asked for above, and it passes 120 easily, so the cap fired on 4727 lines already inside the word budget. What a character count catches that a word count cannot is the paragraph living on one line, and those run from 320 to 1057 characters.

**One rule set, both kinds of file.** An implementation file follows exactly the limits above. What differs is that its findings are warnings where a header's are errors, because a header's comments are the published page. Three of these were once scoped to headers alone, and each reason turned out to be false. A `.cpp` carries a file lead with its own `@moreinfo` appendix, so "the deep dive goes after `@moreinfo`" names a place that exists there too. A `.cpp` declares classes, 238 of them here. And a header is read as source far more often than as a rendered page, so its line width reaches a reader exactly as an implementation file's does. Two columns of limits drift apart, and the deduction is what keeps them honest.

The first five cut and the last adds, deliberately: the result is a short line on everything rather than an essay on a few things. The word limit counts a SENTENCE rather than a line, because the no-wrap rule makes a line a paragraph. Three short sentences on one line are correct, and one rambling sentence is not. Thirty is where a machine stops a commit, while twenty above is what to aim for when writing. Measured over the tree, a cap of 20 fired on the median sentence and 30 leaves it alone. Vale carries the same 30 as a suggestion, so one number means one thing in both tools.

**The appendix budget counts a SECTION, not the whole appendix.** A single total punishes a file for having several distinct topics. The cheapest way to satisfy one is to delete a section rather than tighten the prose. Ten lines per `## ` section asks each one to be disciplined and lets a file carry as many as it genuinely has. That is what an implementation file needs: the platform backends each hold a handful of separately diagnosed findings, and one shared budget could only force them out of the tree. A fenced block does not count, the same exemption the no-wrap rule makes. A protocol listing is as long as the thing it describes, and counting its lines would ask an author to delete wire format to fit a prose budget.

**No hard wrap in a comment, for the reason markdown gives.** Let the editor soft-wrap, so a one-word edit is a one-word diff rather than a reflowed paragraph. The one-line budget already forbids this on a header's member or code comment. So the rule bites where a block is allowed to be multi-line. That is the class comment, the `@moreinfo` appendix, and any multi-line run in an implementation file. A list item, a heading, a table row and a fenced block are structure rather than a wrapped sentence, and each is left alone. This one holds everywhere, because its reason is the diff rather than the page. A split sentence reflows every line it spans when a word changes, which costs a reviewer the same in either file. One sentence per line is as available in C++ as anywhere; the line simply runs long.

**Use `//` sparingly.** A comment restating what the code does is a naming failure, and the fix is a better name rather than a better sentence. What survives is the WHY a reader cannot recover from the code.

**`//` carries the same one-line budget as `///` in a header, and that is what makes the `///` cap mean anything.** Without it the one-line rule moves text rather than removing it. A fifty-line member comment re-spelled as `//` satisfies every other rule and leaves the file exactly as long. One line is room to say why; past that the reasoning belongs after `@moreinfo`, or on the module's page where a reader will find it. The `//` block above the first class is exempt, being the non-Doxygen sibling of the class comment.

**An implementation file is asked for no member docs.** This one is a detector limit rather than a rule difference. "Public" is read from the shape of a declaration, which cannot see an anonymous namespace or a function body. In a header that approximation holds, because a header's declarations mostly are the public API and each becomes a row on a generated page. In a `.cpp` it matched the fields of file-local structs and plain locals, asking a variable named `got` for a doc comment, so the rule stops at the header. A field that needs explaining carries a trailing `//`, which is what those already did.

**Depth is homed rather than forbidden.** One line is room to say why. Past that the reasoning belongs in an `@moreinfo` appendix, with an `@xref` back from the line that raised the question. Both kinds of file have one. A header's sits on its class or file lead, and an implementation file's on its own file lead, which 212 headers already carry. A larger cap for implementation files was tried and removed, because it homed depth inline. The reasoning then sits beside one call rather than where a reader goes looking for it.

**A finding in a header is an error; one in an implementation file is a warning, with one rule in each direction.** No-hard-wrap blocks in both kinds of file: the tree is at zero findings, so there is nothing left to stage. The line-length cap warns in both, because it is new and its findings are lines nobody wrote wrongly. A staged rule joins the others as the tree meets it. A header's comments are the published page, so a defect there ships, while a `.cpp` publishes nothing and its comments are a note to the next reader. Both are counted and both are reported, because a warning nobody sees is a warning nobody fixes. The split stages the sweep rather than ranking the two kinds of comment, so it goes and everything blocks once the warning column reaches zero. That is how Vale's own config promotes a page to error as the sweep finishes it.

Enforced by [`check_docgen.py`](../moondeck/check/check_docgen.py) over every header under `src/`, the vendored ones excepted.


### Writing a `///` that generates correctly

These are traps, not style: each one silently loses content from the generated page.

- **`///`, not `//`.** Only `///` generates. A `//` comment is invisible on the technical page.
- **The class `///` sits directly above `class X`**, inside the namespace. Separated by includes or the namespace open, Doxygen drops it and the page loses its description.
- **Put detail on the member it describes**, not in the class comment. Every public control and method gets its own `///` leading with one sentence, which is what the generated page shows as its summary.
- **Deep dives go after `@moreinfo`** at the end of the class block, and a post-process moves them below the member lists. The budget above applies: this is not an unbounded appendix.
- **No relative `.md` links.** Doxygen keeps only `http(s)://` links. For an in-page link use `@xref{anchor|label}`, which renders to a link at a heading on the same generated page. The anchor is that heading's slug: lowercased, punctuation dropped, spaces to hyphens. `ParallelLedDriver.h` is the worked example, pointing its vocabulary sentence at its own appendix:

    ```cpp
    /// The encode is a fused correct and transpose, per row. Vocabulary: strand, lane, slot, row, under
    /// @xref{terminology|More info → Terminology}.
    ///
    /// @moreinfo
    ///
    /// ## Terminology
    ```

    A renamed heading leaves the anchor pointing at nothing, and the generator drops the dead link rather than failing. So `check_docgen` verifies that every `@xref` names a heading in the same file. The reverse is not a rule: a `@moreinfo` section is an appendix a reader scrolls to, and most are reached that way rather than through a link.

    **A section written to hold a comment's depth carries an `@xref` back from that comment.** Moving the reasoning out is what keeps the code readable; the anchor is what keeps it findable. The reader at the line that raises the question gets a link to the answer. Where no single line owns the section, the appendix stands alone and the reverse rule above applies.
- **Wrap any `<tag>` in backticks**, or it renders as a live element and swallows the page.
- **Write "such as", not "e.g."** The brief ends at the first period.
- **A weasel that CONTRASTS is information.** "bytes actually allocated" against requested, "the factor actually in use" against configured: the word carries the distinction the comment exists to draw. Vale flags the word rather than the use, which is why its Weasel rule is a suggestion.
- **A `"\u2014"` string literal is not prose.** The dash a control shows for an unset value is UI text: converting it changes what the device displays. Vale flags it, and it stays.
- **`@xref`, `@card` and `@moreinfo` are ours, and Doxygen must not know them.** Each survives as plain text precisely because it is not a Doxygen command, and a post-process turns it into a link, an image or a section afterward. Declaring them as `ALIASES` makes Doxygen consume them and the post-process finds nothing left: every card image, cross-reference and More info section goes. This is also why the marker is `@xref` rather than `@ref`, which Doxygen does own.
- **Doxygen's own warnings stay off.** `WARN_IF_DOC_ERROR` reports only the three commands above, so on this codebase it is noise by construction rather than a signal being silenced.
