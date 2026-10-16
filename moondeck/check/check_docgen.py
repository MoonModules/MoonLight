#!/usr/bin/env -S uv run --script
# /// script
# dependencies = ["markdown", "libclang"]
# ///
# `libclang` because the help file the app reads is generated from the headers, and this check
# regenerates it to prove the committed one matches.
# `markdown` because the docs hooks it imports slug a heading with Python-Markdown's own function.
"""The documentation the build GENERATES, held to the standards that describe it.

The header is the one source: its `///` comments are the technical reference, read in place, and `gen_help` compiles them into help.json, which the app's help and the docs site's card decks both read. So this check holds the header comments to the comment rules, and the help they compile to to the help rules: a line for every module and control, the app's word caps, and a card that leads with its picture. Prose is NOT here: em-dashes, spelling and weasel words are Vale's.

Pinned by test/python/test_check_docgen.py. A check like this fails silently when it
breaks: a regex that stops matching prints a clean run, which reads exactly like a tree
with nothing wrong in it. So every rule here is tested firing on a page built to break it,
not only staying quiet on one that obeys.

    uv run moondeck/check/check_docgen.py
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

# A card's control line is read in a narrow column beside its picture, so one control that runs to a paragraph is what makes it unreadable. The visual catalogs are tighter, the GIF carrying what the words would.
MAX_CONTROL = 100         # characters of any ONE control line, its name included
MAX_CONTROL_VISUAL = 80   # ... on the visual catalogs
# A control's line says what it does. Its default is the reset button's and its range the slider's, so a line stating either restates the UI, and drifts from the code when either changes. Words in backticks are names, such as an option called `default`, and are not read.
_RESTATES_RE = re.compile(r"\bdefaults?\b|(?<![\w.])-?\d+(?:\.\d+)?\s*(?:–|-|to)\s*-?\d+|\bup to \S*\d", re.IGNORECASE)

# Every card carries an image. Effects, modifiers and layouts show MOTION, so theirs is a .gif; the rest are cards and controls, sharper and smaller as a .png. From the hook, the one home that screenshot_modules shares.
import mkdocs_hooks as _hooks  # noqa: E402
ANIMATED_PAGES = _hooks.ANIMATED_PAGES
PREVIEWLESS_PAGES = _hooks.PREVIEWLESS_PAGES

# ---------------------------------------------------------------------------
# The OTHER generated surface: the `///` comments that become the technical pages.
#
# A card is read across a row; a member comment is read beside the thing it describes,
# and the generated page shows its first sentence as the summary. So the budget is one
# line: cut first, and only a fact neither the code nor the spec says goes after `@technical`.
# The word cap is 30: a sentence is one thought and past twenty it is
# usually two, but that is writing advice, and a gate stops a commit only on a real
# ramble. Measured over the tree, 20 fired on the median sentence and 30 leaves the
# outliers alone. Vale's SentenceLength carries the same 30 as a suggestion.
# EVERY C++ source in the repository, not a list of directories inside one of them. A list is a
# tolerance wearing different clothes: each directory it omits is silently exempt, and nobody
# notices a new one appearing. `src` is the bulk; the rest are the entry points, the ISA generator
# and the tests, which sat outside it only by build-system convention.
#
# `test` is in scope even though nothing generates a page from it: these rules are about how the
# code READS, which is why a test's comments are held to them like any other source we own.
HEADER_ROOTS = ("src", "test", "esp32/main", "moonbase/main", "moondeck/moonlive", "teensy")
# `.cpp` too: a rule that stopped at the header was a tolerance the file extension decided, and
# the same `///` and `//` comments live on both sides of a split. A `.cpp` gets no generated page,
# so only the comment-shape rules reach it; the page rules key off the header that names it.
HEADER_SUFFIXES = ("*.h", "*.cpp")
# Upstream code, vendored rather than written here, spelled exactly as check_prose.py spells it.
# `test/doctest.h` is the single-header doctest release: vendored the same way, just not under a
# directory named for it, and holding a hundred findings nobody here will ever fix.
HEADER_EXEMPT = ("src/platform/desktop/vendor/", "src/platform/esp32/vendor/", "src/ui/vendor/", "test/doctest.h")
MAX_CLASS_DOC = 10      # lines of `///` directly above `class X`
MAX_TECHNICAL_SECTION = 10  # lines of ONE `## ` section inside the `@technical` appendix
MAX_MEMBER_DOC = 1      # lines of a `///` run that is NOT a class comment
MAX_DOC_WORDS = 30      # words in one sentence of a comment, `///` or `//`
# Characters of one `//` line, whatever it holds. The word budget governs a sentence and the
# no-wrap rule keeps a sentence whole, so neither bounds a line carrying several of them: without
# this a comment stays legal while running off the side of the screen. Related sentences share a
# line up to here, which is what keeps a dense comment compact instead of fanning out one line per
# sentence.
#
# Every file, because the limit is about reading SOURCE and a header is read as source far more
# often than as a rendered page. It was briefly implementation-only, on the argument that the
# browser wraps a generated paragraph anyway, which ignores the audience doing the work.
# 250 for now, on the way down. One number, so tightening it is this line and nothing else.
# 120 was tried first and punished the compactness the other rules ask for: several short
# sentences sharing a line is good writing and runs past 120 easily, so the cap fired on 4727
# lines whose sentences were each already inside the word budget. At 250 it catches the runaway
# a reader actually struggles with, the 320-to-1057-character paragraph living on one line.
MAX_COMMENT_LINE_CHARS = 250
# The same budget over a whole `///` RUN, so the line cap cannot be satisfied by chopping. A run
# is measured in lines and each line in characters, and neither catches the move that splits one
# over-long line into two half-long ones: the text is identical and every rule passes. Multiplying
# the line cap by the run's line budget states the real limit, which is how much a reader is asked
# to take in before the next declaration.
MAX_RUN_CHARS = MAX_CLASS_DOC * MAX_COMMENT_LINE_CHARS
# A LEAD's line, which is wider. A lead is the summary that opens a header or a class, so one of
# its lines carries what the whole file is for: what the thing is, which hardware it runs on, where
# the shared body lives, what the wire format is. Held to the member cap, the driver headers each
# lost about a third of that, and what went was content rather than padding. The run budget above
# still bounds the lead as a whole, so the wider line buys density rather than sprawl.
MAX_LEAD_LINE_CHARS = 400
# The prefixes a LINE-length finding carries, so `_blocks` can recognize the one rule that warns
# even in a header. Matching on the reason keeps the exception in ONE place rather than making
# every rule pass its own severity. Prefix AND unit: "chars >" alone also matched the run rule
# (`lead N chars > M`), and the prefix alone also matched the word budget (`comment line N words`),
# each a differently motivated rule silently demoted.
_LINE_LENGTH_RULES = ("doc line ", "comment line ")
# Staged as the line-length cap is: a new rule over the sections written before it, so it counts and ratchets rather than stopping commits.
_UNREFERENCED_RULE = "unreferenced appendix section"
# The one rule staged the OTHER way: an implementation file's hard wrap blocks too. The tree is at
# zero, so nothing is held hostage, and a split sentence reflows every line it spans on the next
# word change, which costs a reviewer the same in a `.cpp` as in a header.
_HARD_WRAP_RULE = "hard wrap"
_STALE_HELP_RULE = "help.json stale"
_RESTATES_RULE = "control restates the UI"
# A control's longer help, its `@guide` section, adds what its line cannot hold: what each option does, a consequence, a reason. Words shared with the line show a repeat mechanically.
_GUIDE_REPEATS_RULE = "guide repeats its control"
# The app's help is read at the card with a setting in hand, so each piece fits a glance: a control's longer help shows inline under the control, a guide section opens under the arrow, and the whole is what one tap shows. Tables, steps and reasons belong on a how-to page the guide links.
_HELP_SIZE_RULE = "app help too long"
# The module's guide is its general information, so a control explained there is in the wrong place: its member carries it.
_GUIDE_NAMES_CONTROL_RULE = "module guide explains a control"
# The app shows a module's line under its title and a control's line under its field, so either missing leaves the user a blank.
_HELP_LINE_RULE = "help line missing"
# A tag emoji is a filter chip, so one only a single module or script carries groups nothing.
_TAG_GROUP_RULE = "tag emoji on one module"
# A declaration's comment is its `///`, which the help, the docs and the IDE hover read; a `//` above it reaches none of them, so a note there is either the declaration's doc or a line of the body.
_NOTE_ON_DECL_RULE = "// on a declaration"
MAX_MORE_WORDS = 40      # a control's longer help, its `@guide` section titled with the control
MAX_GUIDE_WORDS = 60     # any other `@guide` section
MAX_HELP_WORDS = 250     # the summary, the guide and every control's longer help together
# A paragraph spread over several lines: one topic is one paragraph and one line, cut to fit the cap rather than split. Staged as the line cap is, a new rule over comments written before it.
_SPLIT_PARAGRAPH_RULE = "split paragraph"
# One line, in every file. Depth is not forbidden, it is HOMED: a header carries it in an
# `@technical` appendix (212 do) and an implementation file carries it in the same appendix on its
# own file lead. A cap of 8 in a `.cpp` was an attempt to give depth a home inline, which leaves
# the reasoning beside one call rather than where a reader looks for it.
MAX_CODE_COMMENT = 1


def _generates_a_page(rel: str) -> bool:
    """Whether this file becomes a generated documentation page.

    A header does; an implementation file does not. THE REASON, not the extension, is what the
    three rules below actually test: a page is what a member doc becomes a row on, and what a
    line budget keeps readable. Spelling it `rel.endswith(".cpp")` at each site stated the
    syntactic fact three times and left the semantic one unwritten, so a future `.hpp` or an
    excluded vendor header would be judged by a rule whose reason does not apply to it.
    """
    return not rel.endswith(".cpp")

def _blocks(key: str, why: str = "") -> bool:
    """Whether a finding fails the gate or annotates it.

    The same split `_generates_a_page` draws, carried through to severity. A header's comments
    ARE the published page, so a finding there is a documentation defect and blocks. An
    implementation file publishes nothing: its comments are a note to the next reader, and an
    over-long one is worth fixing without being worth stopping a commit for.

    Keeping both in one report is the point. A warning that vanishes is a warning nobody fixes,
    so the count stays visible per area and the sweep still closes it; what changes is that the
    remaining implementation-file work cannot hold a header's commit hostage.

    TEMPORARY, and it names its own removal: this is a staging device for the sweep, not a claim
    that an implementation comment matters less. When the `.cpp` side reaches zero this function
    goes and every finding blocks, the same way `.vale.ini` promotes a page to error once the
    sweep has finished it and loses its per-page section when the last page lands.

    ONE rule is staged in each direction. The line-length cap warns in a header too: every other
    rule describes how this tree is already written, so a finding is a defect against a standard
    the file was written under, while the cap is new and erroring on 820 lines nobody wrote
    wrongly would stop commits over a target. And NO-HARD-WRAP blocks in both kinds of file: the
    tree meets it, at zero findings, so the staging has nothing left to stage. The reason holds
    everywhere too, being the diff rather than the page, and a rule met everywhere is a rule that
    should hold everywhere.
    """
    if why.startswith((_HARD_WRAP_RULE, _STALE_HELP_RULE)):
        return True
    if why.startswith(_LINE_LENGTH_RULES) and "chars >" in why:
        return False
    if why.startswith((_UNREFERENCED_RULE, _SPLIT_PARAGRAPH_RULE)):
        return False
    return _generates_a_page(key.partition("::")[0])


# No hard wrap: a sentence continued on the next `///` line. The one-line budget above already
# forbids this on a header's member or code comment, so it bites where a block is ALLOWED to be
# multi-line: the class comment, the `@technical` appendix, and any multi-line run in a `.cpp`.
# Same reason markdown forbids it: let the editor soft-wrap, so a one-word edit is a one-word
# diff rather than a reflowed paragraph.
_WRAP_SKIP = ("##", "@", "-", "*", "|", "```", ">")
# A NUMBERED item is a bullet that happens to start with a digit, so it is structure exactly as
# `-` is. Left out, an enumerated spec read as prose being wrapped and the joiner fused a five-item
# list onto one 299-character line, which is the layout carrying the meaning destroyed to satisfy
# a rule about sentences. Matched rather than prefixed because the marker is `1.`, not one string.
_LIST_ITEM_RE = re.compile(r"^\d+[.)]\s")
# An INDENTED line inside a comment is a block, not prose: a wire table, a listing, a diagram.
# Joining one destroys the layout that carries its meaning, and a reference table read as a
# sentence is the worst of both. Four spaces after the marker is the markdown convention.
_WRAP_INDENT = 4



def _is_structural(body: str) -> bool:
    """Whether a comment line is layout rather than prose: a list item, a table row, a fence.

    Structure is measured by what it shows, so its line count is not a budget to meet. The same
    set the no-wrap rule skips, plus an indented line, which is a block by the markdown rule the
    standards already name.
    """
    b = body.strip()
    if not b:
        return True                     # a blank `//` line separates paragraphs
    return (b.startswith(_WRAP_SKIP) or bool(_LIST_ITEM_RE.match(b))
            or body.startswith(" " * _WRAP_INDENT))




# Every rule this check emits, longest first so "details table cell" wins over
# "details table". The FULL name, not a first word: "no image" and "one control" each
# truncate to a word that names nothing, and the two details-table rules share theirs.
_RULE_NAMES = (
    "public function has no", "public variable has no",
    "file opens with //", "file lead missing", "@technical on a member", "@xref",
    "code comment", "comment line", "doc line", "lead",
    "unreferenced appendix section", "appendix section",
    "class comment", "member comment", "no image", "image is", "one control",
    "doc sentence", "hard wrap", "split paragraph", _STALE_HELP_RULE,
    _RESTATES_RULE, _GUIDE_REPEATS_RULE, _HELP_SIZE_RULE, _GUIDE_NAMES_CONTROL_RULE, _HELP_LINE_RULE,
    _TAG_GROUP_RULE, _NOTE_ON_DECL_RULE,
)


# A rule's key is a prefix of the message, which is stable but reads as a fragment in prose
# ("31 public function has no"). The report says what a reader would say out loud. Keys absent
# here fall through to themselves, so a new rule needs no edit until its wording is awkward.
_RULE_LABELS = {
    "code comment": "multi-line comment blocks",
    "hard wrap": "hard wraps",
    "split paragraph": "split paragraphs",
    "public function has no": "undocumented functions",
    "public variable has no": "undocumented variables",
    "member comment": "member deep dives",
    "file opens with //": "files opening with //",
    "file lead missing": "missing file leads",
    "class comment": "over-long class comments",
    "doc sentence": "over-long sentences",
    "comment line": "over-wide comment lines",
    "doc line": "over-wide doc lines",
    "lead": "over-long leads",
    "appendix section": "over-long appendix sections",
    "unreferenced appendix section": "appendix sections nothing refers to",
    "@technical on a member": "@technical on a member",
    "@xref": "unresolved @xref",
    "no image": "cards with no image",
    "image is": "wrong image format",
    "one control": "cards listing one control",
    _RESTATES_RULE: "controls restating a default or range",
    _GUIDE_REPEATS_RULE: "control guides repeating their line",
    _HELP_SIZE_RULE: "app help over its word caps",
    _GUIDE_NAMES_CONTROL_RULE: "controls explained in the module guide",
    _HELP_LINE_RULE: "missing help lines",
    _TAG_GROUP_RULE: "tag emoji on one module",
    _NOTE_ON_DECL_RULE: "// notes on declarations",
}


def _rule_label(key: str) -> str:
    """The human name for a rule key, for prose in the report."""
    return _RULE_LABELS.get(key, key)


def _rule_name(reason: str) -> str:
    """Which rule a finding reports, as a stable key for the baseline and the report."""
    head = reason.split(":")[0]
    for name in _RULE_NAMES:
        if head.startswith(name):
            return name
    return head.split(" ")[0]


def _headers():
    """Every source file the rules cover, repo-relative.

    RECURSIVE: the old per-directory glob was flat, so a header one level down was never
    read and its findings never counted. Widening the root without this would have reported
    a clean run over an empty set, which is the failure this check exists to prevent.
    """
    seen = set()
    tracked = _tracked_files()
    for root in HEADER_ROOTS:
        for suffix in HEADER_SUFFIXES:
            for p in sorted((ROOT / root).rglob(suffix)):
                rel = p.relative_to(ROOT)
                if rel.as_posix().startswith(HEADER_EXEMPT) or rel in seen:
                    continue
                # Tracked files only, as repo_health measures: a header a build generates (build_info.h, ui_embedded.h) moved the counts with whatever was built last.
                if tracked is not None and rel.as_posix() not in tracked:
                    continue
                seen.add(rel)
                yield rel


def _tracked_files():
    """The repo's tracked paths, or None outside a git checkout, where every file on disk counts."""
    import subprocess
    r = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True)
    return set(r.stdout.split("\n")) if r.returncode == 0 else None


