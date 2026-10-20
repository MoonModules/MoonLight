"""The scenario performance table MoonDeck's step view shows: a step's contract and observed numbers as one Markdown table."""

def _fmt_us(us: int) -> str:
    """Pretty-print microseconds with a friendly unit (us / ms / s)."""
    if us >= 1_000_000:
        return f"{us / 1_000_000:.2f}s"
    if us >= 1_000:
        return f"{us / 1_000:.1f}ms"
    return f"{us}µs"


# What a cell shows when there is nothing to show. An em-dash is the typographic
# convention for "no value" in a table, and the coding standards exempt a literal one in
# a UI string from the no-em-dashes prose rule. Named once so the six cell renderers agree
# and so the character appears in exactly one place.
NO_VALUE = "\u2014"


def _fps_from_us(us: int) -> str:
    """Convert tick_us → frames-per-second string (no decimals for ≥100 FPS,
    one decimal below; for headline display). Shared core of both
    `_fps_floor_from_contract` (single scalar → '≥ N FPS') and
    `_fps_range_from_observed_range` ([min, max] tick → 'lo-hi FPS')."""
    if us <= 0:
        return NO_VALUE
    fps = 1_000_000 / us
    if fps >= 100:
        return f"{int(round(fps)):,}"
    return f"{fps:.1f}"


def _fmt_heap(bytes_: int) -> str:
    """Pretty-print free-heap bytes (KB once over 1024). 0 = unlimited (desktop)."""
    if bytes_ == 0:
        return "unlimited"
    if bytes_ >= 1024:
        return f"{bytes_ / 1024:.0f}KB"
    return f"{bytes_}B"


def _fps_floor_from_contract(tick_us) -> str:
    """Contract tick ceiling → FPS floor. We store ticks (the assertion unit)
    but render FPS as the headline number (project convention; see README §
    Performance)."""
    if tick_us in (None, 0):
        return NO_VALUE
    return f"≥ {_fps_from_us(int(tick_us))}"


def _fps_range_from_observed_range(v) -> str:
    """Observed tick → FPS range, inverted (slow tick = low FPS). Collapses when the
    formatted endpoints would render the same. Returns the missing-value dash when there is
    nothing to show.

    Reads the current observation shape (a dict of statistics over a rolling sample
    window, moondeck/scenario/_observed.py) and renders p50 to p95: the typical cost and
    the tail. NOT min to max, which spans one lucky run to the worst excursion ever
    recorded and reads as a much wider performance envelope than the code actually has.
    The older [min, max] list is still accepted so a file written before the change
    renders rather than raising."""
    if v is None:
        return NO_VALUE
    pair = _observed_pair(v)
    if pair is None:
        return NO_VALUE
    lo_us, hi_us = pair
    # BOTH endpoints must convert: _fps_from_us returns the missing-value dash for a
    # non-positive tick, and pairing that with a real number renders a range with a dash on one
    # end, which reads as a measurement rather than as absent data.
    if lo_us <= 0 or hi_us <= 0:
        return NO_VALUE
    # Higher FPS comes from the LOWER tick, so the pair inverts.
    hi_fps, lo_fps = _fps_from_us(lo_us), _fps_from_us(hi_us)
    return lo_fps if lo_fps == hi_fps else f"{lo_fps}-{hi_fps}"


def _heap_contract_cell(v) -> str:
    """Contract heap/block floor → '≥ N KB'. None → '—'. 0 → 'unlimited'
    (the desktop platform reports free_heap=0 / max_alloc_block=0 to mean
    "no meaningful ceiling"; rendering them as missing was misleading)."""
    if v is None:
        return NO_VALUE
    if v == 0:
        return "unlimited"
    return f"≥ {_fmt_heap(int(v))}"


def _observed_pair(v):
    """Any observation shape → the (low, high) pair the cell renderers display, or None.

    ONE place knows the stored shape. Observations are a dict of statistics over a
    rolling sample window (moondeck/scenario/_observed.py); a file written before that
    change holds a [min, max] list, and an older one a bare scalar. Each renderer used to
    re-derive this, so the shape change broke them one at a time.

    For the dict, the pair is p50 to p95: the typical value and its tail. Min to max
    would span one lucky run to the worst excursion ever recorded, which reads as a far
    wider envelope than the code actually has. n=0 (never measured on this target, e.g.
    another OS) returns None, so the cell shows a dash rather than a fabricated zero.
    """
    if v is None:
        return None
    if isinstance(v, dict):
        if not v.get("n"):
            return None
        return int(v.get("p50", 0)), int(v.get("p95", 0))
    if isinstance(v, list) and len(v) == 2:
        return int(v[0]), int(v[1])
    if isinstance(v, (int, float)):
        return int(v), int(v)
    return None


