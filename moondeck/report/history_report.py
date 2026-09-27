#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# ///
"""Generate a human-readable history report from git log + gh release list.

The report is an **artifact**, not source — by default it writes to
``build/history.md`` (which is gitignored, same shape as the desktop
binaries that live under ``build/<host>/``). Every developer regenerates
it locally from the MoonDeck button; storing it in the repo would
duplicate what git log already carries.

Output shape:
  - Releases section: most-recent 10 tagged releases, newest first.
  - History section: combined graph + commits. Each commit row shows
    its graph-rail (`*`, `| *`, `*   `, …) as a monospace prefix to
    the SHA + date + subject; merge commits get a ⤴ badge. The full
    body lives in a left-bordered blockquote underneath, visually
    extending the rail's vertical line into the description. Branch
    connector rows (`|\`, `|/`, `| |`) render as standalone monospace
    lines between commits.

Re-runs are deterministic on identical git state except for the
"Generated at …" line in the footer (the only non-determinism).
"""

import argparse
import datetime
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_OUT = ROOT / "build" / "history.md"

# Format string for the git-log dump. NUL-delimited records so commit
# bodies with newlines survive parsing; %x00 between records, %x1f between
# fields within a record. Both bytes are forbidden in commit messages.
GIT_LOG_FORMAT = "%H%x1f%h%x1f%ad%x1f%s%x1f%P%x1f%b%x00"
GIT_DATE_FORMAT = "short"   # → "YYYY-MM-DD"

# Co-Authored-By: and similar trailers add noise to the report; strip
# from blockquoted body paragraphs.
TRAILER_PREFIXES = (
    "Co-Authored-By:",
    "Signed-off-by:",
    "Reviewed-by:",
    "Acked-by:",
)


def run(cmd: list[str]) -> str:
    """Run a command and return stdout text. Raises on non-zero exit."""
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
    return r.stdout


def fetch_commits() -> list[dict]:
    """Parse the entire git log into structured records."""
    raw = run([
        "git", "log",
        f"--format={GIT_LOG_FORMAT}",
        f"--date={GIT_DATE_FORMAT}",
    ])
    out: list[dict] = []
    # Records are NUL-terminated; trailing empty record from the final NUL
    # is dropped.
    for rec in raw.split("\x00"):
        rec = rec.strip("\n")
        if not rec:
            continue
        parts = rec.split("\x1f")
        if len(parts) < 6:
            # Malformed — skip rather than crash. Real logs in practice
            # always produce 6 fields; defensive for the empty-body case.
            continue
        sha, short, date, subject, parents, body = parts
        out.append({
            "sha": sha,
            "short": short,
            "date": date,
            "subject": subject,
            "is_merge": len(parents.split()) > 1,
            "body": body,
        })
    return out


def fetch_graph_with_rails() -> list[dict]:
    """Per-line `git log --graph` parse with the graph rails extracted
    from each row. Used to drive the combined commits-with-graph layout:
    every line is either a commit row (has a SHA in `%h` position) or
    a connector row (just graph characters).

    Returned shape, one dict per source line:
      {"rail": "* | |", "sha": "abc1234", "subject": "..."}  -- commit
      {"rail": "| |/",   "sha": None,      "subject": None}  -- connector
    """
    raw = run([
        "git", "log",
        "--graph",
        "--all",
        # NUL after %h so we can split: rail + spacer + sha + subject is
        # the commit-row shape; rail alone is the connector-row shape.
        "--pretty=format:%h\x1f%s",
    ])
    rows: list[dict] = []
    for line in raw.split("\n"):
        if "\x1f" in line:
            # Commit row. Find the SHA — first run of hex chars after
            # the graph prefix. Walk forward to the first non-graph char
            # (anything not in " *|/\\") to find the rail boundary.
            i = 0
            while i < len(line) and line[i] in " *|/\\":
                i += 1
            rail = line[:i].rstrip()
            rest = line[i:]
            sha_part, subject = rest.split("\x1f", 1)
            rows.append({"rail": rail, "sha": sha_part.strip(), "subject": subject})
        else:
            # Connector row — `| |`, `|/`, `|\`, etc. No commit on this line.
            rows.append({"rail": line.rstrip(), "sha": None, "subject": None})
    return rows