def _appendix_lines(text: str) -> int:
    """How many `///` lines follow an `@technical` in one file: the depth its comments hold outside the code."""
    n, inside = 0, False
    for line in text.split("\n"):
        s = line.strip()
        if not s.startswith("///") or "@guide" in s:
            inside = False
        elif inside:
            n += 1
        elif "@technical" in s:   # anywhere on the line, as _header_rules reads it
            inside = True
    return n


def _appendix_total() -> int:
    """The tree's appendix size, ratcheted so a cleanup cannot pass by moving comments rather than cutting them."""
    return sum(_appendix_lines((ROOT / rel).read_text(encoding="utf-8")) for rel in _headers())


def _appendix_growth() -> list:
    """The changed files whose appendix grew against HEAD, so a rise names where it came from."""
    import subprocess
    r = subprocess.run(["git", "diff", "--name-only", "HEAD"], cwd=ROOT, capture_output=True, text=True)
    covered = {rel.as_posix() for rel in _headers()}
    out = []
    for rel in r.stdout.split():
        if rel not in covered:
            continue
        old = subprocess.run(["git", "show", f"HEAD:{rel}"], cwd=ROOT, capture_output=True, text=True)
        was = _appendix_lines(old.stdout) if old.returncode == 0 else 0
        now = _appendix_lines((ROOT / rel).read_text(encoding="utf-8"))
        if now > was:
            out.append((rel, was, now))
    return out


def _sentences(text: str):
    """The sentences in one comment line, for the word cap.

    A full stop, question mark or exclamation followed by a space ends one. An abbreviation
    ("e.g.", "i.e.") and a decimal ("0.1 bpp") carry a full stop that ends nothing, so a split
    there would report two short sentences where there is one long one, which is the opposite
    of what the cap is for. Both are rare in a comment and neither can lengthen a sentence,
    so the split is deliberately simple: it can merge, never wrongly divide.
    """
    parts = re.split(r"(?<=[.!?])\s+(?=[A-Z`\"'(\[])", text)
    return [p for p in (x.strip() for x in parts) if p]


def _prose_pair(lines: list, k: int, end: int, start: int):
    """The stripped bodies of lines `k` and `k + 1` when both are prose of one comment block, or None.

    A list item, a heading, a table row, a fence or an indented block is structure rather than
    prose, so a pair touching one is no continuation.
    """
    if k + 1 >= end:
        return None
    # INSIDE a fenced block nothing is prose: a fence's lines are verbatim, and a wire table read
    # as prose is flagged on every row.
    fenced = False
    for j in range(start, k):
        if re.sub(r"^\s*//+\s*", "", lines[j]).strip().startswith("```"):
            fenced = not fenced
    if fenced:
        return None
    # `//+` rather than `///`: the rule binds on both markers.
    body = re.sub(r"^\s*//+\s*", "", lines[k]).strip()
    nbody = re.sub(r"^\s*//+\s*", "", lines[k + 1]).strip()
    if not body or not nbody:
        return None
    if (body.startswith(_WRAP_SKIP) or nbody.startswith(_WRAP_SKIP)
            or _LIST_ITEM_RE.match(body) or _LIST_ITEM_RE.match(nbody)):
        return None
    raw = re.sub(r"^\s*//+", "", lines[k])
    nraw = re.sub(r"^\s*//+", "", lines[k + 1])
    if (len(raw) - len(raw.lstrip()) >= _WRAP_INDENT
            or len(nraw) - len(nraw.lstrip()) >= _WRAP_INDENT):
        return None
    return body, nbody


