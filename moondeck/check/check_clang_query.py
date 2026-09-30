#!/usr/bin/env python3
"""Bespoke AST rules — the checks we invent, that no off-the-shelf tool reports.

The rest of the stack enforces rules someone else wrote; this is where OUR rules live. Each
rule is an AST matcher plus a Python predicate, and this file holds a LIST of them: rule two
costs a list entry, not a new script, a new MoonDeck card and a new help page. That framing is
the point — the first rule alone would not earn a script.

clang-query rather than a compiled clang-tidy plugin: matchers are plain text, minutes to
write, no plugin to build, no LLVM ABI to track — and clangd will not load a compiled plugin,
so a custom check written that way could never appear in the editor anyway.

## Rules

1. **RAM-costing arrays.** A fixed array is a fixed size, and the architecture says sizes are
   determined at runtime from available memory. This reports the arrays that actually cost RAM,
   split by where that RAM lives, because the fix differs per category:
     locals   — on the stack; a big one risks overflow on a 4 KB task stack
     members  — per INSTANCE, so the cost multiplies by how many exist
   `constexpr` and static-storage arrays are excluded: they live in flash and cost no RAM, so
   reporting them would flag lookup tables as memory bloat. (Measured: including them roughly
   triples the list with entries nobody should act on — `MoonModule::name_[16]` was itself
   *shrunk* from [24] to save 8 bytes per module, and would have been reported as a problem.)

2. **Heap allocation sites.** Every `new` / `delete` / `malloc`-family call in our own code.
   Not a violation — the driver layer allocates deliberately — but the hot path must not, and
   the count is the thing worth trending.

3. **Comments per declaration.** Which classes, methods and attributes are documented, and with
   which kind: `///` is what moxydoc publishes, while a `//` stacked on a declaration is usually a
   thought that belongs in the code below it. Sized in WORDS against per-scope ideals, with the
   deviation reported rather than a pass/fail — a class header IS the module spec, so its length
   is a judgment, not a defect.

   Clang's lexer discards `//`, so it reaches the AST only via the shadow tree below: a copy of
   src/ where a leading `//` becomes `/// MMDEV:`. That marker is what keeps the two kinds apart
   in the DEV column; the real source is never touched.

Usage:
  uv run moondeck/check/check_clang_query.py              # every rule
  uv run moondeck/check/check_clang_query.py --rule arrays
  uv run moondeck/check/check_clang_query.py --min-bytes 64
"""

import argparse
import collections
import functools
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_clang_tidy  # noqa: E402  — reuse its build-dir and toolchain logic, one owner

# Bytes per element for the types we actually declare arrays of. Anything unlisted (a struct,
# an enum, a class) falls back to 4: this report is about ORDER OF MAGNITUDE, and guessing a
# struct's exact layout would need the full ABI for no gain in what the reader does next.
ELEM_BYTES = {
    "char": 1, "signed char": 1, "unsigned char": 1, "bool": 1, "int8_t": 1, "uint8_t": 1,
    "short": 2, "unsigned short": 2, "int16_t": 2, "uint16_t": 2,
    "int": 4, "unsigned int": 4, "int32_t": 4, "uint32_t": 4, "float": 4,
    "long": 8, "unsigned long": 8, "size_t": 8, "double": 8, "int64_t": 8, "uint64_t": 8,
}
DEFAULT_ELEM_BYTES = 4

# Default array threshold: ZERO — report every RAM-costing array, however small. A size cutoff
# hides things for the wrong reason (`bool birthNumbers_[9]` is 9 bytes and was invisible at the
# old default of 10), and the interesting question is "what does this module allocate", not "is
# any single one big". Measured: no threshold is 362 findings vs 290 at >10 bytes, so the cutoff
# was buying almost nothing. Volume is controlled by MAX_ROWS instead, which truncates the
# LONGEST lists rather than silently dropping the smallest entries.
DEFAULT_MIN_BYTES = 0

# How many rows a table prints before truncating, worst-first. Deep enough that the RAM-costing
# array table reaches the small entries that matter on a 180 KB heap, short enough that a
# repo-wide sweep (362 arrays) stays readable. Truncation is always announced — a silently
# short list would read as a clean result.
DEFAULT_MAX_ROWS = 60

# `VarDecl`/`FieldDecl <path:line:col...> ... name 'type[N]'`. clang-query's `dump` output is
# an AST dump, so this reads its stable prefix rather than trying to parse the whole tree.
_DECL = re.compile(
    r"(?P<kind>VarDecl|FieldDecl) 0x\w+ <(?P<path>[^>]*?):(?P<line>\d+):\d+[^>]*>"
    r"[^\n]*?(?P<name>\w+) '(?P<type>[\w:\s]+?)\[(?P<count>\d+)\]'")

# An allocation node in the AST dump: `CXXNewExpr 0x... <path:line:col, ...>`.
_HEAP_NODE = re.compile(
    r"(?P<kind>CXXNewExpr|CXXDeleteExpr|CallExpr) 0x\w+ <(?P<path>[^>]*?):(?P<line>\d+):"
    r"(?P<col>\d+)")

# The callee inside a CallExpr's child DeclRefExpr: `... Function 0x... 'free' '...'`.
_CALLEE = re.compile(r"Function 0x\w+ '(?P<name>\w+)'")

# What is being allocated or freed, from the operand leaf under the node. Two forms:
#   MemberExpr ... lvalue ->buf        a member  (`free(buf)` inside a method)
#   DeclRefExpr ... lvalue Var 0x... 'ptr'   a local
# `free(buf)` and `free(nodes)` on the SAME LINE are otherwise indistinguishable in the report,
# which is what makes this worth digging two levels through the implicit casts for.
_OPERAND_MEMBER = re.compile(r"MemberExpr 0x\w+ [^\n]*lvalue (?:->|\.)(?P<name>\w+)")
# `Var` for a local, `ParmVar` for a function parameter — `platform::free(ownedBody)` where
# ownedBody is a parameter matched nothing until ParmVar was included (6 of 63 sites).
_OPERAND_VAR = re.compile(
    r"DeclRefExpr 0x\w+ [^\n]*lvalue (?:Var|ParmVar) 0x\w+ '(?P<name>\w+)'")

# The enclosing function, from the head of the "fn" binding block. Its qualified name is not
# in the dump, so the class is recovered from the `'mm::Foo *' implicit this` line when present.
# `[^<]*` before the location: an OUT-OF-LINE definition prints extra fields first
# (`CXXMethodDecl 0x... parent 0x... prev 0x... <path...>`), and requiring `<` immediately
# after the address silently missed every method defined in a .cpp — 4 of 5 sites blank.
_FN_DECL = re.compile(
    r"^(?:CXX(?:Method|Constructor|Destructor|Conversion)Decl|FunctionDecl) 0x\w+[^<]*<[^>]*>"
    r"[^\n]*?\b(?P<name>~?(?:operator\s*\S+|\w+)) '")