def fetch_releases() -> list[dict]:
    """Pull recent releases via gh. Returns [] when gh is unavailable
    or the repo has no releases."""
    try:
        raw = run([
            "gh", "release", "list",
            "--limit", "10",
            # `gh release list --json` doesn't expose the release body —
            # only `gh release view <tag>` does. Fetching N bodies would
            # need N API calls; skip for v1 and show just tag/date/name.
            "--json", "tagName,publishedAt,isPrerelease,name",
        ])
    except (subprocess.CalledProcessError, FileNotFoundError):
        # gh not installed, not authenticated, or the API call failed.
        # Report still renders; just no releases section.
        return []
    import json
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return []


def classify_tag(tag: str) -> str:
    """Map tag-name shape → channel label."""
    if tag == "latest":
        return "latest"
    if "-rc" in tag:
        return "rc"
    return "stable"


def body_lines(body: str) -> list[str]:
    """Return the full commit body as a list of lines, with trailers
    (Co-Authored-By, Signed-off-by, …) stripped and surrounding blank
    lines trimmed. Returned lines preserve the original paragraph
    structure so the markdown blockquote in the report reads the same
    way `git show` does — full message, not a one-paragraph summary."""
    if not body:
        return []
    lines: list[str] = []
    for line in body.split("\n"):
        s = line.rstrip()
        # Drop trailer lines; they're metadata that doesn't add value
        # to a chronological reading.
        if any(s.startswith(p) for p in TRAILER_PREFIXES):
            continue
        lines.append(s)
    # Trim leading/trailing blank lines so blockquote rendering doesn't
    # gain spurious empty `>` markers at the edges.
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    return lines


def render_combined(rails: list[dict], commits: list[dict], repo_url: str | None) -> str:
    """One section that interleaves graph rails with per-commit detail.

    For each row from `git log --graph --all`:
      - Connector rows render as monospace rails on a single line.
      - Commit rows render with the rail in monospace AS A PREFIX to the
        subject + SHA link, and the full commit body as a blockquote
        beneath. Each commit's <div> is the structural unit; the rail's
        vertical lines and the body's left-border-blockquote line up
        visually (matching CSS does the alignment).

    Emits raw HTML (the moondeck renderer passes `<...>`-only lines
    through unescaped). Falls back to the separate-graph rendering if
    the rail parse came back empty.
    """
    if not rails:
        return "## History\n\n_No commits found._\n\n"

    # SHA → full commit record lookup for body rendering.
    by_short = {c["short"]: c for c in commits}

    out = ["## History", ""]
    for row in rails:
        rail = row["rail"] or ""
        if row["sha"] is None:
            # Connector row — just rails.
            out.append('<div class="hr-line"><span class="hr-rail">'
                       + html_escape_minimal(rail) + "</span></div>")
            continue
        # Commit row.
        c = by_short.get(row["sha"])
        if c is None:
            # SHA from the graph wasn't in the structured log (rare —
            # different ref ranges in --all vs the default log). Still
            # render the rail + subject as a minimal entry.
            out.append('<div class="hr-line"><span class="hr-rail">'
                       + html_escape_minimal(rail) + "</span> "
                       + '<code>' + html_escape_minimal(row["sha"]) + "</code> "
                       + html_escape_minimal(row["subject"] or "") + "</div>")
            continue
        # Full commit entry.
        sha_html = f'<a href="{repo_url}/commit/{c["sha"]}" target="_blank" rel="noopener"><code>{c["short"]}</code></a>' if repo_url else f'<code>{c["short"]}</code>'
        merge_badge = '<span class="hr-merge">⤴</span> ' if c["is_merge"] else ""
        out.append('<div class="hr-commit">')
        out.append(
            '  <div class="hr-head">'
            + '<span class="hr-rail">' + html_escape_minimal(rail) + "</span> "
            + merge_badge
            + sha_html + " "
            + '<span class="hr-date">' + html_escape_minimal(c["date"]) + "</span> "
            + '<strong>' + html_escape_minimal(c["subject"]) + "</strong>"
            + "</div>"
        )
        # Body as a regular markdown blockquote — the renderer turns `-`
        # lines into a nested <ul> and `<br>`s the rest.
        body = body_lines(c["body"])
        if body:
            for line in body:
                # Indent the markdown blockquote two spaces so it nests
                # visually under the <div>. The renderer accepts
                # leading-whitespace blockquotes.
                out.append(f"  > {line}" if line else "  >")
        out.append("</div>")
    return "\n".join(out) + "\n"