def _ends_sentence(body: str) -> bool:
    # `.`, `!`, `?` only: a colon introduces what follows and a semicolon joins two clauses, as
    # Unicode's sentence-boundary rules, Vale and the standards all read them.
    return body[-1] in ".!?"


def _wrap_rule(rel: str, lines: list, k: int, end: int, start: int = 0):
    """A sentence carried onto the next comment line, or a paragraph spread over several lines.

    A function rather than a loop body, because two callers need it: a member or class run,
    and a file lead. Every file, header or implementation: the reason is the DIFF,
    since a sentence split across lines reflows every one of them when a word changes.

    One topic is one paragraph, and a paragraph is one line, which markdown joins anyway and a
    reader takes as one thought. It is reported once, at its first line, with the fix its length
    allows: joined, or cut to fit the line cap, since splitting it into sentence lines reads as
    separate topics and packing it into line-sized chunks breaks it at arbitrary points.
    """
    pair = _prose_pair(lines, k, end, start)
    if not pair:
        return []
    body, nbody = pair
    # The test is the PREVIOUS line: a sentence that has not ended is carried over, whatever the
    # continuation starts with (a digit, a backtick or an identifier as often as a lowercase word).
    if not _ends_sentence(body):
        return [(f"{rel}::line {k + 1}",
                 "hard wrap: one line per sentence, let the editor soft-wrap")]
    # Reported where the paragraph starts, so a paragraph of five lines is one finding, not four.
    if k > start:
        prev = _prose_pair(lines, k - 1, end, start)
        if prev and _ends_sentence(prev[0]):
            return []
    last, total = k + 1, len(lines[k].rstrip()) + 1 + len(nbody)
    while True:
        nxt = _prose_pair(lines, last, end, start)
        if not nxt or not _ends_sentence(nxt[0]):
            break
        last += 1
        total += 1 + len(nxt[1])
    fix = ("join it into one line" if total <= MAX_COMMENT_LINE_CHARS
           else f"cut it to fit one line of {MAX_COMMENT_LINE_CHARS} chars")
    return [(f"{rel}::line {k + 1}",
             f"split paragraph: lines {k + 1}-{last + 1} are one paragraph of {total} chars: {fix}")]


def _doc_runs(lines):
    """Each run of consecutive `///` lines, as (start, end, next_code_line)."""
    i = 0
    while i < len(lines):
        if lines[i].lstrip().startswith("///"):
            j = i
            while j < len(lines) and lines[j].lstrip().startswith("///"):
                j += 1
            nxt = lines[j].lstrip() if j < len(lines) else ""
            yield i, j, nxt
            i = j
        else:
            i += 1


def _declared_key(decl: str, start: int) -> str:
    """The baseline key for a declaration: its name, plus its arity where it takes one.

    Two overloads share a name, so keying on the name alone put both under one entry and
    the baseline then tolerated whichever the check happened to measure first (the tree
    has `PreviewDriver::emit` twice today). The parameter COUNT separates them without
    depending on how a signature is spelled, which a normalized type list would.
    """
    name = _declared_name(decl, start)
    m = re.search(r"\(([^)]*)\)", decl)
    if not m or "(" not in decl:
        return name
    args = m.group(1).strip()
    n = 0 if not args or args == "void" else args.count(",") + 1
    return f"{name}/{n}"


# A keyword reads as an identifier before `(`, so `if (ok) {` keys as `if` and a literal
# initializer keys as `true`. Neither locates the comment a finding is about, and `if/1`
# alone accounted for 56 of them; the line number is what a reader can act on.
_NOT_A_NAME = frozenset((
    "if", "for", "while", "switch", "catch", "return", "else", "do",
    "true", "false", "nullptr", "break", "continue", "sizeof", "case", "default",
))


def _declared_name(decl: str, start: int) -> str:
    """The NAME a declaration introduces, for a stable baseline key.

    Splitting on "(" and taking the last token yields `true;` from `bool ok = true;` and
    `Slot>` from a template line, so several members collapse onto one key: the baseline
    then records one member's size and reads every other as having grown. The identifier
    before `(`, `=` or `;` is the name, and a line number is the fallback where no
    declaration follows (a file-level comment block).
    """
    # The name is the identifier that PRECEDES the initializer, so `bool ok = true;` is
    # `ok` rather than `true`. Searching for the first identifier before any of `( = ;`
    # matched the initializer's value and collapsed nine members onto `::true`.
    m = re.match(r"^(?:class|struct)\s+(\w+)", decl)
    if m:
        return m.group(1)
    # The identifier immediately BEFORE the initializer, parameter list or terminator.
    # A greedy prefix swallows it and captures the initializer instead, so anchor on the
    # last identifier that precedes one of `( = ; {`.
    m = re.match(r"^[\w:<>,\s\*&\[\]]*?\b(\w+)\s*(?:\(|=|;|\{)", decl)
    if m and m.group(1) not in _NOT_A_NAME:
        return m.group(1)
    return f"line {start + 1}"


_CLASS_RE = re.compile(r"^(class|struct)\s+\w+")
# A forward declaration names a type defined elsewhere, so it documents nothing and is not a lead.
_FWD_DECL_RE = re.compile(r"^(class|struct)\s+\w+\s*;")
# A free constant or function at file scope. The trailing form accepts a signature that ENDS at the parameter list, because an Allman brace sits on the next line.
_TOP_DECL_RE = re.compile(r"^(inline|constexpr|extern|template|using|typedef)\b|"
                          r"^[\w:<>,\s\*&]+\s+\w+\s*\([^)]*\)\s*(const)?\s*[;{]?\s*$")
# What may follow a parameter list: the qualifiers, and an attribute macro such as `MM_NONBLOCKING`, which is all capitals.
_FUNC_TAIL = r"(?:\s*(?:const|noexcept|override|final|[A-Z][A-Z0-9_]+)\b)*\s*(?:=\s*(?:0|default|delete)\s*)?"
_FUNC_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*\([^)]*\)" + _FUNC_TAIL + r"[;{]")
# A constructor and a destructor have no return type, so the form above misses both.
_SPECIAL_FUNC_RE = re.compile(r"^(?:explicit\s+|virtual\s+)*~?(\w+)\s*\([^)]*\)" + _FUNC_TAIL + r"[;{:]")
_VAR_RE = re.compile(r"^[\w:<>,\s\*&]+\s+(\w+)\s*(=[^;]+)?;")


def _file_comment_end(lines) -> int:
    """Where the file comment stops, as a line index.

    The file comment is the `//` block at the top, before any code. Include lines and pragmas may
    precede it and blank lines separate its paragraphs; the first line of actual content ends it,
    an `#include` included. A file with no leading comment answers 0, so nothing is exempt.

    A BLANK LINE does not end it either, which is the whole reason this is a scan rather than a
    single run: a lead is written in paragraphs, and stopping at the first blank exempted only
    the opening one. The rest then read as prose sitting on the next declaration and were
    reported as essays in the wrong place, when they are the file's documentation in the only
    place a `.cpp` has for it.
    """
    started = False
    end = 0
    for i, ln in enumerate(lines):
        st = ln.strip()
        if st.startswith("//") and not st.startswith("///"):
            started = True
            end = i + 1            # the block reaches at least this far
            continue
        if not st:
            continue               # a blank line separates the lead's paragraphs
        if not started and st.startswith(("#", "/*", "*")):
            continue               # includes and pragmas may still precede it
        break                      # anything else, an include included, ends the lead
    return end


def _slugify_heading(text: str) -> str:
    """A heading's MkDocs anchor: lowercased, punctuation dropped, spaces to hyphens."""
    text = re.sub(r"[`*]", "", text).strip().lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"[\s_]+", "-", text)


def _xref_resolves_rule(rel: str, lines: list[str], out: list) -> None:
    """Every `@xref{anchor}` names a heading that exists in this file.

    The marker renders to a same-page `[label](#anchor)`. A renamed heading leaves the anchor
    pointing at nothing, and the generator's final pass strips the dead link rather than failing,
    so the cross-reference disappears with no warning. This is what makes that visible.

    The name is matched LOOSELY and checked strictly, which is the whole point. The generator
    resolves `[a-z0-9-]+`, so an underscore in a name is not a link at all: it survives Doxygen
    untouched and the reader sees a raw `@xref{...}` mid-sentence. Matching the same narrow class
    here made the rule silent on exactly the names that cannot work, and it reported a clean run
    over 21 of them. So anything `@xref{...}`-shaped is a claim to check.
    """
    headings = {_slugify_heading(m.group(1))
                for ln in lines
                for m in [re.match(r"\s*///\s*#{1,6}\s+(.+)$", ln)] if m}
    for i, ln in enumerate(lines):
        for m in re.finditer(r"@xref\{([^}|]+)(?:\\?\|[^}]*)?\}", ln):
            # The label separator may be escaped as `\|`, which the generator accepts, so the
            # backslash lands in the anchor here and would fail the slug test on its own.
            anchor = m.group(1).strip().rstrip("\\")
            if not re.fullmatch(r"[a-z0-9-]+", anchor):
                out.append((f"{rel}::line {i + 1}",
                            f"@xref{{{anchor}}} is not a heading slug, so it renders as raw text"))
            elif anchor not in headings:
                out.append((f"{rel}::line {i + 1}",
                            f"@xref{{{anchor}}} names no heading in this file"))