_FN_CLASS = re.compile(r"CXXThisExpr 0x\w+ <[^>]*> '(?:const )?(?:\w+::)*(?P<cls>\w+) \*'")

# A ScratchBuffer member: `FieldDecl 0x... <path:line:col...> ... name 'ScratchBuffer<uint8_t>':...`
_SCRATCH = re.compile(
    r"FieldDecl 0x\w+[^<]*<(?P<path>[^>]*?):(?P<line>\d+):\d+[^>]*>"
    r"[^\n]*?(?P<name>\w+) 'ScratchBuffer<(?P<elem>[^>]+)>'")

# The type a `new` produces: `CXXNewExpr 0x... <...> 'Foo *' array ...`.
_NEW_TYPE = re.compile(r"CXXNewExpr 0x\w+ <[^>]*> '(?P<type>[^']+?) ?\*'")

# A doc comment's span. The AST dump elides whatever is unchanged from the previous node, so the
# same construct prints three ways and all three must parse:
#   <line:272:4, line:298:76>   multi-line
#   <line:309:8, col:61>        multi-column, one line
#   <col:23, col:71>            ENTIRELY on the current line — no line number at all
# The third form is why ~45 one-line `///` comments (every effect, layout and modifier header)
# were silently missing from the first version: a regex demanding a digit pair never matched
# them. When a `line:` is absent the line is inherited from the enclosing context, which is
# tracked while walking.
_FULLCOMMENT = re.compile(
    r"FullComment 0x\w+ <(?:(?P<path>[^:>]*):(?P<pstart>\d+):\d+|line:(?P<start>\d+):\d+|col:\d+)"
    r"(?:, (?:[^:>]*:(?P<pend>\d+):\d+|line:(?P<end>\d+):\d+|col:\d+))?>")

# The text of one comment line: `TextComment 0x... <col:4, col:29> Text=" A doc comment."`.
_TEXTCOMMENT = re.compile(r"TextComment 0x\w+ <[^>]*> Text=\"(?P<text>.*)\"")

# Any node that is part of a doc comment's subtree rather than the declaration below it.
# `Comment` alone would also swallow the FullComment of the NEXT comment, so the wrappers are
# named explicitly and FullComment is deliberately absent — reaching one means this comment ended.
_COMMENT_NODE = re.compile(
    r"\b(?:ParagraphComment|BlockCommandComment|ParamCommandComment|TParamCommandComment"
    r"|InlineCommandComment|VerbatimBlockComment|VerbatimBlockLineComment|VerbatimLineComment"
    r"|HTMLStartTagComment|HTMLEndTagComment)\b")

# The declaration a comment is attached to — the next decl node after the comment subtree.
# Clang dumps a doc comment as the first child of the declaration it documents, so "the next
# decl below this FullComment" is its owner. Validated on a real TU: 206 of 207 comments
# resolved, split 17 class / 155 method / 28 field / 6 constructor.
# The name is the LAST identifier before the type/qualifier tail, not the first token after the
# source range — a non-greedy `\b(\w+)\b` there captures `col` out of `<line:299:1, col:7> col:7
# implicit referenced class ControlList`, which is how the first version reported every row as
# "col". Anchoring on the trailing keyword (`class`/`struct`/`union`) or the quoted type is what
# makes it the declaration's own name.
_OWNER = re.compile(
    r"(?P<kind>CXXRecordDecl|CXXMethodDecl|CXXConstructorDecl|CXXDestructorDecl"
    r"|CXXConversionDecl|FieldDecl"
    r"|FunctionDecl|VarDecl|EnumDecl|EnumConstantDecl|TypedefDecl|TypeAliasDecl) 0x\w+[^<]*"
    r"<(?:[^:>]*:)?(?:line:)?(?P<line>\d+):\d+[^>]*>"
    # A record's name follows its keyword and may be trailed by `definition`; a `[^\n]*?` before
    # the keyword would let the tail win, so the trailing token is matched explicitly. Without
    # this, every `class Foo … definition` line reported its name as "definition".
    r"(?:[^\n]*?\b(?:class|struct|union|enum)\s+(?P<cname>\w+)(?:\s+definition)?\s*$"
    r"|[^\n]*?(?P<fname>~?(?:operator\s*\S+|\w+))\s+')", re.M)

# A DIRECT child of the block head: exactly one tree connector before the node name. A deeper
# node (`| |-`, `|   `-`) belongs to a nested declaration, which clang-query emits as its own
# bound block — so depth is what stops a class absorbing every comment its members carry.
_TOP_CHILD = re.compile(r"^[|`]-")

# A record declaration head, for the forward-declaration test above. Only records can be
# forward-declared; a method or attribute always has its definition where it is declared.
_RECORD_HEAD = re.compile(r"^(?:CXXRecordDecl|ClassTemplateDecl) ")

# The file a dumped node belongs to. The AST dump prints an absolute path only when it CHANGES,
# then `line:N` for subsequent nodes in the same file — so the current file has to be tracked
# as the dump is walked, not read off each node.
_PATH_HINT = re.compile(r"<(?P<path>/[^:>]*\.(?:h|cpp|hpp|cc)):(?P<line>\d+):")

# `<line:N:C>` with no path — same file, new line. Feeds the line that a `col:`-only span inherits.
_LINE_HINT = re.compile(r"<line:(?P<line>\d+):\d+")

# What each owner kind counts as, for the per-scope split. A constructor is a method; an enum or
# a typedef is neither class nor method, so it reports as `other` rather than being dropped —
# a scope nobody expected is a finding, not a reason to hide the row.
_SCOPE = {
    "CXXRecordDecl": "class", "EnumDecl": "class",
    "EnumConstantDecl": "attribute",
    "CXXMethodDecl": "method", "CXXConstructorDecl": "method",
    "CXXDestructorDecl": "method", "CXXConversionDecl": "method", "FunctionDecl": "method",
    "FieldDecl": "attribute", "VarDecl": "attribute",
}

# What "documented well" looks like, in WORDS, per declaration kind.
#
# Words rather than lines: a line is a formatting accident — the same paragraph is 5 lines at 100
# columns and 9 at 60 — while the word count is what a reader actually absorbs. Measured on this
# tree, a comment line carries a median of 13 words in BOTH kinds, so a line-based yardstick of
# class 10 / method 3 / attribute 3 converts to 130 and 40.
#
# A class carries the spec: what it is for, how it fits, what a caller must know. A method or an
# attribute is one idea. This is a ruler, not a gate — MoonI80Peripheral's header may be exactly
# right at ten times the ideal, because it IS the driver's spec.
_IDEAL_DOC_WORDS = {"class": 130, "method": 40, "attribute": 40, "other": 40}