def html_escape_minimal(s: str) -> str:
    """Minimal HTML escape for the raw-HTML rows in render_combined."""
    return (s or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def render_releases(releases: list[dict]) -> str:
    """Markdown table of the most-recent releases."""
    if not releases:
        return (
            "## Releases\n\n"
            "_No releases found (gh not authenticated, or no releases yet)._\n\n"
        )
    rows = [
        "## Releases",
        "",
        "| Tag | Date | Type | Name |",
        "|-----|------|------|------|",
    ]
    for r in releases:
        tag = r.get("tagName", "")
        date = (r.get("publishedAt") or "")[:10]   # YYYY-MM-DD
        kind = classify_tag(tag)
        name = (r.get("name") or "").replace("|", "\\|")
        rows.append(f"| `{tag}` | {date} | {kind} | {name} |")
    rows.append("")
    return "\n".join(rows) + "\n"


def resolve_repo_url() -> str | None:
    """Return the GitHub web URL for the origin remote, or None."""
    try:
        raw = run(["git", "config", "--get", "remote.origin.url"]).strip()
    except subprocess.CalledProcessError:
        return None
    # Two shapes — https://github.com/user/repo[.git] and git@github.com:user/repo.git.
    if raw.startswith("git@"):
        host, path = raw.split(":", 1)
        url = f"https://{host[4:]}/{path}"
    else:
        url = raw
    if url.endswith(".git"):
        url = url[:-4]
    return url


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "--out",
        type=Path,
        default=DEFAULT_OUT,
        help=f"Output path (default: {DEFAULT_OUT.relative_to(ROOT)})",
    )
    args = ap.parse_args()

    commits = fetch_commits()
    releases = fetch_releases()
    rails = fetch_graph_with_rails()
    repo_url = resolve_repo_url()

    parts = [
        "# MoonLight history report",
        "",
        "Auto-generated by `moondeck/report/history_report.py`. Re-run from "
        "MoonDeck's Desktop tab → **History Report** button. Source data: "
        "`git log` + `gh release list`.",
        "",
        "The report is an artifact, not source — it writes to `build/history.md` "
        "by default (gitignored). Storing it in the repo would duplicate what "
        "git log already carries.",
        "",
        render_releases(releases),
        render_combined(rails, commits, repo_url),
        "## Summary",
        "",
        f"- {len(commits)} commits in this report.",
        f"- {len(releases)} releases.",
        f"- Repo: {repo_url or '_(no origin remote configured)_'}",
        f"- Generated at {datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')}",
        "",
    ]
    text = "\n".join(parts)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")

    # Print the resolved path + a MOONDECK_VIEW marker line. MoonDeck's
    # log renderer recognizes the marker and routes the URL into the View
    # pane (same as the ? help button) instead of opening a new tab.
    # Iframes can't load file:// from the http:// MoonDeck origin (browser
    # mixed-protocol restriction), which is why we point at MoonDeck's
    # /api/history-report endpoint — same trick /api/help uses.
    rel = args.out.relative_to(ROOT) if args.out.is_relative_to(ROOT) else args.out
    print(f"Wrote {rel} ({len(text):,} bytes, {len(commits)} commits).")
    print("MOONDECK_VIEW: /api/history-report")
    return 0


if __name__ == "__main__":
    sys.exit(main())