def _file_lead_rule(rel: str, lines: list[str], out: list) -> None:
    """A header opens with `///`, so its generated page says what the file is for.

    Two shapes qualify, because two kinds of header exist. A header of free functions or constants
    leads with a `///` block saying what the file is for. A single-class header leads with the class
    comment, which IS the file's documentation. Anything else, including the `//` block
    four platform headers used to open with, generates nothing: Doxygen reads `//` as a note to the
    next reader of the source, not as documentation.
    """
    for i, ln in enumerate(lines):
        st = ln.strip()
        if not st or st.startswith(("#", "/*", "*")):
            continue
        # A provenance marker is not the file's documentation: an SPDX tag is machine-read, and
        # an `Author:` line credits where the code came from. Both sit above the real lead, so
        # they are skipped rather than counted as one, and the `///` below them generates the page.
        if st.startswith(("// SPDX-", "// Author:")):
            continue
        # The first comment in the file, whatever it is.
        if st.startswith("///"):
            return                      # a `///` lead: the file's own or a class comment, both fine
        if st.startswith("//"):
            # Every file leads with `///`, header or not. A lead is the one comment written for
            # someone who has not read the file yet, and `///` is what carries it onto a page and
            # what lets an `@technical` appendix hang off it: spelled `//` the same words reach
            # nobody but the next person to scroll past them. 216 implementation files still lead
            # with `//`, which is the sweep this states rather than an exemption.
            out.append((f"{rel}::line {i + 1}",
                        "file opens with // : a file leads with /// so it can carry an appendix"))
            return
        # Code before any comment: no lead at all. `namespace`, `using` and a forward declaration
        # are not a file's documentation, so keep looking until something documented turns up.
        if st.startswith(("namespace", "using", "extern")) or _FWD_DECL_RE.match(st):
            continue
        out.append((f"{rel}::line {i + 1}",
                    "file lead missing: a header opens with /// saying what the file is for"))
        return


def _appendix_marker(line: str) -> bool:
    """Whether a line opens an appendix: `@guide`, the sections written for users that the app shows as a module's help, or `@technical`, the ones written for developers. Both end the lead, sit only on a lead and cap each `## ` section; only `@technical` counts toward the ratcheted total, since a guide section is help moved out of a card rather than depth moved out of the code."""
    # `@guide` alone on its line opens the module's guide; on a member it starts the control's longer help, which is no appendix.
    return "@technical" in line or re.fullmatch(r"\s*(///)?\s*@guide\s*", line) is not None


def _technical_placement_rule(rel: str, lines: list[str], out: list) -> None:
    """`@technical` and `@guide` are appendices, and only a lead carries one.

    The file lead and each class or struct comment may have one; a member may not, because a member
    gets a single line and the generated page shows that line as its summary. An `@technical` on a
    member is depth in the wrong place, and relaxing the member budget to allow it put a four-line
    block on every function in a swept header.
    """
    for start, end, nxt in _doc_runs(lines):
        if not any(_appendix_marker(lines[k]) for k in range(start, end)):
            continue
        if start == _lead_start(lines):
            continue                    # the file lead
        if _CLASS_RE.match(nxt):
            continue                    # a class or struct comment
        out.append((f"{rel}::{_declared_key(nxt, start)}",
                    "@technical on a member: the appendix belongs on the file or class lead"))


def _lead_start(lines: list[str]) -> int | None:
    """Where the file's lead sits: the first `///` run, when nothing but includes, namespaces, forward declarations and `//` notes precede it, and no declaration follows it directly. What makes a lead is its position."""
    for start, end, nxt in _doc_runs(lines):
        # A block of tags alone, such as the test inventory's `/// @module`, is metadata rather than the lead.
        if all(re.sub(r"^\s*///\s*", "", lines[k]).startswith("@") for k in range(start, end)):
            continue
        before = [ln.strip() for ln in lines[:start]]
        code = [st for st in before if st and not st.startswith(("#", "//", "/*", "*", "namespace", "using", "extern", "{"))
                and not _FWD_DECL_RE.match(st)]
        # Flush against a declaration, the run is that declaration's comment, as clang reads it.
        return None if code or not (not nxt or nxt.startswith(("#", "{", "namespace", "//"))) else start
    return None