# `@moreinfo` ends the part being measured. The directive splits a class comment in two (see
# gen_api.py): everything before it is the lead description, rendered above the attribute/method
# lists, and everything after is deep-dive reference relocated BELOW them under `## More info`.
#
# Only the lead is measured, because the ideal asks "how much must a reader take in before the
# member lists" — and reference material deliberately parked at the bottom of the page is not that.
# Counting the whole block punished the very structure the directive exists to encourage:
# NetworkSendDriver read +407% as one blob, and +48% once its 469 More-info words were separated
# from its 193-word lead.
#
# Matched on the AST node, not the source text: clang parses the directive into
# `InlineCommandComment ... Name="moreinfo"`, so the split point is stated rather than guessed at.
_MOREINFO_NODE = re.compile(r'InlineCommandComment.*Name="moreinfo"')

# The `//` side deliberately has NO ideal, because zero IS the ideal: a developer note stacked on
# a declaration is usually a thought that belongs in the code below it. Measuring it as
# deviation-from-one-line made the best case (no note at all) read as -100%, i.e. worst — so DEV
# is reported as a raw word count. Zero reads as zero.

# Three constraints this matcher set was built around. Recorded here because two are NEGATIVE
# results — the kind that gets re-attempted by whoever forgets they were already tried:
#
#   1. There is NO size-threshold matcher. `hasSize(N)` is exact-match and `sizeGreaterThan`
#      does not exist, so every "> N bytes" decision happens in Python below, not in the AST.
#   2. `varDecl` alone silently misses EVERY class member — it needs `fieldDecl` beside it.
#      The first version of this rule had only `varDecl` and reported a plausible, wrong list.
#   3. "Fixed number vs named constant" is NOT RECOVERABLE. The AST folds `kMaxLanes` to `[16]`
#      before a matcher sees it: `busPinBuf_` reads as `uint16_t[16]` with no trace of the
#      spelling. Only regex over source text could tell them apart, which is the fragile
#      approach this whole script exists to replace. Do not try again.
RULES = {
    "arrays": {
        "title": "RAM-costing fixed arrays",
        "output": "dump",
        # constexpr and static-storage arrays are excluded: they live in flash and cost no RAM.
        "matcher": ("namedDecl(anyOf("
                    "varDecl(hasType(constantArrayType()), "
                    "unless(anyOf(isConstexpr(), hasStaticStorageDuration()))), "
                    "fieldDecl(hasType(constantArrayType()))))"),
    },
    "scratch": {
        "title": "ScratchBuffer members (managed heap)",
        "output": "dump",
        "matcher": ('fieldDecl(hasType(cxxRecordDecl(hasName("ScratchBuffer"))))'),
    },
    "comments": {
        "title": "Comments per declaration (scope x kind)",
        "output": "dump",
        # `shadow: True` parses a copy of src/ where every leading `//` became `/// MMDEV: …`,
        # so ONE pass reports both kinds against the declaration each documents. Without it this
        # rule sees only `///` — 24% of the tree's comment lines.
        "shadow": True,
        # The scopes that can actually carry a comment: class/struct, method, and class ATTRIBUTE.
        # parmVarDecl is deliberately absent — a C++ parameter cannot hold a doc comment (1054
        # probed, zero with one), it is documented via `@param` in the method's own comment, and
        # this tree uses `@param` 8 times, all in a .js file. Reporting 1054 permanent violations
        # would bury every other row.
        # `unless(isLambda())` on the record, and `unless(ofClass(isLambda()))` on the method:
        # a lambda written INSIDE a function body is code, not a documented declaration, and
        # its compiler-synthesised closure class + call operator would otherwise be reported
        # as 53 undocumented "methods". The AST knows the difference — this asks it, rather
        # than testing the name, so a REAL `operator()` on a named functor class is still
        # reported.
        "matcher": ("namedDecl(anyOf(cxxRecordDecl(unless(isLambda())), "
                    "cxxMethodDecl(unless(ofClass(cxxRecordDecl(isLambda())))), "
                    "cxxConstructorDecl(), cxxDestructorDecl(), cxxConversionDecl(), "
                    "fieldDecl(), enumDecl(), "
                    "enumConstantDecl(), "
                    "functionDecl(unless(cxxMethodDecl(ofClass(cxxRecordDecl(isLambda())))))), "
                    'unless(isExpansionInSystemHeader())).bind("d")'),
    },
    "heap": {
        "title": "Heap allocation sites",
        # `dump`, not `diag`: diag gives a location with no expression and print gives an
        # expression with no location. Only dump carries both, which is what makes a per-site
        # listing possible rather than just a per-file count.
        "output": "dump",
        # `stmt(...)` is REQUIRED: a bare top-level anyOf() of Stmt matchers is ambiguous
        # ("unresolved overloaded type") and clang-query then matches NOTHING while still
        # exiting 0 — a silent zero, the same trap as the clang-tidy flag forms.
        # `unless(cxxMethodDecl())` matters: hasAnyName("free") also matches our OWN member
        # methods called free() (MoonLive, Buffer, MappingLUT all have one), which are not heap
        # deallocation at all. Measured on main.cpp: 62 matches with them, 47 without — 15 false
        # positives pointing at the wrong lines.
        # `hasAncestor(functionDecl().bind("fn"))` names the ENCLOSING function, so the report
        # says which code allocates rather than only which line. clang-query then emits two
        # blocks per match ("fn" then "root"), which is why collect_heap parses per-match.
        "matcher": ("stmt(anyOf(cxxNewExpr(), cxxDeleteExpr(), callExpr(callee(functionDecl("
                    # The project's OWN allocator belongs here, not just libc's: platform::alloc
                    # is how 35 sites in src/ acquire memory, and listing only `free` made the
                    # table read as "8 frees, 0 allocations" on HueDriver — an implied leak that
                    # was not there. allocInternal pairs with the ordinary free() (platform.h:63);
                    # allocExec/freeExec are their own pair.
                    'hasAnyName("malloc","calloc","realloc","free","strdup",'
                    '"alloc","allocInternal","allocExec","freeExec",'
                    '"heap_caps_malloc","heap_caps_calloc","heap_caps_realloc","heap_caps_free",'
                    '"ps_malloc","ps_calloc"), unless(cxxMethodDecl()))))), '
                    'hasAncestor(functionDecl().bind("fn")))'),
    },
}


def module_files(module):
    """The source files that ARE a module: src/**/<Module>.h and .cpp.

    The MoonDeck dropdown offers module names (`AudioService`, `Control`), which map 1:1 onto
    filenames in this codebase — one module, one header, optionally one .cpp. Returns [] for an
    unknown name so the caller can say so rather than silently reporting on everything.
    """
    hits = []
    for suffix in (".h", ".cpp"):
        hits += [p for p in (ROOT / "src").rglob(f"{module}{suffix}")]
    return sorted(str(p.relative_to(ROOT)) for p in hits)


