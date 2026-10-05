#!/usr/bin/env python3
"""Hot-path discipline, checked by the compiler: what the render path calls that can block.

`MoonModule::tick/tick20ms/tick1s` carry `MM_NONBLOCKING` (platform.h). Clang 20+ then verifies
under `-Wfunction-effects` that nothing they reach allocates or blocks — TRANSITIVELY, through
the whole call graph — the half a regex over the tick body cannot see, being blind to what its
callees do.

Reports unique SITES. A header included by N translation units yields N copies of the same
warning, so a raw build prints ~1350 lines for ~175 real findings; deduplicating on
(file, line) is most of this script's value.

Two engines, each doing what it is good at. The compiler finds the blocking calls, because
`-Wfunction-effects` is transitive and a matcher would have to rebuild the call graph to match
it. clang-query then annotates each site with the branch that guards it (the COND column) —
a purely local AST question. Sites that run unconditionally sort first: they cost every tick,
where a guarded one may not run at all.

The count is the code report's: `check_code.py` runs this build and adds one "blocking call on
the render path" row per site to code.md, whose committed count may only fall. This script is the
detailed view of the same sites. `-Wno-error=function-effects` in CMakeLists keeps the build green,
since each finding is a judgment (fix it, annotate the callee, or accept it with a scoped reason)
and the answer differs per site.

Usage:
  uv run moondeck/check/check_nonblocking.py            # summary by callee, then every site
  uv run moondeck/check/check_nonblocking.py --module AudioService
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_clang_query  # noqa: E402  — the module→files resolver, one owner
import check_clang_tidy   # noqa: E402  — build-dir resolution, one owner

# `path:line:col: warning: <msg> [-Wfunction-effects]`
_WARN = re.compile(r"^(?P<file>\S+?):(?P<line>\d+):(?P<col>\d+): warning: "
                   r"(?P<msg>.*?) \[-Wfunction-effects\]")
_CALLEE = re.compile(r"non-'nonblocking' function '(?P<name>[^']+)'")

# Clang follows each warning with a `note:` saying WHY the callee is not nonblocking — either it
# reaches another blocking function, or it has a construct that rules it out (a static local
# needs a guard variable and a lock). That note is the difference between "this line is flagged"
# and "here is what to fix", so it becomes the WHY column.
_NOTE = re.compile(r"note: function cannot be inferred 'nonblocking' because it "
                   r"(?:calls non-'nonblocking' function '(?P<via>[^']+)'|(?P<reason>.+))")

MAX_ROWS = 40

# Shown when clang gave no root cause (a leaf it could not look inside). Named rather than
# inlined: a backslash escape inside an f-string EXPRESSION is a syntax error before 3.12.
NO_CAUSE = "\u2014"

# The enclosing method of a call site. Clang names the call and the callee but NOT the function
# they sit in, and that is the column that says which tick tier is affected — tick() every frame
# vs tick1s() once a second is an order of magnitude difference in what a blocking call costs.
# Walked backwards from the site to the nearest definition at class-member indent.
# Excludes control-flow keywords, which otherwise match `if (...) {` and report `IN: if`.
# Handles both in-class definitions and out-of-line `void Foo::tick() {`.
_KEYWORDS = ("if", "for", "while", "switch", "catch", "else", "do", "return")
_DEF = re.compile(r"^(?P<indent>\s*)(?:[\w:<>,&*\s]+?\s)?(?:(?P<cls>\w+)::)?(?P<name>\w+)\s*"
                  r"\([^;]*\)\s*(?:const\s*)?(?:MM_NONBLOCKING\s*)?(?:override\s*)?"
                  r"(?:noexcept\s*)?\{")

# Which tick tier a function belongs to, once resolved through the enclosing method.
TIERS = ("tick", "tick20ms", "tick1s")

# ---------------------------------------------------------------- guard form (COND)
#
# Whether a flagged call runs every time its tick does. `-Wfunction-effects` answers
# "can this block", never "is it reached" — so a report without this column ranks a call behind
# `if (++n >= 50)` (fires twice a second) level with one that runs unconditionally every frame.
#
# Asked of clang-query rather than scanned from the text, because the branch forms in this tree
# defeat a text scan: `if constexpr` (AudioService.h — a text scan reads it as a plain `if`),
# conditions spanning lines, and macros. The ancestor of a call is exactly the kind of LOCAL AST
# fact clang-query is for. Detection stays with the compiler: -Wfunction-effects is TRANSITIVE
# through the call graph, which a matcher would have to rebuild by hand.
#
# GUARDED IS NOT RARE. `if (enabled_)` is conditional and true every frame; only the rate hint
# below separates those, and it is a heuristic — read it as a lead, not a verdict.

# One matcher per branch kind, each naming the guard from the OUTSIDE in (`ifStmt(hasDescendant(
# callExpr))`) rather than from the call outwards. `hasAncestor(...).bind()` is rejected outright
# — "Matcher does not support binding" — so the kind cannot be read back from a combined query;
# running them separately is what makes the answer unambiguous.
#
# Cheap enough to do this way: measured ~1s per TU (a narrow matcher, unlike the comments rule's
# ~44s whole-subtree dump), so seven passes over the TUs holding findings add seconds to a report
# whose cost is dominated by the clean rebuild.
#
# Ordered most-specific first: the first form to claim a site keeps it, so a call in a loop body
# reads as `loop` even when an `if` also encloses it — the loop says more about how often it runs.
# `forEachDescendant`, NOT `hasDescendant`: the latter binds only the FIRST matching descendant
# per guard, so a branch containing several calls reported one and left the rest reading as
# unconditional. Measured on the `for` at DevicesModule.h:274 — `hasDescendant` bound only
# mergePacket (:277) while recvFrom (:275) in the same loop came back blank.
#
# Each matcher names the GUARDED REGION, never the whole statement. A call in an `if`'s CONDITION
# runs every time the `if` is reached — `if (!listener_.open()) return;` (DevicesModule.h:385) is
# an unconditional call to a blocking function — so matching the bare `ifStmt` labeled it `if`
# and sorted it below the unconditional rows, i.e. wrong in the reassuring direction. Same for a
# loop's condition, a `switch`'s subject, and the LHS of `&&`/`||`, which is always evaluated.
_GUARD_MATCHERS = (
    ("loop", "forStmt(hasBody(forEachDescendant(callExpr().bind(\"c\"))))"),
    ("loop", "whileStmt(hasBody(forEachDescendant(callExpr().bind(\"c\"))))"),
    ("loop", "cxxForRangeStmt(hasBody(forEachDescendant(callExpr().bind(\"c\"))))"),
    ("if", "ifStmt(anyOf(hasThen(forEachDescendant(callExpr().bind(\"c\"))),"
           "            hasElse(forEachDescendant(callExpr().bind(\"c\")))))"),
    # `switchCase`, not `switchStmt`: the latter has no `hasBody` (clang-query rejects it), and
    # matching the whole statement would claim calls in the SUBJECT expression, which always runs.
    # `switchCase` covers `case` and `default` alike — the bodies, which is the guarded region.
    ("switch", "switchCase(forEachDescendant(callExpr().bind(\"c\")))"),
    # Only the branches, not the condition.
    ("?:", "conditionalOperator(anyOf("
           "  hasTrueExpression(forEachDescendant(callExpr().bind(\"c\"))),"
           "  hasFalseExpression(forEachDescendant(callExpr().bind(\"c\")))))"),
    # `a && blocking()` — the RHS runs only if the left side allows it; the LHS always runs.
    ("&&", "binaryOperator(hasAnyOperatorName(\"&&\", \"||\"),"
           " hasRHS(forEachDescendant(callExpr().bind(\"c\"))))"),
)

# A guarded early exit: `if (...) return;` / `continue` / `break`. These guard everything BELOW
# them in the function, which no ancestor matcher can see — the call is genuinely unnested, so
# every matcher above reports nothing and the site reads as "runs every time".
#
# That was actively wrong, not merely incomplete. HueDriver::tick calls pushOneChangedLight
# unnested (HueDriver.h:112) but four early returns above it — including a `now - lastPutMs_ <
# kPutIntervalMs` rate limit — mean it runs at most once per interval, not once per frame.
#
# `returnStmt` ONLY. `continue`/`break` leave the enclosing LOOP, not the function, so one above
# a call — in a drain loop that has already closed, say — guards nothing about that call, and
# counting it labeled an unconditional site as guarded. A call genuinely inside the loop body is
# the `loop` matcher's to claim.
_EXIT_MATCHER = ("functionDecl(forEachDescendant("
                 "  returnStmt(hasAncestor(ifStmt())).bind(\"c\")))")

# The bound call's own header line in a dump, whose start position is the join key onto a
# warning: `CallExpr 0x7a0ee05e8 </abs/path/File.h:369:11, col:25> 'int'`. Three location spellings
# appear — a full path, a bare `line:369:11` when the file is unchanged from the previous node,
# and `col:11` when the line is too — so file and line are both optional and inherited when absent.
_CALL_LOC = re.compile(r"^\w+ 0x\w+ <(?:(?P<file>(?!line:|col:)[^:<>]+):)?"
                       r"(?:line:(?P<line>\d+)|(?P<full>\d+)|col:\d+)")

# A rate-limiting guard: a counter compared against a constant or a `k`-prefixed limit, the
# `if (++broadcastTick_ >= kBroadcastEverySec)` shape. Also a once-only latch (`if (!inited_)`),
# which runs its body exactly once for the life of the process.
#
# `> 0` is deliberately NOT a rate limit: `if (pairTicksLeft_ > 0)` is a plain non-empty test, and
# accepting any integer literal marked four unrelated guards in HueDriver::tick1s as throttled.
# A threshold is a named `k` constant or a literal of 2 or more.
_LIMIT = r"(?:k[A-Z]\w*|[2-9]\d*|\d{2,})"
_RATE = re.compile(rf"(\+\+|--)\s*\w+\s*(>=|>|==)|"          # ++n >= limit
                   rf"\w+\s*(>=|>)\s*{_LIMIT}\b|"            # n >= kLimit / n >= 50
                   rf"[-+]\s*\w+\s*(<|<=)\s*{_LIMIT}\b|"     # now - last < kIntervalMs
                   rf"%\s*{_LIMIT}\s*==")                    # n % kEvery == 0
#
# The latch word must END the identifier (`inited_`, `sawLights`, `isOpen`) rather than sit inside
# it: a trailing `\w*` let `open` match `online`, so `if (!online) return;` — a plain connectivity
# guard — marked DevicesModule's ageOut call as rate-limited.
_LATCH_WORD = r"(?:inited|ready|open|opened|started|done|valid)"
_LATCH = re.compile(rf"!\s*\w*{_LATCH_WORD}_?\b|"
                    rf"\w*{_LATCH_WORD}_?\s*==\s*false")


def _src_lines(rel_path, _cache={}):
    """The file's lines, cached per path; `[]` when it does not exist.

    One reader for the three text heuristics (`_rate_on_line`, `rate_hint`, `_enclosing_def`) —
    they each carried an identical read-and-memoize block, so a change to the caching rule had to
    be made in three places to stay correct.
    """
    src = _cache.get(rel_path)
    if src is None:
        f = ROOT / rel_path
        src = _cache[rel_path] = (f.read_text(encoding="utf-8", errors="replace").split("\n")
                                  if f.exists() else [])
    return src


def _rate_on_line(rel_path, line):
    """True when THIS one line spells a rate limiter or a once-only latch — no surrounding scan.

    Used for early exits, where the condition and the `return` share a line and any wider window
    would read a neighboring block that does not guard the call at all.
    """
    src = _src_lines(rel_path)
    if not 0 < line <= len(src):
        return False
    return bool(_RATE.search(src[line - 1]) or _LATCH.search(src[line - 1]))


# The matcher-rejection detector lives in check_clang_query: one home for the clang-query
# plumbing both reports share (this module already imports it for the TU list and paths).


def guard_forms(rows, build_dir, tool):
    """`(file, line) -> guard form` for every flagged site, from the AST.

    Runs one clang-query pass per branch kind over only the TUs that actually contain findings,
    then joins on the call's start position — the same key the warnings carry.

    Returns None when the answer could not be obtained — no clang-query, or a matcher it refused.
    The caller then renders the column as `?`, because a tool that could not look must never read
    as "nothing is guarded", which is the comfortable answer and the wrong one.
    """
    if not tool or not rows:
        # No findings means nothing to annotate. Bail BEFORE resolving TUs: `including_tus` falls
        # back to every TU when handed an empty set, so this would otherwise run the full matcher
        # sweep to label a table with no rows in it.
        return None
    # Every TU that includes a file with findings. Headers have no TU of their own — they are
    # analyzed through whichever .cpp includes them — so the set is resolved, not guessed.
    tus = check_clang_query.including_tus({r["file"] for r in rows}, build_dir)
    if not tus:
        return None

    def run(form, matcher):
        """Bound sites for one matcher, or None when clang-query could not answer."""
        try:
            out = check_clang_query._run_rule(
                {"output": "dump", "matcher": matcher}, tus, build_dir, tool)
        except Exception as exc:
            print(f"clang-query failed on the {form} matcher ({exc}); COND cannot be reported.",
                  file=sys.stderr)
            return None
        # A matcher clang-query cannot resolve yields ZERO matches and exit 0 — indistinguishable
        # from "no site is guarded". Same detection as check_clang_query.main; without it a single
        # matcher-syntax drift silently blanks the column and every row reads as unconditional.
        if check_clang_query._matcher_rejected(out):
            print(f"clang-query rejected the {form} matcher; COND cannot be reported.",
                  file=sys.stderr)
            return None
        return list(_bound_sites(out))

    found = {}
    for form, matcher in _GUARD_MATCHERS:
        sites = run(form, matcher)
        if sites is None:
            return None
        for rel, line in sites:
            found.setdefault((rel, line), form)

    # Early exits last: only a site that no enclosing branch claimed can be guarded this way,
    # and being INSIDE an `if` says more about how often a call runs than sitting after one.
    sites = run("ret", _EXIT_MATCHER)
    if sites is None:
        return None
    exits = {}
    for rel, line in sites:
        exits.setdefault(rel, set()).add(line)
    for r in rows:
        if (r["file"], r["line"]) in found:
            continue
        # An exit guards this call only if it sits ABOVE it in the SAME function, so the search
        # is bounded by the enclosing method's own first line.
        start = function_start(r["file"], r["line"])
        if not start:
            continue
        above = [ln for ln in exits.get(r["file"], ()) if start < ln < r["line"]]
        if above:
            # Test the EXIT lines themselves, not a window around them: an early exit's condition
            # is on its own line (`if (now - last < kInterval) return;`), and walking up from it
            # would pick up whatever unrelated block sits above — which marked ageOut (:286) as
            # rate-limited from a `++n >= k` block that had already closed at :284.
            found[(r["file"], r["line"])] = ("ret·rate" if any(
                _rate_on_line(r["file"], ln) for ln in above) else "ret")
    return found


def _bound_sites(out):
    """`(repo-relative file, line)` for every node bound as "c" in a clang-query dump.

    `dump`, not `print`: print emits the SOURCE TEXT of a match with no location, and the join
    onto a warning needs file:line. The bound node's own header line carries it.

    The file is tracked across blocks because a dump states a path once and then spells later
    positions `line:274:9` against it. Two traps that cost real coverage here:
     - it must hold the RAW path, not `_rel()` of it. `_rel` returns None outside src/, so
       filtering at assignment made one SDK match blank the file for every `line:`-only match
       after it — silently dropping our own sites (DevicesModule.h:275 read as unconditional
       while sitting inside a `for`).
     - it must reset per TU. `_run_rule` concatenates every TU's output, so a path carried across
       the seam labels one TU's matches with another's file.
    """
    for chunk in out.split("Match #1:"):
        cur = None
        for block in chunk.split('Binding for "c":')[1:]:
            head = next((ln for ln in block.split("\n") if ln.strip()), "")
            m = _CALL_LOC.search(head)
            if not m:
                continue
            if m["file"]:
                cur = m["file"]
            # A bare `col:` form inherits BOTH file and line from the enclosing context, so it
            # carries no line of its own and is skipped — guessing one would join the guard onto
            # the wrong warning, worse than leaving the column blank.
            line = m["line"] or m["full"]
            rel = check_clang_query._rel(cur) if cur else None
            if rel and line:
                yield rel, int(line)


def rate_hint(rel_path, line):
    """True when the guard above `line` looks like a rate limiter or a once-only latch.

    Read from the source text, not the AST: this asks what the condition MEANS, and a counter
    compared to a constant is a spelling question, not a structural one. Heuristic by nature —
    it can miss a rate limiter written another way, so it only ever ADDS a hint to a site the
    AST already called conditional.
    """
    src = _src_lines(rel_path)
    if not 0 < line <= len(src):
        return False
    # Walk UP out of the call's own block, stopping at the line that opened it — the guard.
    # `if (++n >= k) {` / `n = 0;` / `send();` is a common shape, so a fixed one-line window misses
    # the throttle (measured: DevicesModule.h:281 guards the call at :283, two lines down).
    #
    # Bounded by INDENTATION rather than a line count: only lines indented at least as far as the
    # call belong to its block, so the scan cannot wander into a sibling guard above. A fixed wider
    # window did exactly that — HueDriver::tick1s line 123's `++reportTick_ >= kReportEverySec`
    # marked the four unrelated guards above it as rate-limited.
    indent = len(src[line - 1]) - len(src[line - 1].lstrip())
    scan = [line - 1]
    for i in range(line - 2, max(-1, line - 12), -1):
        if not src[i].strip():
            continue
        scan.append(i)
        if len(src[i]) - len(src[i].lstrip()) < indent:
            break            # this line opened the block — the guard itself; stop here
    for i in scan:
        if i >= 0 and (_RATE.search(src[i]) or _LATCH.search(src[i])):
            return True
    return False


def _enclosing_def(rel_path, line):
    """`(name, 1-based line)` of the definition a call site sits in, or `("", 0)`."""
    src = _src_lines(rel_path)
    for i in range(min(line, len(src)) - 1, -1, -1):
        m = _DEF.match(src[i])
        if m and len(m.group("indent")) <= 4 and m.group("name") not in _KEYWORDS:
            return m.group("name"), i + 1
    return "", 0


def enclosing_function(rel_path, line):
    """The method a call site sits in, or "" when it cannot be determined."""
    return _enclosing_def(rel_path, line)[0]


def function_start(rel_path, line):
    """The line the enclosing definition opens on, or 0 — the lower bound when asking whether an
    early exit sits above a call. Without it, a `return` from the function ABOVE this one would
    read as guarding a call it cannot reach."""
    return _enclosing_def(rel_path, line)[1]


def function_effects_enabled(build_dir) -> bool:
    """Whether the build in `build_dir` carries `-Wfunction-effects`, read from its CMake cache rather than a rebuild."""
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    return re.search(r"^MM_HAS_WFUNCTION_EFFECTS:.*=(1|ON|TRUE|YES)\s*$", cache, re.IGNORECASE | re.MULTILINE) is not None


def build_output(build_dir, clean=True):
    """A full rebuild's warnings, or None if the build failed.

    `--clean-first` because an incremental build only recompiles what changed, and a cached TU
    prints nothing — which would read as "no findings". Likewise a FAILED build produces partial
    output: returning it would report a small number as if the tree were nearly clean, the same
    silent-zero trap the other checks guard against.
    """
    cmd = ["cmake", "--build", str(build_dir)] + (["--clean-first"] if clean else [])
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
    out = proc.stdout + proc.stderr
    if proc.returncode != 0:
        errors = [line for line in out.splitlines() if ": error:" in line]
        print(f"Build FAILED (exit {proc.returncode}) — findings would be incomplete.",
              file=sys.stderr)
        for error_line in errors[:5]:
            print(f"  {error_line.replace(str(ROOT) + '/', '')}", file=sys.stderr)
        return None

    # FAIL CLOSED. -Wfunction-effects exists only on Clang 20+; CMake silently omits the flag
    # on anything else, and this script would then report "0 findings" from a build that never
    # ran the check: indistinguishable from a clean tree. A zero is only trustworthy if the
    # warning is actually enabled, so say so instead of reporting a comfortable number.
    #
    # INCREMENTAL asks a narrower question and needs a narrower guard. A run where nothing
    # recompiled legitimately produces no diagnostics, so an empty log there is not proof the
    # flag is missing. What it IS proof of is that nothing was measured, and a gate that prints
    # a tick for an empty read is the silent zero this whole function exists to prevent. So the
    # caller is told which of the two happened rather than being handed a clean bill either way.
    if not clean and "[-Wfunction-effects]" not in out:
        compiled = any(line.startswith("[") and ".cpp" in line for line in out.splitlines())
        print("Nothing was measured: this incremental build produced no -Wfunction-effects\n"
              + ("output, and no translation unit recompiled. Every file is cached, so this run\n"
                 "says nothing about the tree. Re-run without --incremental for the full picture.\n"
                 if not compiled else
                 "output even though something recompiled, which means the warning is not enabled\n"
                 "(it needs Clang 20+; CMake omits it silently otherwise).\n"),
              file=sys.stderr)
        return None
    if clean and "[-Wfunction-effects]" not in out:
        print("No -Wfunction-effects diagnostics in the build output.\n"
              "That means either the tree really is clean, or the compiler does not support the\n"
              "warning (it needs Clang 20+; CMake omits it silently otherwise). Check with:\n"
              f"  grep -n 'Wfunction-effects' {(ROOT / 'CMakeLists.txt').relative_to(ROOT)}\n"
              "  cmake --build <dir> --clean-first 2>&1 | grep -c function-effects",
              file=sys.stderr)
        return None
    return out


def collect(out):
    """Unique findings keyed on (file, line, col), each with the root cause clang gives.

    A warning line names the call; the `note:` line that follows names why that callee blocks.
    Both are needed to act on a finding, so they are carried together.
    """
    rows, last = {}, None
    for line in out.splitlines():
        clean = line.replace(str(ROOT) + "/", "")
        m = _WARN.match(clean)
        if m:
            f = m["file"]
            last = None
            if f.startswith(("src/", "test/")):
                c = _CALLEE.search(m["msg"])
                key = (f, int(m["line"]), int(m["col"]))
                # Two violation kinds: a call to something that blocks, or a construct that
                # blocks by itself (a static local needs a guard variable and a one-time lock).
                # The second has no callee, so name the construct instead of truncating the
                # warning text into the CALLS column.
                rows.setdefault(key, {
                    "file": f, "line": int(m["line"]),
                    "callee": c["name"] if c else "(static local variable)",
                    "why": "" if c else "guard variable + one-time lock on first use",
                    "fn": enclosing_function(f, int(m["line"])),
                })
                last = key
            continue
        # The note belongs to the warning just above it.
        if last:
            n = _NOTE.search(clean)
            if n and not rows[last]["why"]:
                rows[last]["why"] = (f"calls {n['via']}" if n["via"]
                                     else n["reason"].strip().rstrip("."))
    return sorted(rows.values(), key=lambda r: (r["file"], r["line"]))


def float_conversions_on_the_hot_path():
    """`formatTo` sites whose format string carries a float conversion, which allocates.

    The annotation on `formatTo` promises it neither blocks nor allocates, and that promise rests on the conversions its callers use: an integer or a string writes through the buffer, while a `%f` reaches `_dtoa_r` and then `_malloc_r` on the ESP32's newlib.
    The appendix in format.h states that; this is what holds it, because a reader checking by eye is not a guarantee.

    Reads the whole CALL rather than the call's first line. Seven sites in this tree put the format
    string on the line below `formatTo(`, so a line-by-line scan could not see their conversions at
    all: a `%f` there would have passed a gate that reported itself clean.
    """
    bad = []
    for path in sorted((ROOT / "src").rglob("*.h")) + sorted((ROOT / "src").rglob("*.cpp")):
        if "vendor" in path.parts:
            continue
        lines = path.read_text(errors="replace").splitlines()
        for n, line in enumerate(lines, 1):
            if "formatTo(" not in line:
                continue
            # The call's text, to its closing `;` or a short lookahead: a format string is split
            # across adjacent string literals often enough that the argument list, not the line,
            # is the unit to search.
            call = line
            for extra in lines[n:n + 8]:
                if ";" in call:
                    break
                call += extra
            if re.search(r"%[-+ #0-9.*]*[aAeEfFgG]", call):
                bad.append(f"{path.relative_to(ROOT)}:{n}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--module", help="Only findings in this module's source files.")
    ap.add_argument("--incremental", action="store_true",
                    help="Skip the clean rebuild — only files that changed are recompiled, so "
                         "this reports on THOSE. For a gate that asks 'did this change add a "
                         "blocking call', not for the full picture.")
    args = ap.parse_args()

    # BEFORE the build, because this is a grep rather than a compile: it answers in milliseconds,
    # and a `%f` at a formatTo site breaks the annotation's promise whatever the build then finds.
    floats = float_conversions_on_the_hot_path()
    if floats:
        print("A float conversion at a formatTo site, which allocates on newlib:")
        for f in floats:
            print(f"  {f}")
        print("  formatTo promises no allocation, and a `%f` breaks that promise.\n")

    build_dir = check_clang_tidy._host_build_dir()
    if not (build_dir / "CMakeCache.txt").exists():
        print(f"No build in {build_dir.relative_to(ROOT)} — run "
              f"`uv run moondeck/build/build_desktop.py` first.", file=sys.stderr)
        return 2

    # NOT APPLICABLE is not the same as FAILED. -Wfunction-effects is Clang 20+ only, and
    # CMakeLists probes for it (`check_cxx_compiler_flag`) inside the non-MSVC branch, so an MSVC
    # cache carries no MM_HAS_WFUNCTION_EFFECTS at all. Without the warning this check can never
    # produce a finding, and building to discover that wastes a full rebuild and then reports a
    # failure the toolchain guarantees. Reading the cache says the same thing for free.
    #
    # Skipping is safe here in a way it would not be for a normal gate: the desktop build that
    # DOES carry the flag is the one every contributor and CI runs, so the tree is still measured,
    # though not from this host. Say so loudly rather than printing a number, because this script's
    # standing rule is that a comfortable zero from a check that never ran is the worst outcome.
    #
    # ONLY the compiler half is skipped. The float-conversion grep above needs no compiler, so it
    # has already run and printed on this host, and its verdict rides out on the same
    # `1 if floats else 0` every other exit uses. Returning a bare 0 here would print a broken
    # promise and then report success, which is the silent zero this script exists to refuse.
    if not function_effects_enabled(build_dir):
        # The cache, not the compiler: a Clang below 20, or a cache written before the probe
        # existed, lands here too, and naming MSVC alone would misdescribe both.
        print(f"SKIP: the build cache in {build_dir.relative_to(ROOT)} carries no "
              f"MM_HAS_WFUNCTION_EFFECTS=1\n(MSVC, a Clang below 20, or a cache older than the "
              f"probe), so the transitive hot-path\ncheck cannot run here. Reconfigure on a Clang "
              f"20+ host to run it, or read the\nclang-hotpath card, which CI keeps current. The "
              f"float-conversion findings above DID run.")
        return 1 if floats else 0

    out = build_output(build_dir, clean=not args.incremental)
    if out is None:
        return 2
    rows = collect(out)

    if args.module:
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}\n")
        rows = [r for r in rows if r["file"] in only]

    # Annotate each site with the branch that guards it. Separate pass, separate tool: the
    # compiler says WHAT can block, the AST says WHETHER it is reached every tick.
    tool = check_clang_tidy._find_tool("clang-query")
    guards = guard_forms(rows, build_dir, tool)
    for r in rows:
        if guards is None:
            # Unknown, NOT unconditional. A missing or refused tool is not an answer, and `—`
            # would read as one.
            r["cond"] = "?"
        else:
            form = guards.get((r["file"], r["line"]), "")
            # `ret` is already final — guard_forms tested the exit lines themselves and appended
            # `·rate` if one of THEM throttles. Re-testing here would scan upward from the call
            # instead and pick up any unrelated block above it (measured: DevicesModule's ageOut
            # read `ret·rate` from a `++n >= k` block that had already closed two lines earlier).
            if not form or form.startswith("ret") or "·" in form:
                r["cond"] = form or NO_CAUSE
            else:
                r["cond"] = f"{form}·rate" if rate_hint(r["file"], r["line"]) else form

    # THE ABSOLUTE NUMBER, first, because it is the one with a criterion rather than a comparison.
    # A call with no guard runs on EVERY tick, so its target is zero and no list is needed to say
    # so; a guarded one may never be reached on the hot path and is a judgment per site. Reporting
    # only a diff against a frozen list made every finding equal and none of them actionable.
    n_uncond = sum(1 for r in rows if r.get("cond") == NO_CAUSE)
    print(f"{len(rows)} call(s) from the render path that can block or allocate.")
    print(f"{n_uncond} of them are UNCONDITIONAL, running on every tick. That number's target is "
          f"zero.")
    if not rows:
        print("\nNone. \u2713  (If that seems wrong, confirm the build actually recompiled \u2014 a "
              "cached TU prints no warnings.)")
        # The float verdict survives an empty report: a clean -Wfunction-effects run says nothing
        # about a `%f`, which the compiler cannot see through the annotation at all.
        return 1 if floats else 0

    # Split by tick tier: tick() runs every frame, tick1s() once a second, so the same blocking
    # call costs roughly two orders of magnitude more in one than the other (50/s vs 1/s).
    # Pooling them hides that.
    def tier_of(r):
        return r["fn"] if r["fn"] in TIERS else "other"

    def table(title, subset, note):
        if not subset:
            return
        # Unconditional first: a call that runs every time its tick does is strictly worse than
        # the same call behind a branch. (file, line) breaks ties, so the order stays stable
        # between runs and findings in one file stay together — that is how they get fixed.
        # Only NO_CAUSE leads: `?` is UNKNOWN, not proven-unconditional, so ranking it as urgently
        # would assert something the run did not establish. (It is also all-or-nothing — when the
        # guard query fails every row reads `?` — so it only ever affects a mixed table.)
        subset = sorted(subset, key=lambda r: (r["cond"] != NO_CAUSE, r["file"], r["line"]))
        # Never narrower than the header: a tier where every row is `—` is width 1, and the dashes
        # then stop lining up under "COND".
        cond_w = max(min(max(len(r["cond"]) for r in subset), 9), len("COND"))
        callee_w = min(max(len(r["callee"]) for r in subset), 36)
        fn_w = min(max(len(r["fn"] or "?") for r in subset), 18)
        why_w = min(max((len(r["why"]) for r in subset), default=0), 44) or 1
        loc_w = min(max(len(f"{r['file']}:{r['line']}") for r in subset), 44)

        def clip(s, w):
            return s if len(s) <= w else s[: w - 1] + "\u2026"

        print(f"\n{title} \u2014 {len(subset)}   ({note})")
        print(f"      {'COND':<{cond_w}}  {'CALLS':<{callee_w}}  {'IN':<{fn_w}}  "
              f"{'WHY IT BLOCKS':<{why_w}}  FILE:LINE")
        print(f"      {'-' * cond_w}  {'-' * callee_w}  {'-' * fn_w}  "
              f"{'-' * why_w}  {'-' * loc_w}")
        for r in subset[:MAX_ROWS]:
            why = r["why"] or NO_CAUSE
            print(f"      {clip(r['cond'], cond_w):<{cond_w}}  "
                  f"{clip(r['callee'], callee_w):<{callee_w}}  "
                  f"{clip(r['fn'] or '?', fn_w):<{fn_w}}  "
                  f"{clip(why, why_w):<{why_w}}  "
                  f"{r['file']}:{r['line']}")
        if len(subset) > MAX_ROWS:
            print(f"  \u2026 {len(subset) - MAX_ROWS} more. Use --module <name> to scope.")

    if guards is not None:
        print(f"\nCOND = the branch guarding the call ({NO_CAUSE} = none, runs every time). "
              f"`\u00b7rate` marks a\nrate limiter or once-only latch. Guarded is not rare: "
              f"`if (enabled_)` still runs every frame.")
    elif not tool:
        print("\nCOND = ? \u2014 clang-query not found, so no site could be checked for a guard. "
              "Install\nLLVM (brew install llvm) for the column. Unknown is not unconditional.")
    else:
        print("\nCOND = ? \u2014 the guard query did not complete (reason above), so no site could "
              "be\nchecked. Unknown is not unconditional.")

    for tier, note in (("tick", "every frame \u2014 the hot path"),
                       ("tick20ms", "50x a second"),
                       ("tick1s", "once a second \u2014 sub-hot path"),
                       ("other", "reached from a tick, tier not resolved")):
        table(tier + "()" if tier != "other" else "OTHER",
              [r for r in rows if tier_of(r) == tier], note)
    # The report itself is not a gate, but a float conversion at a formatTo site is: it breaks a
    # promise the annotation makes, rather than adding one more finding to a list.
    return 1 if floats else 0


if __name__ == "__main__":
    sys.exit(main())