def _header_rules(rel: str, text: str):
    """The comment budget, on one header.

    Counted rather than judged, so the constants at the top of this file ARE the rules and
    this docstring names no number of its own. A class comment and each `@technical` `## `
    section have a line budget; any other `///` run has one line; a sentence has a word
    budget; a `//` run has one line in a header and an essay's worth in a `.cpp`; and every
    public member of a header carries a `///`. The budgets cut and the last one adds, pulling
    against each other so the result is a short line on everything rather than an essay on a
    few things.

    A file also opens with `///`, `@technical` sits only on a lead, and every `@xref` resolves
    to a heading in its own file.

    `//` carries the same budget as `///` IN A HEADER, because without that the `///` cap moves
    text rather than removing it: a fifty-line member comment re-spelled as `//` satisfies
    every other rule and leaves the file exactly as long. An implementation file generates no
    page, so there the line count catches an essay rather than policing a page's shape.
    """
    out = []
    lines = text.split("\n")
    lead = _lead_start(lines)

    for start, end, nxt in _doc_runs(lines):
        n = end - start
        # The file lead documents the FILE, not a member, so it is held to the class budget and splits at @technical the same way; read as a member comment it would get one line.
        if start == lead:
            head = next((k for k in range(start, end) if _appendix_marker(lines[k])), end)
            n = head - start
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::line {start + 1}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
            # And the same budget in CHARACTERS, so the line cap cannot be met by chopping one
            # long line into two shorter ones: the run is what a reader takes in, and splitting
            # it leaves that unchanged.
            chars = sum(len(re.sub(r"^\s*///\s?", "", lines[k])) for k in range(start, head))
            if chars > MAX_RUN_CHARS:
                out.append((f"{rel}::line {start + 1}",
                            f"lead {chars} chars > {MAX_RUN_CHARS}"))
            # The word cap applies here too: the file lead is prose a reader sees.
            for k in range(start, end):
                for sentence in _sentences(re.sub(r"^\s*///\s*", "", lines[k]).strip()):
                    if len(sentence.split()) > MAX_DOC_WORDS:
                        out.append((f"{rel}::line {k + 1}",
                                    f"doc sentence {len(sentence.split())} words > {MAX_DOC_WORDS}"))
                width = len(re.sub(r"^\s*///\s?", "", lines[k]))
                if width > MAX_LEAD_LINE_CHARS:
                    out.append((f"{rel}::line {k + 1}",
                                f"doc line {width} chars > {MAX_LEAD_LINE_CHARS}"))
                # The no-wrap rule binds here too: the file lead is the longest block in the
                # tree, so exempting it exempted the prose most likely to be wrapped.
                out += _wrap_rule(rel, lines, k, end, start)
            continue
        if _CLASS_RE.match(nxt):
            # The LEAD only: the run ends at @technical, whose own lines are the appendix and
            # are measured per section below. Counting both here would put the documented
            # budget (10 lead, then 10 per appendix section) out of reach of any header.
            head = next((k for k in range(start, end) if _appendix_marker(lines[k])), end)
            n = head - start
            if n > MAX_CLASS_DOC:
                out.append((f"{rel}::{nxt.split()[1].rstrip('{:')}",
                            f"class comment {n} lines > {MAX_CLASS_DOC}"))
            # The run cap reaches HERE too. Applied only to the file lead it missed the
            # block it was written for: nine lines of 359 characters is 3231, over the cap, with
            # every line inside the lead cap and every sentence inside the word budget.
            chars = sum(len(re.sub(r"^\s*///\s?", "", lines[k])) for k in range(start, head))
            if chars > MAX_RUN_CHARS:
                out.append((f"{rel}::line {start + 1}",
                            f"lead {chars} chars > {MAX_RUN_CHARS}"))
        else:
            # Both kinds of file: a `.cpp` carries a file lead with its own `@technical` appendix exactly as a header does.
            #
            # Structure discounts here exactly as it does on the `//` cap. Without that, one
            # marker exempted a bulleted list and the other did not, so the same six lines were
            # clean as `//` and a finding as `///`, which is two rule sets wearing one name.
            # A control's `@guide` line is its longer help, Doxygen's detailed description, and rides beside the one line.
            prose = n - sum(1 for k in range(start, start + n)
                            if _is_structural(body := re.sub(r"^\s*///\s?", "", lines[k])) or body.startswith("@guide"))
            if prose > MAX_MEMBER_DOC:
                out.append((f"{rel}::{_declared_key(nxt, start)}",
                            f"member comment {prose} lines > {MAX_MEMBER_DOC}: "
                            f"cut it to one line"))
        for k in range(start, end):
            # PER SENTENCE, not per line. The no-wrap rule makes a line a paragraph, so a line
            # holding three short sentences is correct and a single rambling one is not: counting
            # the line would fail the first and pass nothing the standards care about. The rule
            # reads "a sentence is one thought; past twenty words it is usually two", which is a
            # statement about sentences.
            text_ = re.sub(r"^\s*///\s*", "", lines[k]).strip()
            for sentence in _sentences(text_):
                words = len(sentence.split())
                if words > MAX_DOC_WORDS:
                    out.append((f"{rel}::line {k + 1}",
                                f"doc sentence {words} words > {MAX_DOC_WORDS}"))
            # A LEAD's line is wider than a member's: it carries what the whole type is for,
            # and held to the member cap the driver headers each lost about a third of that.
            cap = MAX_LEAD_LINE_CHARS if _CLASS_RE.match(nxt) else MAX_COMMENT_LINE_CHARS
            # The PROSE, not the marker and the indent. Measuring the raw line held a member
            # nested in a class to a stricter budget than the same words at file scope, which
            # is a rule nobody wrote, and it disagreed with the run cap below that strips both.
            width = len(re.sub(r"^\s*///\s?", "", lines[k]))
            if width > cap:
                out.append((f"{rel}::line {k + 1}",
                            f"doc line {width} chars > {cap}"))
            out += _wrap_rule(rel, lines, k, end, start)

    # A file OPENS with `///`, so the page says what the file is for. The lead is the file's
    # own `///` block in a header of free functions, or the class comment in a single-class
    # header, where the class IS the file. Without one the page is a bare member list: 222 headers
    # were in that state, and four of the six platform headers opened with `//`, which Doxygen
    # drops entirely.
    _file_lead_rule(rel, lines, out)

    # `@technical` is an APPENDIX, and a page has one per documented entity: the file lead and each
    # class or struct comment. A member gets one line (MAX_MEMBER_DOC), so an `@technical` on one is
    # depth in the wrong place: it belongs in the file lead's appendix or on the module's page.
    _technical_placement_rule(rel, lines, out)

    # An `@xref{anchor}` names a heading on the SAME generated page. When the heading is renamed
    # the link resolves to nothing, and `_strip_unresolved_anchor_links` drops it silently: the
    # reader loses the cross-reference and no build says so. Checking it here is what makes the
    # rename visible. The reverse, a section nothing refers to, is `_unreferenced_sections`.
    _xref_resolves_rule(rel, lines, out)

    # PER SECTION, not per appendix. A whole-appendix cap punishes a file for having several
    # distinct topics, and the cheapest way to satisfy it is to delete a section rather than
    # tighten the prose. Capping each `## ` section instead asks every one of them to be
    # disciplined and lets a file carry as many as it genuinely has: the platform implementations
    # each hold a handful of separately-diagnosed findings (a clock-divisor window, a latch-versus-
    # data fault, a ring's competing axes) that one shared budget could only force out of the tree.
    for i, ln in enumerate(lines):
        if not _appendix_marker(ln):
            continue
        section, count, fenced = "(lead)", 0, False
        for m in lines[i + 1:]:
            if not m.lstrip().startswith("///") or _appendix_marker(m):
                break
            text = re.sub(r"^\s*///\s?", "", m).strip()
            # A fenced block is structure rather than prose, the same exemption the no-wrap rule
            # makes: a protocol listing or a table is as long as the thing it describes, and
            # counting its lines asks the author to delete wire format to fit a prose budget.
            if text.startswith("```"):
                fenced = not fenced
                continue
            if fenced:
                continue
            if text.startswith("## "):
                if count > MAX_TECHNICAL_SECTION:
                    out.append((f"{rel}::@technical {section}",
                                f"appendix section {count} lines > {MAX_TECHNICAL_SECTION}"))
                section, count = text[3:].strip(), 0
            elif text:
                count += 1
        if count > MAX_TECHNICAL_SECTION:
            out.append((f"{rel}::@technical {section}",
                        f"appendix section {count} lines > {MAX_TECHNICAL_SECTION}"))

    # `//` carries the same budget as `///` in a HEADER, or the `///` cap only MOVES text:
    # re-spelling a fifty-line member comment as `//` satisfies every other rule and leaves the
    # file the same length, which is what a first pass through these headers produced.
    # An implementation file has no page to protect, so it gets the essay limit instead.
    #
    # The file comment is exempt: the non-Doxygen sibling of the class comment, with no member to
    # sit beside. That is the ONE block at the top, before anything is declared, and it used to be
    # spelled "above the first class". In a header of free functions the first class can be 250
    # lines down (MoonLiveBuiltins_light.h) or absent, which exempted every comment in the file:
    # 1002 runs across 149 files, each sitting on the declaration it documents.
    file_comment_end = _file_comment_end(lines)
    i = 0
    while i < len(lines):
        st = lines[i].strip()
        if st.startswith("//") and not st.startswith("///"):
            j = i
            while j < len(lines) and lines[j].strip().startswith("//") \
                    and not lines[j].strip().startswith("///"):
                j += 1
            cap = MAX_CODE_COMMENT
            # A list, a table or a fenced block is STRUCTURE, and its lines are its layout rather
            # than an over-long run of prose. Counted, an enumerated spec could only satisfy the
            # cap by being fused onto one line, which is the layout destroyed to pass a rule about
            # sentences. The lines that are prose still count, so an essay with a list in it is
            # measured on the essay.
            structural = sum(1 for k in range(i, j)
                             if _is_structural(re.sub(r"^\s*//+\s?", "", lines[k])))
            if j - i - structural > cap and i >= file_comment_end:
                nxt = next((lines[k].strip() for k in range(j, len(lines)) if lines[k].strip()), "")
                out.append((f"{rel}::{_declared_key(nxt, i) if nxt else f'line {i + 1}'}",
                            f"code comment {j - i - structural} lines > {cap}"))
            # The same word budget as a `///` line, and measured the same way: PER SENTENCE.
            # Counting the whole line instead made this rule fight the no-wrap rule above, which
            # asks for one sentence per line: a dense three-sentence comment then had to fan out
            # over three lines to pass, which is longer to read rather than shorter. A line
            # carrying several short sentences is good writing, and one rambling sentence is not.
            for k in range(i, j):
                text_ = re.sub(r"^\s*//+\s*", "", lines[k]).strip()
                for sentence in _sentences(text_):
                    words = len(sentence.split())
                    if words > MAX_DOC_WORDS:
                        out.append((f"{rel}::line {k + 1}",
                                    f"comment line {words} words > {MAX_DOC_WORDS}"))
                width = len(re.sub(r"^\s*//+\s?", "", lines[k]))
                if width > MAX_COMMENT_LINE_CHARS:
                    out.append((f"{rel}::line {k + 1}",
                                f"comment line {width} chars > {MAX_COMMENT_LINE_CHARS}"))
                # And the same no-wrap rule. It used to run on `///` only, which left every `//`
                # block in the tree unmeasured: the budget above says a line is a sentence, and
                # a sentence carried onto the next line breaks that whichever marker spells it.
                out += _wrap_rule(rel, lines, k, j, i)
            i = j
        else:
            i += 1

    # Every public member carries one. A generated page shows a member with no `///` as a
    # bare signature, which tells a reader nothing the declaration did not.
    # Only declarations in the CLASS BODY itself. A `{` opens a function body, and
    # `return true;` or `Preset& p = presets_[count_];` inside one parses as a
    # declaration, so 251 statements were reported as undocumented public members.
    #
    # The depth of a class body is NOT a constant: every header opens `namespace mm`, but
    # a nested struct adds another level (Hub75Slots puts its class at 3 where the others
    # are at 2). So remember WHICH depth the class opened at, rather than counting to a
    # number that is right for most files and wrong for the rest.
    # A STACK, not one depth: a nested type opens its own body and the enclosing class
    # resumes when it closes. Holding a single depth meant a nested struct overwrote the
    # class that contained it, and closing the struct then read as closing the class, so
    # every public member after one escaped this check entirely (a class with a nested
    # type reported nothing at all).
    # LOCAL types are exempt for the reason the private ones are: a struct declared inside a
    # METHOD body is unnameable outside it and reaches no generated page, so asking it for a
    # `///` documents a line no reader sees. The depth says which is which: a type declared in
    # a class body opens one level below it, one declared in a method opens two or more,
    # because the method's own `{` sits between them.
    frames = []                                  # (body_depth, public, local) per open class/struct
    public = False
    depth = 0
    class_depth = None
    in_local = False
    # The depth a FREE function's body opens at, or None outside one. `frames` is empty there,
    # so the depth test below has nothing to compare against and a struct inside `void f() { … }`
    # read as a public member, though it is as unnameable as one inside a method.
    fn_depth = None
    for i, ln in enumerate(lines):
        st = ln.strip()
        opens, closes = ln.count("{"), ln.count("}")
        before = depth
        depth += opens - closes
        if fn_depth is not None and depth < fn_depth:
            fn_depth = None                      # the function body closed
        # A declaration at file scope carries one comment kind too: a free function, a constant, a type.
        if (not frames and fn_depth is None and _generates_a_page(rel) and not st.startswith(("template", "extern"))
                and (_TOP_DECL_RE.match(st) or _CLASS_RE.match(st) or st.startswith("enum "))):
            out += _note_on_declaration(rel, lines, i, st)
        if (not frames and fn_depth is None and opens and not _CLASS_RE.match(st)
                and not st.startswith(("namespace", "extern", "//", "/*", "*", "#"))
                and "(" in st):
            fn_depth = before + 1                # a free function's body opens here
        if _CLASS_RE.match(st) and opens:
            # A NESTED struct inherits the enclosing access: one declared after `private:` is
            # private however it is spelled, and demanding a `///` on its fields asked a file to
            # document what no reader of the generated page can see. Only a top-level struct
            # (nothing open above it) starts public.
            nested = bool(frames)
            # Below the enclosing class body rather than directly in it: a method's body
            # intervenes, so this type is local to that method and reaches no reader.
            local = (bool(frames) and (before + 1 > frames[-1][0] + 1 or frames[-1][2])) \
                or (fn_depth is not None and before >= fn_depth)
            frames.append((before + 1, public, local))
            class_depth = before + 1
            public = st.startswith("struct") and not (nested and not public)
            in_local = local
            continue
        while frames and depth < frames[-1][0]:
            _, public, _ = frames.pop()          # this body closed; the enclosing one resumes
            class_depth = frames[-1][0] if frames else None
            in_local = frames[-1][2] if frames else False
        if st.startswith("public:"):
            public = True
            continue
        if st.startswith(("private:", "protected:")):
            public = False
            continue
        if not st or st.startswith(("//", "/*", "*", "#")):
            continue
        # Any access: a private member's comment shows on hover as a public one's does.
        if (before == class_depth and not in_local and _generates_a_page(rel)
                and not st.startswith(("return", "if", "for", "while", "}", "friend "))):
            out += _note_on_declaration(rel, lines, i, st)
        if not public:
            continue
        # `before` is the depth the line STARTS at: a member sits exactly in the body.
        # `friend` grants access to another type; it declares no member and reaches no
        # generated page, so asking it for a `///` documents a line no reader sees.
        if (before != class_depth or in_local
                or st.startswith(("return", "if", "for", "while", "}", "friend "))):
            continue
        # Past a `template <...>` line: doxygen attaches a comment to the declaration, and the
        # template header sits between the two. Reading only the line above asked a templated
        # member for a `///` it already had, and the answer was a second one wedged below the
        # template header, which doxygen drops and a reader sees twice.
        j = i - 1
        while j >= 0 and lines[j].lstrip().startswith("template"):
            j -= 1
        prev = lines[j].lstrip() if j >= 0 else ""
        if prev.startswith("///") or "///" in ln:
            continue
        # A test hook is named `...ForTest`, which says what it is, so a `///` on it adds nothing.
        if _declared_name(st, i).endswith("ForTest") or re.match(r"~\w+ForTest\s*\(", st):
            continue
        # Header-only, and this is the ONE place that is a detector limit rather than a rule
        # difference. Everything else applies to both kinds of file; "public" here is read from a
        # declaration's SHAPE, which cannot see `namespace {` or a function body, and a `.cpp` is
        # mostly bodies. Turned on there it names `true`, `false`, `nullptr` and `break` as public
        # variables wanting a doc comment: 5599 findings, none of them real.
        if (_FUNC_RE.match(st) or _SPECIAL_FUNC_RE.match(st)) and _generates_a_page(rel):
            # In a `.cpp` this matched the constructor or destructor of a file-local RAII struct:
            # `~WinsockInit`, `Lock`, `ParkGuard`. None is public API, and each is already explained
            # by the struct's own comment above it.
            out.append((f"{rel}::{_declared_key(st, i)}", "public function has no ///"))
        elif "(" not in st and _VAR_RE.match(st) and _generates_a_page(rel):
            # "Public" here is the declaration's SHAPE, which cannot see `namespace {` or a
            # function body. In a `.cpp` that matched fields of file-local structs and plain
            # locals, naming `got`, `ok` and `cur` and asking a local variable for a doc comment.
            out.append((f"{rel}::{_declared_name(st, i)}", "public variable has no ///"))
    return out