def including_tus(files, build_dir):
    """The translation units that reach `files` — the TUs worth parsing to analyze them.

    A header is not a TU: it is compiled through whichever .cpp includes it. Parsing every TU to
    reach one header costs minutes (263s here) when a single TU usually suffices (18s), so this
    resolves the .cpp files whose include graph contains the header, transitively.

    Resolution is textual (`#include "<name>"`, followed through intermediate headers) rather
    than from the compiler's depfiles, because those only exist after a build with -MD and would
    make the report depend on how the tree was last built. A missed edge costs coverage, so the
    caller falls back to every TU when this finds nothing.
    """
    tus = _source_tus(build_dir)
    wanted = {Path(f).name for f in files}
    if not wanted:
        return tus

    # header name -> the files that include it, so we can walk the graph upward.
    src = list((ROOT / "src").rglob("*.h")) + list((ROOT / "src").rglob("*.cpp"))
    includes = {}
    for f in src:
        try:
            body = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        includes[f] = {m.group(1).rsplit("/", 1)[-1]
                       for m in re.finditer(r'#\s*include\s+"([^"]+)"', body)}

    reached, frontier = set(wanted), set(wanted)
    while frontier:
        nxt = set()
        for f, inc in includes.items():
            if inc & frontier and f.name not in reached:
                reached.add(f.name)
                nxt.add(f.name)
        frontier = nxt

    hits = [tu for tu in tus if Path(tu).name in reached]
    return hits or tus          # no edge found -> analyze everything rather than nothing


