#!/usr/bin/env python3
"""Compare two runs of bench_tempo.py, track by track: better, worse, or chance.

Pairs the tracks both runs scored by a hash of their audio, gives each run's share right, the exact McNemar test on the tracks right in one run only, and every track whose reading changed. The comparison prints and goes to MoonDeck's View pane, where the **Tempo Report** card shows it.

Usage:
    uv run moondeck/check/tempo_compare.py --last                          # the two newest runs on the same files
    uv run moondeck/check/tempo_compare.py <run.jsonl> <run.jsonl> [--key acc2_4]

@attribution McNemar's test
"""

import argparse
import json
import math
import sys
from pathlib import Path

from bench_tempo import OUT_DIR, pct, show

KEYS = ("acc1_2", "acc1_4", "acc2_2", "acc2_4")


def read_run(path: Path) -> list[dict]:
    """A run file's rows, without a line cut short when the run stopped."""
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    return rows


def mcnemar(b: int, c: int) -> float:
    """Exact two-sided McNemar: a binomial test on the tracks the two runs disagree on."""
    n = b + c
    if n == 0:
        return 1.0
    p = sum(math.comb(n, i) for i in range(min(b, c) + 1)) / 2 ** n
    return min(1.0, 2 * p)


def last_pair(out: Path) -> tuple[Path, Path]:
    """The two newest run files holding the same set of files, older first, the files known by their audio's hash."""
    runs = sorted(out.glob("run-*.jsonl"), reverse=True)
    files = {r: frozenset(row["hash"] or row["path"] for row in read_run(r)) for r in runs}
    for i, newer in enumerate(runs):
        for older in runs[i + 1:]:
            if files[older] == files[newer] and files[newer]:
                return older, newer
    sys.exit("--last: no two runs on the same files yet")


def compare(a_path: Path, b_path: Path, key: str, private: bool, out: Path) -> str:
    """Pair two runs on the tracks both scored: each run's share right, the exact McNemar test, and every track that changed."""
    # Tracks pair by a hash of their audio; a file that failed to decode has none and pairs with nothing.
    def load(path: Path) -> dict[str, dict]:
        return {r["hash"]: r for r in read_run(path) if r.get("hash") and key in r.get("judge", {})}
    a, b = load(a_path), load(b_path)
    common = sorted(set(a) & set(b), key=lambda h: (a[h]["genre"], a[h]["name"]))
    only_a = [h for h in common if a[h]["judge"][key] and not b[h]["judge"][key]]
    only_b = [h for h in common if b[h]["judge"][key] and not a[h]["judge"][key]]
    lines = [f"# Tempo runs compared on {key}", "",
             f"A is `{a_path.name}`, B is `{b_path.name}`: {len(common)} tracks scored in both.", "",
             "| | A | B |", "|---|---:|---:|",
             f"| Right ({key}) | {pct([a[h] for h in common], key)} | {pct([b[h] for h in common], key)} |",
             f"| Right in this run only | {len(only_a)} | {len(only_b)} |", "",
             f"Exact McNemar p = {mcnemar(len(only_a), len(only_b)):.4g} on {len(only_a)} and {len(only_b)} tracks right in one run only; "
             "a p below 0.05 means a split this lopsided is unlikely under chance alone.", ""]
    changed = [h for h in common if a[h]["judge"]["verdict"] != b[h]["judge"]["verdict"] or a[h]["est"] != b[h]["est"]]
    if changed:
        lines += ["## Tracks whose reading changed", "", "| Track | Genre | Label | A | B |", "|---|---|---:|---|---|"]
        for h in changed:
            track = h if private else a[h]["name"].replace("|", "/")
            lines.append(f"| {track} | {a[h]['genre']} | {a[h]['ref']:g} | {reading(a[h])} | {reading(b[h])} |")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    show(text, out)
    return text


def reading(row: dict) -> str:
    return f"{row['est']:g} {row['judge']['verdict']}" if row["est"] is not None else "no tempo"


def main() -> None:
    sys.stdout.reconfigure(encoding="utf-8")   # track names print as they are, whatever the console's code page
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", type=Path, nargs="*", metavar="RUN", help="two run files, older first")
    ap.add_argument("--last", action="store_true", help="the two newest runs on the same files")
    ap.add_argument("--key", choices=KEYS, default="acc1_2", help="the score the comparison pairs (default acc1_2)")
    ap.add_argument("--private", action="store_true", help="name tracks by a hash of their audio")
    ap.add_argument("--out", type=Path, default=OUT_DIR, help="where the runs are (default build/tempo)")
    args = ap.parse_args()
    if args.last == bool(args.runs) or (args.runs and len(args.runs) != 2):
        ap.error("give two run files, or --last")
    a_run, b_run = args.runs or last_pair(args.out)
    compare(a_run, b_run, args.key, args.private, args.out)


if __name__ == "__main__":
    main()