def _note_on_declaration(rel: str, lines: list[str], i: int, st: str) -> list:
    """A `//` among the comment lines directly above declaration line `i`, past any `template` line: one finding, since the declaration's comment is its `///`."""
    j = i - 1
    while j >= 0 and lines[j].lstrip().startswith("template"):
        j -= 1
    while j >= 0 and lines[j].lstrip().startswith("//"):
        # A snippet marker, `// --8<--`, is machine-read by the docs build, and an SPDX tag or an `// Author:` line is provenance: none is a comment on the declaration.
        if lines[j].lstrip().startswith(("// --8<--", "// SPDX-", "// Author:")):
            break
        if not lines[j].lstrip().startswith("///"):
            return [(f"{rel}::{_declared_name(st, i)}", f"{_NOTE_ON_DECL_RULE}: make it the declaration's `///`, or move it into the body")]
        j -= 1
    return []


def _help_cards(help_file: dict) -> list:
    """Each catalog card that misses its picture or shows the wrong kind, and each control line that runs too long or restates its default or range."""
    out = []
    for type_name, m in sorted(help_file.get("modules", {}).items()):
        page = "moonmodules/" + m.get("doc", "").partition("#")[0]
        catalog = help_file.get("catalogs", {}).get(m.get("doc", "").partition("#")[0])
        # The class whose comment introduces the page is no card on it, as the docs build draws it.
        if catalog is None or type_name == catalog.get("base"):
            continue
        source = m.get("source", "").partition("/blob/")[2].partition("/")[2] or type_name
        max_control = MAX_CONTROL_VISUAL if page in ANIMATED_PAGES else MAX_CONTROL
        restating = [c for c, h in m.get("controls", {}).items() if _RESTATES_RE.search(re.sub(r"`[^`]*`", "", h.get("line", "")))]
        if restating:
            out.append((source, f"{_RESTATES_RULE}, a default or a range the reset button or slider shows: {', '.join(restating)}"))
        for c, h in m.get("controls", {}).items():
            width = len(f"`{c}`: {h.get('line', '')}")
            if width > max_control:
                out.append((source, f"one control {width} > {max_control}: {c}"))
        image = m.get("image", "")
        if not image:
            if page not in PREVIEWLESS_PAGES:
                out.append((source, "no image: every card leads with one"))
        else:
            want = ".gif" if page in ANIMATED_PAGES else ".png"
            if not image.lower().endswith(want):
                why = "an effect, modifier or layout shows motion" if want == ".gif" else "a card or control is sharper and smaller as a png"
                out.append((source, f"image is {image.rsplit('.', 1)[-1]}, not a {want.lstrip('.')}: {why}"))
    return out


def _appendix_sections(lines: list[str]) -> list[str]:
    """The `## ` headings of a file's `@technical` appendices, in order, skipping fenced examples as the section budget does."""
    out = []
    for i, ln in enumerate(lines):
        if "@technical" not in ln:
            continue
        fenced = False
        for m in lines[i + 1:]:
            if not m.lstrip().startswith("///") or "@guide" in m:
                break
            text = re.sub(r"^\s*///\s?", "", m).strip()
            if text.startswith("```"):
                fenced = not fenced
            elif not fenced and text.startswith("## "):
                out.append(text[3:].strip())
    return out


def _unreferenced_sections():
    """Appendix sections no `@xref` in their file reaches.

    A section only a reader scrolling past finds is the first to cut: the appendix total falls by removing what no reader is sent to. The header is read in place, on GitHub or in an editor, so only its own `@xref`s send a reader to a section.
    """
    out = []
    for rel in _headers():
        if _generates_a_page(rel.as_posix()):
            out += _unreferenced_in(rel.as_posix(), (ROOT / rel).read_text(encoding="utf-8").split("\n"))
    return out


def _unreferenced_in(rel: str, lines: list[str]) -> list:
    """One file's appendix sections that none of its own `@xref`s reach."""
    xrefs = {m.group(1).strip().rstrip("\\") for ln in lines for m in re.finditer(r"@xref\{([^}|]+)", ln)}
    return [(f"{rel}::@technical {section}", f"{_UNREFERENCED_RULE}: no @xref reaches it")
            for section in _appendix_sections(lines) if _slugify_heading(section) not in xrefs]


# Markdown prose is hard-wrapped when a paragraph line continues onto the next. The standards
# forbid it for the same reason the header rule does: the editor soft-wraps, so a one-word edit
# is a one-word diff rather than a reflowed paragraph. Checked over AUTHORED pages only, since a
# generated one carries its source's shape and is fixed at the generator.
# Indentation is tested on the RAW line above, since these are matched after stripping.
_MD_SKIP_PREFIX = ("#", "|", "-", "*", ">", "<", "```", "!", "=", ":")
_MD_NUMBERED = re.compile(r"^\d+[.)]\s")


def _md_generated(text: str) -> bool:
    """A page a tool rewrites, which every generator marks with the same warning.

    Keyed on "do not edit by hand" rather than on "generated by": a hand-written page may well
    say what produced the files it lists, and exempting on that wording let an authored index
    out of the check. A tool that owns a file says so in the imperative.
    """
    head = "\n".join(text.split("\n")[:8]).lower()
    return "do not edit by hand" in head


def _md_hard_wraps(rel: str, text: str):
    """Each paragraph line that continues onto the next one."""
    if _md_generated(text):
        return []
    out = []
    lines = text.split("\n")
    fenced = False
    for i, line in enumerate(lines[:-1]):
        stripped = line.strip()
        if stripped.startswith("```"):
            fenced = not fenced
            continue
        if fenced or not stripped:
            continue
        # INDENTATION is read before stripping: an indented block is a listing or a fenceless code
        # sample, not prose, and the stripped line can never carry the leading whitespace that says so.
        if line.startswith(("    ", "\t")) or stripped.startswith(_MD_SKIP_PREFIX) or _MD_NUMBERED.match(stripped):
            continue
        nxt = lines[i + 1].strip()
        if (not nxt or lines[i + 1].startswith(("    ", "\t"))
                or nxt.startswith(_MD_SKIP_PREFIX) or _MD_NUMBERED.match(nxt)):
            continue
        # A line ending mid-sentence is the wrap; one ending a sentence is a deliberate break.
        if stripped[-1] not in ".!?:":
            out.append((f"{rel}::line {i + 1}",
                        "hard wrap: one line per paragraph, let the editor soft-wrap"))
    return out


def _help_freshness():
    """The committed help.json against what the headers and cards generate now: a header edit that never reached the file would ship help that contradicts the module."""
    import gen_help
    try:
        stale = gen_help.stale()
    except (SystemExit, Exception) as e:   # no LLVM, or a parse that failed: reported, so the rest of the report still runs
        return [("docs/help/help.json", f"{_STALE_HELP_RULE} unchecked: {e}")]
    return ([("docs/help/help.json",
              f"{_STALE_HELP_RULE}: run `uv run moondeck/docs/gen_help.py` and commit the result")]
            if stale else [])


_STOPWORDS = {"with", "from", "that", "this", "into", "each", "when", "then", "than", "their", "there", "which",
              "while", "what", "only", "also", "them", "they", "does", "over", "under", "onto", "just", "more", "most", "such"}


def _guide_repeats(help_file: dict) -> list:
    """Each control whose longer help opens by repeating its line: most of the line's words back in the guide's first sentence."""
    def words(t: str) -> set:
        return {w for w in re.findall(r"[a-z]{4,}", t.lower()) if w not in _STOPWORDS}
    out = []
    for type_name, m in sorted(help_file.get("modules", {}).items()):
        for name, h in m.get("controls", {}).items():
            line, more = h.get("line", ""), h.get("more", "")
            said = words(line)
            if not more or not said:
                continue
            first = words(re.split(r"(?<=[.!?])\s", more, maxsplit=1)[0])
            # Either way round: the guide saying most of the line, or opening with a sentence the line already holds.
            if len(said & first) / len(said) >= 0.6 or (len(first) >= 2 and len(said & first) / len(first) >= 0.6):
                source = m.get("source", "").partition("/blob/")[2].partition("/")[2] or type_name
                out.append((source, f"{_GUIDE_REPEATS_RULE} `{name}`: its guide opens with what its line says; say only what the line cannot hold"))
    return out


def _help_lines(help_file: dict) -> list:
    """Each module without its help line, and each control without its own: the `///` the app shows under a title and a field."""
    out = []
    for type_name, m in sorted(help_file.get("modules", {}).items()):
        source = m.get("source", "").partition("/blob/")[2].partition("/")[2] or type_name
        if not m.get("line"):
            out.append((source, f"{_HELP_LINE_RULE}: `{type_name}` has no class `///`"))
        out += [(source, f"{_HELP_LINE_RULE}: control `{c}` has no `///` on its member or `add…` line")
                for c, h in m.get("controls", {}).items() if not h.get("line")]
    return out