def _source_tus(build_dir):
    """The src/ translation units to analyze.

    Only src/: test/ TUs would double the runtime to report on code that never ships, and a
    header is analyzed through whichever .cpp includes it (hence the dedupe below).
    """
    db = json.loads((build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    return sorted({e["file"] for e in db if "/src/" in e["file"].replace("\\", "/")})


# The marker a `//` comment carries once it has been rewritten into a doc comment for parsing.
# Plain text on purpose: an `@`-prefixed marker is parsed as a doc COMMAND and stripped from the
# text, so `@MMDEV` vanishes and the two kinds become indistinguishable again. This survives.
_DEV_MARK = "MMDEV:"

# Leading `//` only — never `///` (already a doc comment) and never a trailing `x = 1; // note`
# (it documents nothing, and rewriting it would change the code line).
_LEADING_DEV = re.compile(r"^([ \t]*)//(?!/)( ?)", re.M)


def _shadow_tree(build_dir):
    """A copy of `src/` where every leading `//` is a MARKED doc comment.

    Clang's lexer DISCARDS `//`: only `///` and `/** */` become AST nodes, because the compiler
    consumes those itself. That leaves 76% of this tree's comment lines invisible to any matcher.
    Rewriting them into `/// MMDEV: …` makes them parse, and the marker keeps them separable from
    real doc comments — so one AST pass can report BOTH kinds against the declaration each one
    documents, which is the whole point (a `//` on a class is a different finding from a `///`).

    The rewrite happens in a SHADOW COPY under the build dir; `src/` is never touched. That is the
    line between this and a bad idea: the report describes the real tree, and the only thing the
    marker changes is which lexer bucket a comment lands in.

    Two things probed and settled, recorded so they are not re-derived:
      - `-Wdocumentation` WOULD fabricate warnings on rewritten dev notes (a `// @param wrong`
        becomes a real diagnostic). It does not matter here: clang-query never enables it.
      - Function PARAMETERS cannot carry a doc comment in C++ at all — 1054 ParmVarDecls in one TU,
        zero with a comment, and `@param` appears 8 times in the tree (all in a .js file). Class
        ATTRIBUTES (FieldDecl) are the per-member scope that does exist, and they are reported.
    """
    shadow = build_dir / "comment-shadow"
    # Reused within a run: the comments rule and the visibility pass both need it, and rebuilding
    # (rmtree + copytree + rewrite of all of src/) twice per run bought nothing — the source
    # cannot change mid-run. A stale tree from a PREVIOUS run would be wrong, so the marker file
    # records which source state it was built from.
    stamp = shadow / ".built-from"
    # A FINGERPRINT of every source path + its size and mtime, not just the newest mtime. Max-mtime
    # cannot see a DELETION (deleting anything but the newest leaves the max unchanged) nor an edit
    # that restores an older timestamp — in both cases the shadow tree stays stale and the report
    # describes source that is no longer there. Path and size are in the digest too, so a rename or
    # a same-mtime content change also invalidates.
    #
    # Hashing metadata rather than file CONTENT is deliberate: it is one stat() per file instead of
    # reading all of src/, and the failure it must catch is a stale tree between runs, not a
    # deliberate forgery.
    digest = hashlib.sha256()
    for f in sorted((ROOT / "src").rglob("*")):
        if f.is_file():
            st = f.stat()
            digest.update(f"{f.relative_to(ROOT)}|{st.st_size}|{st.st_mtime_ns}\n".encode())
    fingerprint = digest.hexdigest()
    if stamp.exists() and stamp.read_text(encoding="utf-8").strip() == fingerprint:
        return shadow
    if shadow.exists():
        shutil.rmtree(shadow)
    shutil.copytree(ROOT / "src", shadow / "src")
    for f in list((shadow / "src").rglob("*.h")) + list((shadow / "src").rglob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        marked = _LEADING_DEV.sub(rf"\1/// {_DEV_MARK}\2", text)
        if marked != text:
            f.write_text(marked, encoding="utf-8")
    stamp.write_text(fingerprint, encoding="utf-8")
    return shadow


def _run_rule(rule, tus, build_dir, tool):
    """Run one matcher over every TU, in parallel.

    clang-query has no parallel runner of its own (unlike run-clang-tidy) and costs ~44s per
    TU here, so serial would be ~11 minutes for a job that takes ~50s across cores.
    """
    query = f"set output {rule['output']}\nm {rule['matcher']}\n"
    qfile = build_dir / "clang-query-rule.txt"
    qfile.write_text(query, encoding="utf-8")

    cmd = [tool, "-p", str(build_dir), "-f", str(qfile)]
    cmd += [f"--extra-arg={a}" for a in check_clang_tidy._toolchain_args()]

    # The comments rule parses the shadow tree instead, so `//` is visible. The include path is
    # prepended so a TU's `#include "core/Foo.h"` resolves to the rewritten header, not the real
    # one — without it only the .cpp itself would be marked and every header would read as clean.
    shadow = None
    if rule.get("shadow"):
        shadow = _shadow_tree(build_dir)
        # `--extra-arg-before`, NOT `--extra-arg`: the latter APPENDS, so the compile
        # database's own `-I…/src` was searched first and every transitive include resolved
        # to the REAL, unmarked header. Only first-level quoted includes hit the shadow, so
        # `//` counts were a systematic undercount — a silent zero for most shared headers.
        cmd += [f"--extra-arg-before=-I{shadow / 'src'}"]
        tus = [str(shadow / Path(tu).resolve().relative_to(ROOT)) for tu in tus]

    def one(tu):
        p = subprocess.run(cmd + [tu], cwd=ROOT, capture_output=True, text=True)
        return p.stdout + p.stderr

    with ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 2)) as ex:
        out = "".join(ex.map(one, tus))
    if shadow:
        # Paths in the dump point into the shadow; map them back so a row is clickable.
        out = out.replace(str(shadow / "src"), str(ROOT / "src"))
    return out


@functools.lru_cache(maxsize=None)
def _rel(path):
    """Repo-relative path, or None for anything outside src/ (SDK and vendored headers)."""
    p = Path(path)
    if p.is_absolute():
        # Resolved like ROOT, so a symlinked checkout or a drive letter in another case still matches.
        try:
            p = p.resolve().relative_to(ROOT)
        except ValueError:
            return None
    rel = p.as_posix()
    return rel if rel.startswith("src/") else None


def _truncate(rows, max_rows):
    """The rows to print, plus a note when some were dropped.

    Always announces the cut: a table that silently stops at 20 reads as "that is all there is",
    which is the same class of lie as a silent zero. Rows are already worst-first, so the ones
    kept are the ones worth acting on.
    """
    if not max_rows or len(rows) <= max_rows:
        return rows, []
    hidden = len(rows) - max_rows
    return rows[:max_rows], [
        "",
        f"  … {hidden} more not shown (--max-rows {max_rows}). Use --max-rows 0 for all, "
        f"or --module <name> to scope."]


def collect_arrays(out, min_bytes):
    """Array declarations over the byte threshold, deduplicated by declaration site.

    A header included by N translation units yields N identical AST nodes, so the key is
    (file, line, name) — the declaration itself, not the times it was parsed.
    """
    rows = {}
    for m in _DECL.finditer(out):
        rel = _rel(m["path"])
        if not rel:
            continue
        elem = m["type"].strip()
        count = int(m["count"])
        size = ELEM_BYTES.get(elem, DEFAULT_ELEM_BYTES) * count
        if size <= min_bytes:
            continue
        rows[(rel, int(m["line"]), m["name"])] = {
            "file": rel, "line": int(m["line"]), "name": m["name"],
            "elem": elem, "count": count, "bytes": size,
            "where": "member" if m["kind"] == "FieldDecl" else "local",
        }
    return sorted(rows.values(), key=lambda r: -r["bytes"])


def collect_heap(out):
    """Allocation sites with what they are, what they touch, and the function they sit in.

    Parsed from the AST dump rather than `set output diag`: diag gives a location with no
    expression and `print` gives an expression with no location — only dump has both, plus the
    bound ancestor. clang-query emits one block per binding, so a match looks like

        Match #7:
        Binding for "fn":     <the enclosing function, with its whole body below it>
        Binding for "root":   <the allocation node, with its operand below it>

    which is why this splits on `Match #` and reads the two blocks separately rather than
    scanning line by line — the "fn" block contains allocation nodes of its own (every other
    site in the same function), and a line-wise scan would attribute them to the wrong match.
    """
    rows = {}
    for block in re.split(r"^Match #\d+:", out, flags=re.M)[1:]:
        parts = re.split(r'^Binding for "(\w+)":$', block, flags=re.M)
        # parts: [pre, name1, body1, name2, body2, ...]
        bind = {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}
        root, fn_block = bind.get("root", ""), bind.get("fn", "")
        if not root:
            continue

        lines = root.strip().splitlines()
        if not lines:
            continue
        head = lines[0]
        m = _HEAP_NODE.search(head)
        if not m:
            continue
        rel = _rel(m["path"])
        if not rel:
            continue

        sub = "\n".join(lines[1:])
        kind = m["kind"]
        if kind == "CXXNewExpr":
            what = "new[]" if " array " in head else "new"
            ty = _NEW_TYPE.search(head)
            target = ty["type"] if ty else ""
        elif kind == "CXXDeleteExpr":
            what = "delete[]" if " array " in head else "delete"
            target = ""
        else:
            callee = _CALLEE.search(sub)
            what = callee["name"] + "()" if callee else "call"
            target = ""

        # What is allocated/freed. `free(buf)` and `free(nodes)` can share a line, so without
        # this the two are indistinguishable in the report.
        if not target:
            op = _OPERAND_MEMBER.search(sub) or _OPERAND_VAR.search(sub)
            target = op["name"] if op else ""

        # The enclosing function, qualified with its class where the dump reveals one.
        fn = ""
        fn_lines = fn_block.strip().splitlines()
        if fn_lines:
            fm = _FN_DECL.match(fn_lines[0])
            if fm:
                fn = fm["name"]
                cm = _FN_CLASS.search(fn_block)
                if cm and cm["cls"] not in fn:
                    fn = f"{cm['cls']}::{fn}"

        rows[(rel, int(m["line"]), int(m["col"]))] = {
            "file": rel, "line": int(m["line"]), "what": what,
            "target": target, "fn": fn,
        }
    return sorted(rows.values(), key=lambda r: (r["file"], r["line"]))


def collect_scratch(out):
    """ScratchBuffer members, deduplicated by declaration site.

    These are heap allocations the `heap` rule cannot see: the actual platform::alloc lives once
    inside ScratchBuffer.cpp, so a module declaring three of them shows zero allocation sites in
    its own source. Size is deliberately absent — a ScratchBuffer is sized at RUNTIME from the
    light count (that is the point of it), so there is no static number to report.
    """
    rows = {}
    for m in _SCRATCH.finditer(out):
        rel = _rel(m["path"])
        if not rel:
            continue
        rows[(rel, int(m["line"]), m["name"])] = {
            "file": rel, "line": int(m["line"]), "name": m["name"], "elem": m["elem"],
        }
    return sorted(rows.values(), key=lambda r: (r["file"], r["line"]))


def render_scratch(rows, max_rows=0):
    nfiles = len(set(r["file"] for r in rows))
    L = [f"{len(rows)} found in {nfiles} file{'s' if nfiles != 1 else ''}."]
    if not rows:
        return L

    name_w = min(max(len(r["name"]) for r in rows), 24)
    elem_w = min(max(len(r["elem"]) for r in rows), 20)
    L += ["", f"  {'MEMBER':<{name_w}}  {'ELEMENT':<{elem_w}}  FILE:LINE",
          f"  {'-' * name_w}  {'-' * elem_w}  {'-' * 30}"]
    shown, note = _truncate(rows, max_rows)
    for r in shown:
        L.append(f"  {r['name'][:name_w]:<{name_w}}  {r['elem'][:elem_w]:<{elem_w}}  "
                 f"{r['file']}:{r['line']}")
    return L + note


# Access is NOT in the AST dump's declaration line — clang prints `private` only as a separate
# AccessSpecDecl node, which a per-block parse never sees. So visibility comes from a second,
# cheap matcher pass listing the non-public declarations by file:line, and rows are tagged
# against that set.
#
# Why report private members at all, rather than filtering them out: doxygen never publishes
# them, so the "it should reach the module page" argument does not apply — but a bloated comment
# is still bloat, and clangd's hover shows it to whoever maintains the code. The VIS column lets
# a reader weigh a finding (a public method with no `///` is an API gap; a private one is a
# maintenance note) without dropping half the tree from the report.
_NONPUBLIC_MATCHER = ("namedDecl(anyOf(cxxRecordDecl(), cxxMethodDecl(), cxxConstructorDecl(), "
                      "cxxDestructorDecl(), cxxConversionDecl(), fieldDecl(), enumDecl(), "
                      "enumConstantDecl(), functionDecl()), "
                      "anyOf(isPrivate(), isProtected()), "
                      'unless(isExpansionInSystemHeader())).bind("d")')


def collect_nonpublic(out):
    """The (file, line) of every private/protected declaration in the dump."""
    seen = set()
    cur_path = None
    for block in out.split('Binding for "d":')[1:]:
        head = next((ln for ln in block.split("\n") if ln.strip()), "")
        hint = _PATH_HINT.search(head)
        if hint:
            cur_path = hint["path"]
        rel = _rel(cur_path) if cur_path else None
        owner = _OWNER.search(head)
        if rel and owner:
            seen.add((rel, int(owner["line"])))
    return seen


def collect_comments(out):
    """Every declaration, with the WORDS of `///` and `//` attached to it.

    Parsed per BOUND MATCH: `.bind("d")` makes clang-query emit one block per matched declaration,
    headed by that declaration, so the owner is stated rather than inferred. Inferring it
    positionally fails two ways — a class match dumps its whole subtree (so a member's comment
    hangs off the class), and a trailing `///<` attaches to the declaration BEFORE it.

    Only a DIRECT child of the block head is this declaration's own comment; a deeper FullComment
    belongs to a nested declaration, which has its own block.

    Deduplicated by (file, declaration line), and sizes are a MAX rather than a sum: a header
    parsed by several TUs yields the same comment once per TU, and adding them reported a
    129-line class comment as 258.
    """
    rows = {}
    cur_path = None
    for block in out.split('Binding for "d":')[1:]:
        lines = block.split("\n")
        head = next((ln for ln in lines if ln.strip()), "")

        hint = _PATH_HINT.search(head)
        if hint:
            cur_path = hint["path"]
        rel = _rel(cur_path) if cur_path else None
        owner = _OWNER.search(head)
        if not rel or not owner:
            continue
        # `implicit` — a lambda's closure class or a compiler-injected self-reference: anonymous,
        #   so the dump's only "name" is the literal word "definition".
        # `invalid`  — a declaration clang could not fully parse in THIS TU (it parses fine in the
        #   TU that owns it). Same anonymous shape, and it would overwrite the good row.
        if " implicit " in head or " invalid " in head:
            continue
        # A record with no body is a FORWARD DECLARATION (`class JsonSink;`) — there is nothing
        # to document, and the real class is reported from the header that defines it. Clang
        # marks a defined record with a trailing `definition`; without it, the two `class Foo;`
        # lines at the top of every module header showed up as undocumented classes.
        if _RECORD_HEAD.match(head) and not head.rstrip().endswith("definition"):
            continue


        # Both kinds accumulate: a `///` spec and a `//` note can sit on the same method, and
        # keeping only whichever the dump listed first would hide half of it. A declaration with
        # neither still earns a row — "undocumented" is the other half of the question.
        seen = {"///": 0, "//": 0}
        first_line = None
        for k, line in enumerate(lines):
            if "FullComment" not in line or not _TOP_CHILD.match(line):
                continue
            fc = _FULLCOMMENT.search(line)
            if not fc:
                continue
            # Three span forms, and the PATH one is not optional: the dump prints a full path
            # whenever the comment is the first node from a new file, which is exactly the case
            # for a declaration whose doc comment opens a header. Missing it reported two
            # correctly-documented methods as having no comment at all.
            begin = int(fc["pstart"] or fc["start"] or owner["line"])
            body = ""
            for sub in lines[k + 1:]:
                # `@moreinfo` ends the LEAD description; everything after it is relocated below
                # the member lists at render time and is not what the ideal measures.
                if _MOREINFO_NODE.search(sub):
                    break
                tc = _TEXTCOMMENT.search(sub)
                if tc:
                    body += tc["text"] + " "
                    continue
                # The subtree is not flat — ParagraphComment and friends wrap the text — so the
                # scan ends only at a node that is not part of a comment at all.
                if not _COMMENT_NODE.search(sub):
                    break
            kind = "//" if _DEV_MARK in body else "///"
            # The marker is not prose: strip it before counting, or every rewritten `//` block
            # would read one word longer per line than it is on disk.
            words = len(body.replace(_DEV_MARK, " ").split())
            seen[kind] = max(seen[kind], words)
            if first_line is None:
                first_line = begin

        # `= default` / `= delete` have no body to document — reporting them as undocumented is
        # noise. They still count when they DO carry a comment.
        if not seen["///"] and not seen["//"]:
            if " default" in head or " delete" in head:
                continue

        scope = _SCOPE.get(owner["kind"], "other")
        ideal = _IDEAL_DOC_WORDS.get(scope, _IDEAL_DOC_WORDS["other"])
        # MAX across TUs, not last-writer-wins. A header is parsed once per TU that includes it,
        # and a plain assignment let a later TU that saw fewer words overwrite an earlier one that
        # saw more — the docstring claimed max while the code overwrote.
        key = (rel, int(owner["line"]))
        prev = rows.get(key)
        if prev:
            seen["///"] = max(seen["///"], prev["doc_words"])
            seen["//"] = max(seen["//"], prev["dev_words"])
        rows[key] = {
            "file": rel, "line": first_line or int(owner["line"]),
            # The DECLARATION's line, distinct from `line` (where its comment starts) — the
            # visibility pass keys on the declaration, so the two must not be conflated.
            "decl_line": int(owner["line"]),
            "doc_words": seen["///"], "dev_words": seen["//"],
            "scope": scope,
            # DOC is a target, so the deviation is a signed percentage: 0% ideal, -100% absent,
            # no ceiling. DEV has no target — zero IS the ideal — so `dev_words` above is
            # reported raw, with no deviation column at all.
            "doc_deviation": 100 * (seen["///"] - ideal) / ideal,
            "name": owner["cname"] or owner["fname"] or "?",
        }
    # Absolute deviation: a bloated header and a missing comment are both wrong, in opposite
    # directions, and ranking by |deviation| surfaces the two together rather than burying one.
    return sorted(rows.values(), key=lambda r: -abs(r["doc_deviation"]))


def render_comments(rows, max_rows=0):
    """The DECL x kind matrix, then every declaration ranked by how far it sits from the ideal.

    The project's rule is that every class, method and attribute carries a short readable `///`
    — that is what moxydoc publishes — while `//` developer notes belong in the code lines rather
    than stacked on a declaration. Three states, three different fixes, so the matrix shows which
    way each kind of declaration is leaning.

    Sizes are WORDS, not lines: a line is a formatting accident (the same paragraph is 5 lines at
    100 columns and 9 at 60) while words are what a reader absorbs. Measured here, a comment line
    carries a median of 13 words in both kinds.

    DOC DEVIATION is the signed % difference from `_IDEAL_DOC_WORDS`; DEV is an absolute
    count because zero is the
    ideal there, and a ratio would report the best case (no note at all) as -100%.
    """
    L = [f"{len(rows)} found."]
    if not rows:
        return L

    scopes = ("class", "method", "attribute", "other")
    blank = {"///": 0, "//": 0, "none": 0}
    counts = {s: dict(blank) for s in scopes}
    for r in rows:
        c = counts.setdefault(r["scope"], dict(blank))
        if r["doc_words"]:
            c["///"] += 1
        elif r["dev_words"]:
            c["//"] += 1
        else:
            c["none"] += 1

    ideals = " · ".join(f"{s} {_IDEAL_DOC_WORDS[s]}" for s in ("class", "method", "attribute"))
    # Column names match the detail table below, so a reader carries one vocabulary down the
    # report rather than mapping "DOC ///" onto "DOC WORDS" halfway through.
    L += ["", f"  {'DECL':<10}  {'DOC':>7}  {'DEV':>7}  {'NONE':>7}  {'%DOC':>5}"
              f"    (ideal doc words: {ideals}; ideal dev words: 0)",
          f"  {'-' * 10}  {'-' * 7}  {'-' * 7}  {'-' * 7}  {'-' * 5}"]
    for s in scopes:
        c = counts.get(s, blank)
        total = c["///"] + c["//"] + c["none"]
        if total:
            L.append(f"  {s:<10}  {c['///']:>7}  {c['//']:>7}  {c['none']:>7}  "
                     f"{100 * c['///'] // total:>4}%")

    dev = sum(1 for r in rows if not r["doc_words"] and r["dev_words"])
    none = sum(1 for r in rows if not r["doc_words"] and not r["dev_words"])
    L += ["", f"  {dev} declarations carry only a `//` where the rule asks for `///`; "
              f"{none} carry no comment at all."]

    name_w = min(max(len(r["name"]) for r in rows), 30)
    # The public subset, called out because it is the part that SHIPS as documentation: doxygen
    # publishes only public members, so an undocumented public method is an API gap while a
    # private one is a maintenance note. Both are reported; the split says which is which.
    pub = [r for r in rows if r.get("vis") == "pub"]
    if pub:
        pub_doc = sum(1 for r in pub if r["doc_words"])
        L += [f"  Public surface: {pub_doc} of {len(pub)} documented "
              f"({100 * pub_doc // len(pub)}%) — the part doxygen publishes."]

    # DOC DEVIATION sits beside DOC WORDS: the percentage and the number it is derived from
    # belong together, and DEV WORDS is a separate measure. "Deviation", not "ratio" — a ratio is
    # a bare quotient (2.3x), this is a signed percentage difference from a target.
    L += ["", f"  {'DOC DEVIATION':>13}  {'DOC WORDS':>9}  {'DEV WORDS':>9}  {'VIS':<4}  "
              f"{'DECL':<9}  {'NAME':<{name_w}}  FILE:LINE",
          f"  {'-' * 13}  {'-' * 9}  {'-' * 9}  {'-' * 4}  {'-' * 9}  {'-' * name_w}  {'-' * 30}"]
    shown, note = _truncate(rows, max_rows)
    for r in shown:
        L.append(f"  {r['doc_deviation']:>+12.0f}%  {r['doc_words']:>9}  {r['dev_words']:>9}  "
                 f"{r.get('vis', '?'):<4}  {r['scope']:<9}  {r['name'][:name_w]:<{name_w}}  "
                 f"{r['file']}:{r['line']}")
    return L + note


def render_arrays(rows, min_bytes, max_rows=0):
    over = f" over {min_bytes} bytes" if min_bytes else ""
    L = [f"{len(rows)} found{over}."]
    if not rows:
        return L

    by_where = collections.Counter(r["where"] for r in rows)
    L += [f"{by_where['local']} local, {by_where['member']} member."]

    name_w = min(max(len(r["name"]) for r in rows), 30)
    decl_w = min(max(len(f"{r['elem']}[{r['count']}]") for r in rows), 22)
    L += ["", f"  {'BYTES':>7}  {'WHERE':<6}  {'DECLARATION':<{decl_w}}  {'NAME':<{name_w}}  FILE:LINE",
          f"  {'-' * 7}  {'-' * 6}  {'-' * decl_w}  {'-' * name_w}  {'-' * 30}"]
    shown, note = _truncate(rows, max_rows)
    for r in shown:
        decl = f"{r['elem']}[{r['count']}]"
        L.append(f"  {r['bytes']:>7}  {r['where']:<6}  {decl[:decl_w]:<{decl_w}}  "
                 f"{r['name'][:name_w]:<{name_w}}  {r['file']}:{r['line']}")
    return L + note


# Which side of the lifecycle a site is on. `realloc` acquires (it can move and grow), so it
# sits with the allocations; a bare `free`/`delete` releases.
_RELEASING = ("free()", "freeExec()", "delete", "delete[]", "heap_caps_free()")


def render_heap(rows, max_rows=0):
    """Two tables — what acquires memory, and what releases it.

    Split because the two answer different questions: the acquire list is where RAM comes from
    (and what the hot path must not do), the release list is what pairs with it. Reading them
    side by side is how an unpaired allocation shows up.
    """
    L = [f"{len(rows)} found."]
    if not rows:
        return L

    alloc = [r for r in rows if r["what"] not in _RELEASING]
    free = [r for r in rows if r["what"] in _RELEASING]

    def table(title, subset):
        if not subset:
            return ["", f"{title}: none."]
        kinds = collections.Counter(r["what"] for r in subset)
        what_w = min(max(len(r["what"]) for r in subset), 12)
        tgt_w = min(max((len(r["target"]) for r in subset), default=0), 24) or 1
        fn_w = min(max((len(r["fn"]) for r in subset), default=0), 44) or 1

        def clip(s, w):
            return s if len(s) <= w else s[: w - 1] + "…"

        out = ["", f"{title} — {len(subset)}:  "
                   + "  ".join(f"{w} ×{n}" for w, n in kinds.most_common()),
               "", f"  {'WHAT':<{what_w}}  {'TARGET':<{tgt_w}}  {'FUNCTION':<{fn_w}}  FILE:LINE",
               f"  {'-' * what_w}  {'-' * tgt_w}  {'-' * fn_w}  {'-' * 30}"]
        shown, note = _truncate(subset, max_rows)
        for r in shown:
            out.append(f"  {r['what']:<{what_w}}  {clip(r['target'], tgt_w):<{tgt_w}}  "
                       f"{clip(r['fn'], fn_w):<{fn_w}}  {r['file']}:{r['line']}")
        return out + note

    return L + table("ALLOCATE", alloc) + table("FREE", free)


def _matcher_rejected(out):
    """True when clang-query refused the matcher rather than finding nothing.

    A rejected matcher still exits 0 with no matches, so the two are indistinguishable by status —
    and its complaints do not all say `error:` (an ambiguous top-level `anyOf()` reports "Input
    value has unresolved overloaded type"). Shared by the primary rule and the visibility pass:
    the second had no validation at all, so a refused matcher there silently marked every row
    `pub` instead of failing.
    """
    bad = [ln for ln in out.splitlines()
           if "error: " in ln or "unresolved overloaded type" in ln or "not found" in ln]
    return bool(bad) and "binds here" not in out and "Match #" not in out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rule", choices=sorted(RULES), help="Run one rule only.")
    ap.add_argument("--min-bytes", type=int, default=DEFAULT_MIN_BYTES,
                    help=f"Array size threshold in bytes (default {DEFAULT_MIN_BYTES}).")
    ap.add_argument("--module", help="Report only findings in this module's source files.")
    ap.add_argument("--max-rows", type=int, default=DEFAULT_MAX_ROWS,
                    help=f"Longest table to print, worst first (default {DEFAULT_MAX_ROWS}; "
                         f"0 = no limit).")
    args = ap.parse_args()

    tool = check_clang_tidy._find_tool("clang-query")
    if not tool:
        print("clang-query not found. Install LLVM (brew install llvm) or add it to PATH.",
              file=sys.stderr)
        return 2

    build_dir = check_clang_tidy._host_build_dir()
    if not (build_dir / "compile_commands.json").exists():
        print(f"No compile_commands.json in {build_dir.relative_to(ROOT)} — "
              f"run `uv run moondeck/build/build_desktop.py` first.", file=sys.stderr)
        return 2

    # A module's HEADER is not a translation unit — it is analyzed through whichever .cpp
    # includes it. So --module narrows the REPORT, not the TU list; narrowing the TUs would
    # miss every header-only module, which is most of the light domain.
    only = None
    if args.module:
        only = module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}' "
                  f"(looked for src/**/{args.module}.h and .cpp).", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}")

    tus = _source_tus(build_dir)
    if only:
        scoped = including_tus(only, build_dir)
        if scoped and len(scoped) < len(tus):
            print(f"Parsing {len(scoped)} of {len(tus)} TUs: "
                  f"{', '.join(Path(f).name for f in scoped)}")
            tus = scoped
        print()
    if not tus:
        print("No src/ translation units in the compilation database.", file=sys.stderr)
        return 2

    for name in ([args.rule] if args.rule else sorted(RULES)):
        rule = RULES[name]
        out = _run_rule(rule, tus, build_dir, tool)

        # A matcher clang-query rejects prints a parse error and zero matches — which is
        # indistinguishable from a clean tree unless we say so. Same silent-zero trap that
        # cost us twice on clang-tidy.
        # A matcher clang-query cannot resolve yields zero matches and exit 0. Its complaints
        # do not all say "error:" — an ambiguous top-level anyOf() reports "Input value has
        # unresolved overloaded type" — so key on "no matches at all", which is never a real
        # result for these rules on this codebase.
        bad = [ln for ln in out.splitlines()
               if "error: " in ln or "unresolved overloaded type" in ln or "not found" in ln]
        if _matcher_rejected(out):
            print(f"[{name}] clang-query rejected the matcher: {bad[0].strip()}", file=sys.stderr)
            return 2

        print(f"=== {rule['title']} ===")
        if name == "arrays":
            rows = collect_arrays(out, args.min_bytes)
        elif name == "scratch":
            rows = collect_scratch(out)
        elif name == "comments":
            rows = collect_comments(out)
            # A zero here is indistinguishable from a shadow tree that never got included: the
            # first version appended `--extra-arg`, so every transitive header resolved to the
            # REAL unmarked file and `//` counts silently halved. If NOTHING carries a dev
            # comment, the rewrite did not reach the headers — say so instead of reporting it.
            # NO `rows and` guard: an EMPTY result is the same false zero, only more so — the
            # matcher found not one declaration in a tree of thousands. Skipping the check when
            # rows was empty let the worst case through as a clean report.
            if not any(r["dev_words"] for r in rows):
                print("[comments] no `//` comment found anywhere — the shadow tree did not reach "
                      "the headers (include order?). Refusing to report a false zero.",
                      file=sys.stderr)
                return 2
            # Second pass for visibility — see collect_nonpublic. Cheap next to the main run:
            # same TUs, a matcher with no comment traversal.
            vis_out = _run_rule({"output": "dump", "matcher": _NONPUBLIC_MATCHER, "shadow": True},
                                tus, build_dir, tool)
            # Fail closed, exactly as the primary query does: an empty visibility result would mark
            # EVERY row `pub`, which reads as a fully-public API rather than as a failed query.
            if _matcher_rejected(vis_out):
                print("[comments] clang-query rejected the visibility matcher; VIS cannot be "
                      "reported. Refusing to mark every declaration public.", file=sys.stderr)
                return 2
            nonpub = collect_nonpublic(vis_out)
            for r in rows:
                r["vis"] = "priv" if (r["file"], r["decl_line"]) in nonpub else "pub"
        else:
            rows = collect_heap(out)
        if only:
            rows = [r for r in rows if r["file"] in only]
        body = (render_arrays(rows, args.min_bytes, args.max_rows) if name == "arrays"
                else render_scratch(rows, args.max_rows) if name == "scratch"
                else render_comments(rows, args.max_rows) if name == "comments"
                else render_heap(rows, args.max_rows))
        print("\n".join(body))
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
