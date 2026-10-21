#!/usr/bin/env python3
"""CodeQL's open alerts, read from GitHub — the Security tab as a card.

CodeQL runs in CI (.github/workflows/codeql.yml), not locally: it is the one layer of the
analysis stack that sees whole-program taint, and it earns that slot on the six network packet
formats we parse. But its findings live behind the Security tab, which means they are invisible
while working — a report nobody opens is a report nobody reads.

This FETCHES rather than scans. The CodeQL CLI would need a ~1 GB install and minutes per run to
reproduce what CI already computed, and the alert lifecycle (open / fixed / dismissed, tracked
across runs) is the part worth having — that is baselining we would otherwise build ourselves.
The trade is honest and stated in the output: this shows the last ANALYZED commit, so local edits
and unpushed commits are not in it.

Not a gate, like the rest of the stack (docs/reference/testing.md § Static analysis): it reports, the human
judges. Exit is 0 whenever the fetch succeeded, whatever the findings — and non-zero only when the
answer is unknown (no `gh`, not authenticated, no network), because "I could not look" must never
render as "nothing found".

Usage:
  uv run moondeck/check/check_codeql.py                  # open alerts, worst first
  uv run moondeck/check/check_codeql.py --state fixed    # what has been resolved
  uv run moondeck/check/check_codeql.py --all            # every state, including dismissed
  uv run moondeck/check/check_codeql.py --module HueDriver
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_clang_query  # noqa: E402  — the module→files resolver, one owner
import _findings  # noqa: E402

# Worst first. CodeQL reports two different severity scales: `security_severity_level` on the
# security queries (critical/high/medium/low) and `severity` on the quality ones
# (error/warning/note/recommendation). They are merged into one ordering here, because a reader
# wants "what matters most", not two tables to compare by eye.
_ORDER = {"critical": 0, "high": 1, "error": 2, "medium": 3,
          "warning": 4, "low": 5, "note": 6, "recommendation": 7}

MAX_ROWS = 60


def _repo():
    """`owner/name` for the current checkout, or None when it cannot be determined."""
    # `check=False` does not cover the binary being ABSENT — that raises FileNotFoundError before
    # any exit code exists, which is the commonest cold-start failure and would print a traceback
    # instead of the install guidance the caller has ready.
    try:
        p = subprocess.run(["gh", "repo", "view", "--json", "nameWithOwner", "-q", ".nameWithOwner"],
                           cwd=ROOT, capture_output=True, text=True, check=False)
    except OSError:
        return None
    return p.stdout.strip() or None


def fetch(state):
    """Alerts for the repo, or `(None, reason)` when the answer could not be obtained.

    A failure is returned rather than raised so the caller can say WHY the list is empty. An
    empty list and an unreachable API look identical in a table, and only one of them means
    the code is clean.
    """
    repo = _repo()
    if not repo:
        return None, ("`gh` could not identify the repository. Install the GitHub CLI and run "
                      "`gh auth login`, or check that this is a GitHub checkout.")
    # `state` must be passed EXPLICITLY for every query. Omitting it does not mean "any state" —
    # the endpoint defaults to `open`, so an "all states" run that simply dropped the parameter
    # returned the open alerts and called them all of them. Verified against the API: with no
    # `state`, every returned alert has `"state": "open"`.
    states = ("open", "fixed", "dismissed") if state == "all" else (state,)
    out = []
    for st in states:
        query = f"per_page=100&state={st}"
        try:
            p = subprocess.run(["gh", "api", "--paginate",
                                f"repos/{repo}/code-scanning/alerts?{query}"],
                               cwd=ROOT, capture_output=True, text=True, check=False)
        except OSError as exc:
            return None, (f"Could not run `gh` ({exc}). Install the GitHub CLI and run "
                          f"`gh auth login`.")
        if p.returncode != 0:
            err = (p.stderr or "").strip().splitlines()
            detail = err[-1] if err else f"exit {p.returncode}"
            # 404 is the common one and does not mean "clean": code scanning may be off, or the
            # token may lack the security_events scope.
            return None, (f"Could not read the alerts ({detail}). Code scanning may be disabled "
                          f"for {repo}, or `gh auth` may lack the `security_events` scope.")
        # --paginate concatenates one JSON array per page; load them all.
        #
        # FAIL CLOSED on anything that is not a clean run of arrays. Blank stdout, a truncated body
        # or a malformed page all used to `break` out of the loop and return an empty list as
        # SUCCESS — which renders as "No open alerts. ✓", the one reading this script exists to
        # prevent (a tool that could not look must never report a clean answer).
        dec = json.JSONDecoder()
        text = p.stdout.strip()
        if not text:
            return None, (f"`gh` returned nothing for the {st} alerts of {repo}. That is not an "
                          f"empty result — the request produced no body at all.")
        i, pages = 0, 0
        while i < len(text):
            try:
                val, end = dec.raw_decode(text, i)
            except ValueError:
                return None, (f"Could not parse the {st} alerts of {repo}: the response is not "
                              f"valid JSON from byte {i} on (truncated or interleaved output).")
            if not isinstance(val, list):
                return None, (f"Unexpected {st}-alert response from {repo}: a JSON "
                              f"{type(val).__name__}, not the array of alerts the API returns.")
            out.extend(val)
            pages += 1
            i = end
            while i < len(text) and text[i] in " \t\r\n":
                i += 1
        if pages == 0:
            return None, (f"No alert pages parsed for {st} on {repo} — the response held no JSON "
                          f"array, so nothing could be read.")
    return out, None


def collect(alerts, only=None):
    """One row per alert: severity, rule, where, and the message that says what is wrong."""
    rows = []
    for a in alerts:
        rule = a.get("rule") or {}
        inst = a.get("most_recent_instance") or {}
        loc = inst.get("location") or {}
        path = loc.get("path") or "?"
        if only is not None and path not in only:
            continue
        sev = rule.get("security_severity_level") or rule.get("severity") or "note"
        rows.append({
            "sev": sev.lower(),
            "rule": rule.get("id") or "?",
            "file": path,
            "line": loc.get("start_line") or 0,
            "msg": " ".join((inst.get("message") or {}).get("text", "").split()),
            "state": a.get("state") or "?",
            "ref": (inst.get("ref") or "").replace("refs/heads/", ""),
            "url": a.get("html_url") or "",
        })
    # Severity, then file/line so a re-run diffs cleanly and one file's findings stay together.
    return sorted(rows, key=lambda r: (_ORDER.get(r["sev"], 99), r["file"], r["line"]))


def alert_findings(rows):
    """The alerts in the shared findings shape: a rule id, a path and line, and the severity as the level (critical, high and error fail; the rest warn)."""
    return [{"rule": r["rule"], "message": r["msg"], "file": r["file"], "line": r["line"],
             "level": "error" if r["sev"] in ("critical", "high", "error") else "warning"} for r in rows]


def _table(rows, title, show_state=False):
    """One severity-ordered table. Widths come from the data, capped, like the other reports.

    `show_state` adds the STATE column, which only earns its width when the listing mixes states
    (an `--all` run) — on a single-state run the header already said which one.
    """
    # Never narrower than the header itself, or the dashes stop lining up under it (a `note`-only
    # table gave a 4-wide rule under an 8-wide "SEVERITY").
    sev_w = max(min(max(len(r["sev"]) for r in rows), 8), len("SEVERITY"))
    rule_w = max(min(max(len(r["rule"]) for r in rows), 34), len("RULE"))
    loc_w = max(min(max(len(f"{r['file']}:{r['line']}") for r in rows), 46), len("FILE:LINE"))
    st_w = max(min(max(len(r["state"]) for r in rows), 9), len("STATE")) if show_state else 0

    def clip(s, w):
        return s if len(s) <= w else s[: w - 1] + "…"

    st_head = f"{'STATE':<{st_w}}  " if show_state else ""
    st_rule = f"{'-' * st_w}  " if show_state else ""
    L = ["", f"{title} — {len(rows)}",
         f"  {st_head}{'SEVERITY':<{sev_w}}  {'RULE':<{rule_w}}  {'FILE:LINE':<{loc_w}}  WHAT",
         f"  {st_rule}{'-' * sev_w}  {'-' * rule_w}  {'-' * loc_w}  {'-' * 40}"]
    for r in rows[:MAX_ROWS]:
        loc = f"{r['file']}:{r['line']}"
        st = f"{clip(r['state'], st_w):<{st_w}}  " if show_state else ""
        L.append(f"  {st}{clip(r['sev'], sev_w):<{sev_w}}  {clip(r['rule'], rule_w):<{rule_w}}  "
                 f"{clip(loc, loc_w):<{loc_w}}  {clip(r['msg'], 60)}")
    if len(rows) > MAX_ROWS:
        L.append(f"  … {len(rows) - MAX_ROWS} more (severity order, so these are the least "
                 f"severe). Use --module <name> to scope.")
    return L


def render(rows, state, refs, scoped=False):
    """The counts first, then shipping code, then tests.

    Split because the two are read differently: a finding in `src/` ships to a device, one in
    `test/` does not. Measured here, 783 of 855 alerts are in test files — pooling them buries
    the three `high` findings in shipping code under a wall of doctest noise.

    The severity breakdown is printed BEFORE any table, so the numbers survive truncation: a
    reader must be able to see "3 high" even when the table showed only the first 60 rows.
    """
    L = []
    if not rows:
        L.append(f"No {state} alerts. ✓")
        if scoped:
            L.append("  (Scoped to a module — run without one to see the whole repo.)")
        L.append("  (CodeQL analyzes PUSHED commits — local edits are not included.)")
        return L

    branches = ", ".join(sorted(refs)) if refs else "?"
    L.append(f"{len(rows)} {state} alert(s) — analyzed on {branches}, not on your working tree.")

    src = [r for r in rows if r["file"].startswith("src/")]
    other = [r for r in rows if not r["file"].startswith("src/")]

    def tally(subset):
        c = {}
        for r in subset:
            c[r["sev"]] = c.get(r["sev"], 0) + 1
        return " · ".join(f"{k} {v}" for k, v in
                          sorted(c.items(), key=lambda kv: _ORDER.get(kv[0], 99))) or "none"

    L.append(f"  shipping code (src/): {tally(src)}")
    L.append(f"  tests + vendored:     {tally(other)}")

    # With more than one state in the list, say which — a `fixed` row rendered like an open one
    # reads as a live finding. Absent on a single-state run, where the header already said it.
    states = {}
    for r in rows:
        states[r["state"]] = states.get(r["state"], 0) + 1
    if len(states) > 1:
        L.append("  by state:             "
                 + " · ".join(f"{k} {v}" for k, v in sorted(states.items())))

    mixed = len(states) > 1
    if src:
        L += _table(src, "src/ — ships to a device", mixed)
    if other:
        L += _table(other, "test/ + vendored — does not ship", mixed)
    return L


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--state", default="open", choices=("open", "fixed", "dismissed"),
                    help="Alert state to list (default: open).")
    ap.add_argument("--all", action="store_true", help="Every state, not just one.")
    ap.add_argument("--module", help="Only alerts in this module's source files.")
    args = ap.parse_args()

    state = "all" if args.all else args.state
    alerts, err = fetch(state)
    if alerts is None:
        # FAIL LOUD. A tool that could not look must not print a comfortable zero.
        print(err, file=sys.stderr)
        return 2

    only = None
    if args.module:
        only = check_clang_query.module_files(args.module)
        if not only:
            print(f"No source files for module '{args.module}'.", file=sys.stderr)
            return 2
        print(f"Filtered to {args.module}: {', '.join(only)}\n")

    rows = collect(alerts, only)
    # Only the open alerts of the whole repo are the record: a module, a state or --all shows another view.
    if not (only or args.all or state != "open"):
        _findings.write("check_codeql", {r: {"reason": "CodeQL query, run in CI (.github/workflows/codeql.yml)"} for r in sorted({x["rule"] for x in rows})}, alert_findings(rows))
    # The branch comes from the alerts themselves; when none matched a scope there is nothing to
    # read it from, so fall back to every alert's ref rather than printing "?".
    refs = {r["ref"] for r in (rows or collect(alerts)) if r["ref"]}
    print("\n".join(render(rows, state, refs, scoped=only is not None)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