def _tag_groups(help_file: dict, scripts: dict) -> list:
    """Each tag emoji only one module or factory script carries, `scripts` mapping a script's path to its `tags()`."""
    carriers = {}
    for type_name, m in help_file.get("modules", {}).items():
        source = m.get("source", "").partition("/blob/")[2].partition("/")[2] or type_name
        for e in set(m.get("tags", "").replace("\uFE0F", "")):
            carriers.setdefault(e, []).append(source)
    for path, tags in scripts.items():
        for e in set(tags.replace("\uFE0F", "")):
            carriers.setdefault(e, []).append(path)
    return [(who[0], f"{_TAG_GROUP_RULE}: {e} groups nothing; share one from the legend or drop it")
            for e, who in sorted(carriers.items()) if len(who) == 1]


def _script_tags() -> dict:
    """The `tags()` of every factory MoonLive script, by path."""
    out = {}
    for p in sorted((ROOT / "moonlive").rglob("*.ml?")):
        if m := re.search(r'tags\(\)\s*\{\s*return\s*"([^"]*)"', p.read_text(encoding="utf-8")):
            out[p.relative_to(ROOT).as_posix()] = m.group(1)
    return out


def _guide_names_control(help_file: dict) -> list:
    """Each module whose general guide holds a section titled with one of its controls: a control is explained on its member, in its `@guide` line."""
    out = []
    for type_name, m in sorted(help_file.get("modules", {}).items()):
        source = m.get("source", "").partition("/blob/")[2].partition("/")[2] or type_name
        for g in m.get("guide", []):
            if g["title"] in m.get("controls", {}):
                out.append((source, f"{_GUIDE_NAMES_CONTROL_RULE} `{g['title']}`: move it to the member's `/// @guide` line"))
    return out


def _help_size(help_file: dict) -> list:
    """Each module on a generated catalog page whose app help runs past a glance: a control's longer help, a guide section, or the whole."""
    def words(t: str) -> int:
        return len(re.findall(r"\w+", t))
    pages = set(help_file.get("catalogs", {}))
    out = []
    for type_name, m in sorted(help_file.get("modules", {}).items()):
        if m.get("doc", "").partition("#")[0] not in pages:
            continue
        source = m.get("source", "").partition("/blob/")[2].partition("/")[2] or type_name
        mores = {c: words(h["more"]) for c, h in m.get("controls", {}).items() if h.get("more")}
        sections = [(g["title"], words(g["text"])) for g in m.get("guide", [])]   # a list, so two sections sharing a title each count
        out += [(source, f"{_HELP_SIZE_RULE}: `{c}`'s guide {n} words > {MAX_MORE_WORDS}") for c, n in mores.items() if n > MAX_MORE_WORDS]
        out += [(source, f"{_HELP_SIZE_RULE}: guide '{t}' {n} words > {MAX_GUIDE_WORDS}") for t, n in sections if n > MAX_GUIDE_WORDS]
        total = words(m.get("summary", "")) + sum(mores.values()) + sum(n for _, n in sections)
        if total > MAX_HELP_WORDS:
            out.append((source, f"{_HELP_SIZE_RULE}: {total} words > {MAX_HELP_WORDS}; move tables, steps and reasons to a how-to page"))
    return out


def _violations():
    out = _help_freshness()
    out.extend(_guide_repeats(_hooks._help_file()))
    out.extend(_help_size(_hooks._help_file()))
    out.extend(_guide_names_control(_hooks._help_file()))
    out.extend(_help_lines(_hooks._help_file()))
    out.extend(_tag_groups(_hooks._help_file(), _script_tags()))
    out.extend(_help_cards(_hooks._help_file()))

    for rel in sorted((ROOT / "docs").rglob("*.md")):
        parts = rel.relative_to(ROOT / "docs").parts
        # docs/work/ is the exemption the standards name.
        if parts[0] == "work":
            continue
        # DOCS-relative, the key catalog findings use: a repo-relative one listed the same
        # page twice in the report, once per spelling.
        out.extend(_md_hard_wraps(rel.relative_to(ROOT / "docs").as_posix(), rel.read_text(encoding="utf-8")))

    for rel in _headers():
        out.extend(_header_rules(rel.as_posix(), (ROOT / rel).read_text(encoding="utf-8")))
    out.extend(_unreferenced_sections())
    return out


REPORT = ROOT / "docs" / "reference" / "metrics" / "docgen.md"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from _ratchet import committed as committed_report, risen as risen_against  # noqa: E402


# Which summary page owns a source folder, mirroring the zoom diagram in
# documentation-standards.md. A sweep runs page by page, so the report groups by the same unit:
# the question "how far is the sweep" is answered per area, not per file.
#
# Longest prefix wins, so `light/moonlive` beats `light`. A path matching nothing is "unassigned",
# which is itself the finding: a header no summary page owns reaches no reader.
DOC_AREAS = (
    ("src/core/system",           "core/system.md"),
    ("src/core/services",         "core/services.md"),
    ("src/core/module",           "core/system.md"),
    ("src/core/util",             "core/supporting.md"),
    # The numeric vocabulary is the LIGHT domain's, whatever directory it sits in: effects and
    # scripts write against it and the control surface never touches it. A directory prefix put
    # five headers and 240 findings on the control page, where the sweep would have documented
    # them under a heading no reader of that page is looking for. Longest prefix wins, so these
    # file-level entries override the `src/core/util` line above.
    ("src/core/util/math16.h",      "light/power-functions.md"),
    ("src/core/util/math8.h",       "light/power-functions.md"),
    ("src/core/util/noise.h",       "light/power-functions.md"),
    ("src/core/util/oscillators.h", "light/power-functions.md"),
    ("src/core/util/color.h",       "light/power-functions.md"),
    ("src/core/moonlive",         "light/moonlive.md"),
    ("src/light/moonlive",        "light/moonlive.md"),
    ("src/light/effects",         "light/effects.md"),
    ("src/light/layouts",         "light/layouts.md"),
    ("src/light/modifiers",       "light/modifiers.md"),
    ("src/light/drivers",         "light/drivers.md"),
    ("src/light/powerfunctions",  "light/power-functions.md"),
    ("src/light/layers",          "light/supporting.md"),
    ("src/light/util",            "light/supporting.md"),
    ("src/ui",                    "core/ui.md"),
    ("src/platform",              "platform/index.md"),
    # The composition roots and the second boot image. `main.cpp` wires the module tree and
    # registers every docPath on the system page, the ESP32 entry point calls into it, and
    # MoonBase is the firmware-update surface that page documents.
    ("src/main.cpp",              "core/system.md"),
    ("esp32/main",                "core/system.md"),
    ("moonbase/main",             "core/system.md"),
    # A MoonLive backend tool: it compiles a script through one real assembler and prints
    # the bytes, which is what `check_encodings.py` drives.
    ("moondeck/moonlive",         "light/moonlive.md"),
)


def _doc_area(path: str) -> str:
    """The summary page that owns `path`, or a bucket for what no page covers."""
    best = ""
    area = ""
    for prefix, page in DOC_AREAS:
        if path.startswith(prefix) and len(prefix) > len(best):
            best, area = prefix, page
    if area:
        return area
    if path.startswith("test/"):
        return "(tests, no card)"
    return "(unassigned)"