def _heap_observed_cell(v) -> str:
    """Observed heap/block as 'N KB' or 'N-M KB'; the missing-value dash when absent; and
    'unlimited' for 0, matching the contract cell (desktop reports 0 to mean "no
    meaningful value", which should display as unlimited rather than be dropped)."""
    pair = _observed_pair(v)
    if pair is None:
        return NO_VALUE
    lo, hi = pair
    if lo == 0 and hi == 0:
        return "unlimited"
    return _fmt_heap(lo) if lo == hi else _fmt_heap_range([lo, hi])


def _format_perf_table(step: dict) -> list[str]:
    """Build a markdown table for a step's contract + observed data, one row
    per board. Returns [] when neither contract nor observed has anything
    measurable; otherwise returns the table lines plus any per-board audit
    footer (set_by / reason / observed-at) below."""
    contract = step.get("contract") or {}
    observed = step.get("observed") or {}
    boards = sorted(set(contract.keys()) | set(observed.keys()))
    if not boards:
        return []

    lines: list[str] = []
    lines.append("**Performance** (contract / observed), tick stored, FPS shown:")
    lines.append("")
    lines.append("| Board | FPS | heap | block |")
    lines.append("|---|---|---|---|")
    for b in boards:
        c = contract.get(b) or {}
        o = observed.get(b) or {}
        fps = f"{_fps_floor_from_contract(c.get('tick_us'))} / {_fps_range_from_observed_range(o.get('tick_us'))}"
        heap = f"{_heap_contract_cell(c.get('free_heap'))} / {_heap_observed_cell(o.get('free_heap'))}"
        block = f"{_heap_contract_cell(c.get('max_alloc_block'))} / {_heap_observed_cell(o.get('max_alloc_block'))}"
        lines.append(f"| `{b}` | {fps} | {heap} | {block} |")
    lines.append("")

    # Audit footer: contract origin + observation timestamps, only when present.
    audit: list[str] = []
    for b in boards:
        c = contract.get(b) or {}
        o = observed.get(b) or {}
        bits: list[str] = []
        if c.get("set_by") or c.get("reason"):
            sb = c.get("set_by") or "?"
            rs = f' "{c["reason"]}"' if c.get("reason") else ""
            bits.append(f"contract set {sb}{rs}")
        at = o.get("last_updated", o.get("at"))
        if at:
            bits.append(f"observed {_fmt_at_range(at)}")
        if bits:
            audit.append(f"- `{b}`: {' · '.join(bits)}")
    if audit:
        lines.extend(audit)
        lines.append("")
    return lines


def _fmt_us_range(v) -> str:
    """Pretty-print a [min, max] tick range. Collapses when the *formatted*
    endpoints would render identically (e.g. 84,500µs and 84,520µs both round
    to "85µs" at our resolution, showing them as a range adds noise)."""
    if isinstance(v, list) and len(v) == 2:
        lo, hi = int(v[0]), int(v[1])
        lo_s, hi_s = _fmt_us(lo), _fmt_us(hi)
        lo_fps, hi_fps = _fps_from_us(lo), _fps_from_us(hi)
        if lo_s == hi_s and lo_fps == hi_fps:
            return f"{lo_s} ({lo_fps} FPS)"
        return f"{lo_s}-{hi_s} ({hi_fps}-{lo_fps} FPS)"
    return f"{_fmt_us(int(v))} ({_fps_from_us(int(v))} FPS)"


def _fmt_heap_range(v) -> str:
    """Pretty-print a [min, max] heap/block range. Collapses when the formatted
    endpoints would render identically (KB rounding hides sub-KB drift)."""
    if isinstance(v, list) and len(v) == 2:
        lo, hi = int(v[0]), int(v[1])
        lo_s, hi_s = _fmt_heap(lo), _fmt_heap(hi)
        if lo_s == hi_s:
            return lo_s
        return f"{lo_s}-{hi_s}"
    return _fmt_heap(int(v))


def _fmt_at_range(at) -> str:
    """The date this observation last took a sample (`last_updated`). The older `at` key
    and its two-element [first_seen, last_updated] form are still accepted so a file
    written before the change renders; only the last element is shown, the first having
    described samples that had long aged out of the window."""
    if isinstance(at, list) and len(at) == 2:
        return str(at[1])
    return str(at)