def _write_report(found, appendix=None) -> None:
    """The current state as a tracked page, so its git history is the trend.

    The baseline records WHICH cards are tolerated. This records HOW MUCH is left, per page
    and per rule, so `git log -p docs/reference/metrics/docgen.md` answers whether the
    documentation is getting better. Same shape as repo-health.md, for the same reason.
    """
    from collections import Counter, defaultdict
    by_page = defaultdict(list)
    for key, why in found:
        page, _, title = key.partition("::")
        by_page[page].append((title, why))
    errs = [f for f in found if _blocks(f[0], f[1])]
    warns = [f for f in found if not _blocks(f[0], f[1])]
    rules = Counter(_rule_name(why) for _, why in found)
    rules_e = Counter(_rule_name(why) for _, why in errs)
    rules_w = Counter(_rule_name(why) for _, why in warns)

    out = ["# Docgen", "",
           "Generated by [`moondeck/check/check_docgen.py`](../../../moondeck/check/check_docgen.py) "
           "on every run. **Do not edit by hand.**", "",
           "Every place the generated documentation breaks the shape [the standards]"
           "(../../contributing/documentation-standards.md#the-card) define. Current state only: "
           "the trend is this file's git history. The list only shrinks.", "",
           f"**{len(errs)} error(s)** and **{len(warns)} warning(s)** "
           f"across {len(by_page)} page(s).", "",
           *([f"**{appendix} appendix line(s)** after `@technical`. This only falls as well: a cleanup "
              "cuts first, and moves only what neither the code, the test name nor the spec says.", ""]
             if appendix is not None else []),
           "An error is in a file that generates a documentation page, a header or a catalog page, "
           "so the finding is a defect in what gets published and it fails the gate. A warning is "
           "worth fixing without being worth stopping a commit for: any finding in an implementation "
           "file, which publishes nothing, and the rules still staged as counts in a header too, the "
           "over-wide line and the appendix section nothing refers to. Both are counted here, "
           "because a warning nobody sees is a warning nobody fixes.", "",
           "The split is temporary. It stages the sweep rather than ranking the two kinds of "
           "comment, so when the warning column reaches zero it goes and every finding blocks.", "",
           "## By rule", "", "| Rule | Errors | Warnings |", "|---|---:|---:|"]
    # Errors first, since that is the number a gate turns on: a rule with more warnings than
    # errors would otherwise outrank the one actually blocking the commit.
    for rule in sorted(rules, key=lambda r: (-rules_e.get(r, 0), -rules_w.get(r, 0), r)):
        out.append(f"| {_rule_label(rule)} | {rules_e.get(rule, 0)} | {rules_w.get(rule, 0)} |")
    # By the unit a sweep actually runs in: one summary page and the headers it owns.
    areas = defaultdict(int)
    areas_e = defaultdict(int)
    areas_w = defaultdict(int)
    for page, items in by_page.items():
        if page.endswith(".md"):
            continue
        area = _doc_area(page)
        areas[area] += len(items)
        # Per FINDING, through _blocks: a file is not wholly one severity, because a header's
        # line-length finding warns while its neighbors block.
        for _, why in items:
            (areas_e if _blocks(page, why) else areas_w)[area] += 1
    if areas:
        out += ["", "## By documentation area", "",
                "The unit a sweep runs in: one summary page and the headers it owns, as the "
                "[hierarchy zoom](../../contributing/documentation-standards.md"
                "#zooming-in-on-the-green-boxes) lays them out. A page is swept when its errors "
                "reach zero; its warnings say how much implementation-file cleanup is left "
                "behind that.", "",
                "| Summary page | Errors | Warnings |", "|---|---:|---:|"]
        for area in sorted(areas, key=lambda a: (-areas_e[a], -areas[a], a)):
            out.append(f"| `{area}` | {areas_e[area]} | {areas_w[area]} |")
    cards = {k: v for k, v in by_page.items() if k.endswith(".md")}
    headers = {k: v for k, v in by_page.items() if not k.endswith(".md")}
    if cards:
        out += ["", "## Catalog pages", "", "| File | Findings |", "|---|---:|"]
        for page in sorted(cards, key=lambda k: (-len(cards[k]), k)):
            out.append(f"| `{page}` | {len(cards[page])} |")

    # PER AREA, because the area is the unit a sweep runs in and the question a reader has is
    # "what would I open first". A flat list of 300 files answered that only by being read whole,
    # and the per-finding dump under it was 90% of a 4,200-line page: a log rather than a report.
    # Each area gets its own files ranked, a tail line for the long thin end, and its rule mix.
    # What a single finding says is what the check PRINTS; this page is where the work is planned.
    out += ["", "## Where the work is", "",
            "Per area, since that is the unit a sweep runs in: the files ranked, then the rule mix. "
            "A file's own findings are in the check's output, which names every one.", ""]
    per_area = defaultdict(dict)
    for page, items in headers.items():
        per_area[_doc_area(page)][page] = items
    for area in sorted(per_area, key=lambda a: (-sum(len(v) for v in per_area[a].values()), a)):
        files = per_area[area]
        total = sum(len(v) for v in files.values())
        # Ranked WITHIN each kind, and each kind gets its own head, so the biggest warning file
        # cannot be buried in the tail behind ten smaller errors. An 88-finding implementation
        # file is worth seeing even though it does not block.
        # Counted through _blocks, which reads the REASON as well as the file: a header's
        # line-length finding warns, so a file holding only those is a warning file. Grouping
        # on the file alone labeled 74 such files "error" under a report saying 0 errors.
        def _blocking(page, items):
            return [f for f in items if _blocks(page, f[1])]
        errs_a = sum(len(_blocking(k, v)) for k, v in files.items())
        out += [f"### {area}", "",
                f"**{errs_a} error(s)** and **{total - errs_a} warning(s)** "
                f"across {len(files)} file(s).", "",
                "| Findings | File | |", "|---:|---|---|"]
        HEAD = 10
        for kind, blocking in (("error", True), ("warning", False)):
            counts = {k: len([f for f in v if bool(_blocks(k, f[1])) == blocking])
                      for k, v in files.items()}
            group = sorted((k for k in files if counts[k]), key=lambda k: (-counts[k], k))
            for page in group[:HEAD]:
                out.append(f"| {counts[page]} | `{page}` | {kind} |")
            tail = group[HEAD:]
            if tail:
                n = sum(counts[p] for p in tail)
                lo, hi = min(counts[p] for p in tail), max(counts[p] for p in tail)
                span = f"{lo}" if lo == hi else f"{lo}-{hi}"
                out.append(f"| {span} each | *{len(tail)} more {kind} files, {n} findings* | |")
        # Every ERRORING file's findings in full, right where the work is planned. An error
        # blocks a commit, so the one question its reader has is what to fix, and a summary
        # sends them back to a stdout that has scrolled away. Warnings stay counted rather
        # than listed: 3135 of them is the log this page was rewritten to stop being.
        for page in sorted(files):
            errs_f = [f for f in files[page] if _blocks(page, f[1])]
            if not errs_f:
                continue
            out += [f"#### `{page}`", ""]
            for key, why in sorted(errs_f, key=lambda f: f[1]):
                where = key.rpartition("::")[2] if "::" in key else ""
                out.append(f"- {where + ': ' if where else ''}{why}")
            out += [""]

        mix = Counter(_rule_name(why) for k, v in files.items()
                      for _, why in v if _blocks(k, why))
        mix_w = Counter(_rule_name(why) for k, v in files.items()
                        for _, why in v if not _blocks(k, why))
        out += [""]
        if mix:
            out += ["By rule, errors: "
                    + ", ".join(f"{n} {_rule_label(r)}" for r, n in mix.most_common()) + ".", ""]
        if mix_w:
            out += ["By rule, warnings: "
                    + ", ".join(f"{n} {_rule_label(r)}" for r, n in mix_w.most_common()) + ".", ""]
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"Docgen report: {len(found)} finding(s) written to {REPORT.relative_to(ROOT)}")


def _report(entries, heading: str) -> None:
    """Every entry, grouped by page, with a per-page count.

    A check that prints only a total tells a reader nothing they can act on, and this one
    runs as a MoonDeck card whose whole output is its log. So the detail is the report.
    """
    from collections import defaultdict
    by_page = defaultdict(list)
    for key, why in entries:
        page, _, title = key.partition("::")
        by_page[page].append((title, why))
    print(heading)
    for page in sorted(by_page):
        rows = by_page[page]
        print(f"\n  {page}  ({len(rows)})")
        for title, why in sorted(rows):
            print(f"    {title}: {why}")


def _committed_counts():
    """Every rule's warning count in the COMMITTED report, or None when there is no baseline.

    The report is tracked, so the last commit's copy is the number to beat. Read from git rather
    than from the working tree, because the run rewrites the file before the comparison happens.
    """
    text = committed_report(REPORT)
    if text is None:
        return None
    counts = {}
    # Only the `## By rule` table: the area and file tables share its column shape, and a row from
    # either would enter the baseline as a rule that can never appear in `now`.
    in_rules = False
    for line in text.split("\n"):
        if line.startswith("## "):
            in_rules = line.strip() == "## By rule"
        m = re.match(r"^\| (.+?) \| (\d+) \| (\d+) \|$", line.strip())
        if in_rules and m and m.group(1) != "Rule":
            counts[m.group(1)] = int(m.group(3))
        # The TOTAL as its own entry, because a per-rule comparison alone misses a rule that is
        # below the baseline and still rising: one under budget can absorb a new finding silently.
        m = re.match(r"^\*\*\d+ error\(s\)\*\* and \*\*(\d+) warning\(s\)\*\* across", line.strip())
        if m and "(total)" not in counts:
            counts["(total)"] = int(m.group(1))
        m = re.match(r"^\*\*(\d+) appendix line\(s\)\*\*", line.strip())
        if m:
            counts["(appendix lines)"] = int(m.group(1))
    return counts or None


def _ratchet(found, appendix=None) -> list:
    """Which rules rose against the committed report. The list only shrinks.

    A warning is staged work, and staged work that grows is not a sweep. Per RULE rather than on
    the total alone, because a total hides one rule paying for another: splitting a long line
    lowers the width count and raises the block count, which is the move the run cap exists to
    refuse.
    """
    from collections import Counter
    base = _committed_counts()
    if base is None:
        return []
    # LABELLED, matching how the report writes them: `_rule_name` returns the internal key
    # ("code comment") while the table carries the display label ("multi-line comment blocks"),
    # and comparing the two key spaces silently compares nothing.
    now = Counter(_rule_label(_rule_name(why)) for key, why in found if not _blocks(key, why))
    now["(total)"] = sum(1 for key, why in found if not _blocks(key, why))
    # The appendix is its own row, because warnings that fall while the appendix grows were moved, not cut; a baseline without the row is a first run for it.
    base = dict(base)
    if appendix is not None and "(appendix lines)" in base:
        now["(appendix lines)"] = appendix
    else:
        base.pop("(appendix lines)", None)
    return risen_against(base, now)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.parse_args()

    found = _violations()

    # The report is the artifact: stdout scrolls away and is truncated, the file is what the
    # reviewer reads, and its git history is the only record of the sweep's trend. Written on
    # every run with no way to suppress it: the run still prints and still returns its exit
    # code, so writing it costs a caller nothing and a skipped write loses the number to beat.
    appendix = _appendix_total()
    _write_report(found, appendix)

    errors = [f for f in found if _blocks(f[0], f[1])]
    warnings = [f for f in found if not _blocks(f[0], f[1])]

    risen = _ratchet(found, appendix)
    if not found and not risen:
        print(f"Docgen check: clean. Limits: one control {MAX_CONTROL} "
              f"({MAX_CONTROL_VISUAL} visual), one comment line, "
              f"{MAX_DOC_WORDS} words.")
        return 0

    print(f"Docgen check: {len(errors)} error(s), {len(warnings)} warning(s).\n")
    if risen:
        print("ROSE against the committed report. These only shrink:")
        for rule, was, now in risen:
            print(f"  {rule}: {was} -> {now}")
            if rule == "(appendix lines)":
                for rel, a, b in _appendix_growth():
                    print(f"    {rel}: {a} -> {b}")
        print("  Fix them, or say in the commit why the rule itself changed.\n")
    if errors:
        _report(errors, "ERRORS. These fail the gate.")
    if warnings:
        if errors:
            print()
        # Not "files that generate no page": the staged rules (line length, unreferenced sections) warn in headers too.
        _report(warnings, "WARNINGS. Worth fixing, not worth blocking a commit.")
    print("\nCut first. Move only what neither the code, the test name nor the spec says,"
          "\nand prefer an existing home over a new appendix section."
          "\nRules: docs/contributing/documentation-standards.md § The card.")
    return 1 if (errors or risen) else 0


if __name__ == "__main__":
    sys.exit(main())
