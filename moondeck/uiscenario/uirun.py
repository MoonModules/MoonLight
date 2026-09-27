"""Drive the MoonLight UI from a run file: one engine, for tests and for video.

Every step goes through the INTERFACE. A step that POSTs its way to the outcome
proves nothing about the UI and, on camera, shows an effect with no visible cause:
a value moving with the pointer parked beside it. The + tab, the type picker, the
card's own buttons and the control inputs are the only ways in.

REST is READ-ONLY here, and it is how a step checks itself: do the thing by hand,
then ask the device what it now holds. That is what lets one run file serve twice.
As a test the check is the assertion; as a video the same step carries a caption and
a duration, and the check is what keeps a silently broken take out of the cut.

Locators follow Playwright's own hierarchy. The UI carries no `data-testid`, but
`data-module`, `data-mid` and `data-tab-mid` are a real contract rather than styling
hooks: app.js queries ITSELF through them (updateModuleControls), so they cannot
drift without breaking the app. set_test_id_attribute points get_by_test_id at them,
which puts every lookup in the sanctioned test-id tier instead of raw CSS. Roles are
not usable here: the buttons carry `title`, which contributes no accessible name.

The mechanism lives here; the content lives in a run file. See moondeck/uiscenario/RUNS.md.
"""

from __future__ import annotations

import json
import re
import shlex
import subprocess
from urllib.parse import urljoin
import time
from dataclasses import dataclass, field
from pathlib import Path

import requests

# The repo root, so a command a run file starts resolves its paths the way a person would.
ROOT = Path(__file__).resolve().parents[2]

# The attribute app.js names a module's card AND its nav button by, offered to
# get_by_test_id as the test-id contract. ONE attribute, not a list: the
# comma-separated form documented for set_test_id_attribute builds a malformed
# selector in 1.62 ([data-module,data-mid="X"]), so it is not usable here.
#
# One is enough. Modules are what a step looks up by name, and cards and nav buttons
# both carry this. Controls are addressed by the [data-mid][data-key] PAIR (a key
# alone collides: every module has an `on`), and tabs by data-tab-mid, so neither is
# a test-id lookup in the first place.
TEST_ID_ATTRS = "data-module"


# ---------------------------------------------------------------------------
# The run file
# ---------------------------------------------------------------------------

@dataclass
class Step:
    action: str
    args: dict = field(default_factory=dict)
    caption: str | None = None       # video: burned in over the step
    hold: float = 0.0                # video: extra dwell after the action
    expect: dict | None = None       # test: read back over REST
    bind: str | None = None          # name the created module for later steps


@dataclass
class Run:
    name: str
    description: str = ""
    steps: list[Step] = field(default_factory=list)
    # How the published clip is encoded. Per-run because pace is editorial: a dense
    # sequence reads fine at 2x while a clip whose POINT is live responsiveness has to
    # run at 1x. Tunable after the fact without re-recording, since publishing is a
    # separate pass over the raw take.
    speed: float = 2.0
    width: int = 960
    # Which app this run drives, and what it needs from the device it drives.
    #
    # `host` is for another MoonLight SURFACE on a known port (the web installer's
    # preview), not for a device: an IP written into a tracked run file is a second
    # bench registry that goes stale the moment a board changes network, and
    # moondeck.json is the one that exists. A run needing particular hardware says so
    # with `requires` instead, and the caller resolves it.
    host: str | None = None
    requires: str | None = None      # a capability the device must have, e.g. "audio"


def load_run(path: Path) -> Run:
    raw = json.loads(Path(path).read_text())
    steps = []
    for s in raw.get("steps", []):
        s = dict(s)
        steps.append(Step(
            action=s.pop("action"),
            caption=s.pop("caption", None),
            hold=float(s.pop("hold", 0.0)),
            expect=s.pop("expect", None),
            bind=s.pop("as", None),
            args=s,                 # whatever the action itself takes
        ))
    return Run(
        name=raw.get("name", Path(path).stem),
        description=raw.get("description", ""),
        steps=steps,
        speed=float(raw.get("speed", 2.0)),
        width=int(raw.get("width", 960)),
        host=raw.get("host"),
        requires=raw.get("requires"),
    )


# ---------------------------------------------------------------------------
# Reading the device: the only REST this module does
# ---------------------------------------------------------------------------

def state(host: str) -> dict:
    return requests.get(f"http://{host}/api/state", timeout=5).json()


def find_module(host: str, name: str) -> dict | None:
    def walk(ms):
        for m in ms:
            if m.get("name") == name:
                return m
            got = walk(m.get("children", []))
            if got:
                return got
        return None
    return walk(state(host).get("modules", []))


def find_by_type(host: str, type_name: str) -> str | None:
    def walk(ms):
        for m in ms:
            if m.get("type") == type_name:
                return m.get("name")
            got = walk(m.get("children", []))
            if got:
                return got
        return None
    return walk(state(host).get("modules", []))


def control_value(host: str, module: str, control: str):
    mod = find_module(host, module)
    if not mod:
        return None
    for c in mod.get("controls", []):
        if c.get("name") == control:
            return c.get("value")
    return None


_DISPLAY_NAMES: dict[str, dict[str, str]] = {}


def display_name_for(host: str, type_name: str) -> str:
    """What the PICKER calls a type: rows read "Ripples", not "RipplesEffect".

    A run file names the type, so it survives a display-name change; the text typed
    into the search box is looked up here.
    """
    # CACHED per host. /api/types builds a throwaway instance of every registered type
    # to read its controls, so it is a heavy GET to repeat once per add on a board.
    if host not in _DISPLAY_NAMES:
        try:
            payload = requests.get(f"http://{host}/api/types", timeout=5).json()
        except requests.RequestException:
            return type_name
        _DISPLAY_NAMES[host] = {t.get("name"): (t.get("displayName") or t.get("name"))
                                for t in payload.get("types", [])}
    return _DISPLAY_NAMES[host].get(type_name, type_name)


def all_names(modules: list) -> set:
    out = set()
    for m in modules:
        out.add(m.get("name", ""))
        out |= all_names(m.get("children", []))
    return out


# ---------------------------------------------------------------------------
# The driver: one interaction per method, all through the UI
# ---------------------------------------------------------------------------

def device_for(requirement: str) -> str | None:
    """A bench device that satisfies a requirement, from moondeck.json.

    The registry is the one place a board's address lives, so a run says what it needs
    ("audio": a microphone) and the address is looked up. Addresses drift between
    networks; an identity does not.
    """
    registry = Path(__file__).resolve().parents[2] / "moondeck" / "moondeck.json"
    try:
        data = json.loads(registry.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    # Which device models carry what. Kept here rather than in the registry because it
    # describes the RUNS' needs, and the registry describes the hardware.
    models = {"audio": ("MM testbench S3",)}.get(requirement, ())
    wanted_macs = set()
    for network in data.get("networks", []):
        for dev in network.get("devices", []):
            if dev.get("deviceModel") not in models:
                continue
            mac = (dev.get("mac") or "").upper()
            if mac:
                wanted_macs.add(mac)
            ip = (dev.get("ip") or "").split(":")[0]
            if ip and _answers(ip):
                return ip

    # The recorded address is STALE whenever the board moved network, which is the
    # normal case on a bench that follows a laptop between a router and a hotspot. The
    # MAC does not move, so the local subnet is swept for it: slower than a lookup, and
    # still the only way to find a board whose address nobody wrote down.
    return _find_by_mac(wanted_macs) if wanted_macs else None


def _find_by_mac(macs: set) -> str | None:
    """Sweep the local subnet for a MoonLight device with one of these MACs."""
    import concurrent.futures as cf
    import socket

    # The address of the interface that reaches the network, NOT gethostbyname of the
    # hostname: on macOS that resolves to 127.0.0.1 and the sweep then scans loopback.
    # Connecting a UDP socket sends nothing but makes the routing table pick the
    # interface, whose local address is the one the bench is on.
    try:
        probe_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe_sock.connect(("192.0.2.1", 53))     # TEST-NET-1: routable, never routed
        local = probe_sock.getsockname()[0]
        probe_sock.close()
    except OSError:
        return None
    if local.startswith("127."):
        return None
    prefix = local.rsplit(".", 1)[0]

    def probe(i):
        ip = f"{prefix}.{i}"
        try:
            r = requests.get(f"http://{ip}/api/state", timeout=1.5)
            if not r.ok:
                return None
            for mod in r.json().get("modules", []):
                for c in mod.get("controls", []):
                    if c.get("name") == "mac" and str(c.get("value", "")).upper() in macs:
                        return ip
        except (requests.RequestException, ValueError):
            return None
        return None

    with cf.ThreadPoolExecutor(max_workers=48) as pool:
        for got in pool.map(probe, range(1, 255)):
            if got:
                return got
    return None


def _answers(host: str) -> bool:
    try:
        return requests.get(f"http://{host}/api/state", timeout=2).ok
    except requests.RequestException:
        return False


class _NullOverlay:
    """Stands in for an overlay when nothing is recording, so the caller's `with` works."""
    def __enter__(self): return self
    def __exit__(self, *exc): return False


class Driver:
    """Performs steps against a page.

    `screencast` is Playwright's own recorder when this run is being filmed, and None
    when it is being tested. The cursor, the click highlights and the captions are all
    its job: show_actions animates a pointer from one action to the next and names the
    action on screen, which is precisely what a viewer needs and what a test does not.
    """

    def __init__(self, page, host: str, screencast=None):
        self.page = page
        self.host = host
        self.screencast = screencast
        # PACED for the camera, brisk for the test. Every dwell below exists so a
        # viewer can follow a pointer; a test watching nobody pays ~40 of them per run
        # for no benefit. The poll-based waits are NOT gated by this: those are
        # correctness (the device answering), not pace.
        self.paced = screencast is not None
        self.bindings: dict[str, str] = {}
        self.failures: list[str] = []
        # Every text a watched element has shown, so a wait arriving after a brief state
        # is still satisfied by it: @xref{wait_for}.
        self._seen_text: dict[str, set[str]] = {}
        # The page may own the clock (a test's does), so a timing rule is testable without a
        # real sleep and without patching `time` for everything else in the process.
        self._now = getattr(page, "now", time.time)
        self._watched: set[str] = set()
        # Commands started by the run, collected by wait_process: @xref{start_process}.
        self._processes: dict[str, subprocess.Popen] = {}

    VIEWPORT = {"width": 1280, "height": 720}

    def open_app(self, host: str | None = None, cards: bool = True) -> None:
        """Load an app and wait for it to be ready to drive.

        One home, because this was written four times with three different sleeps and
        two viewports: a card below the fold then behaved differently in a test than on
        camera, which is the one thing a shared engine exists to prevent.
        """
        self.page.set_viewport_size(self.VIEWPORT)
        # NOT networkidle. The device UI holds a preview WebSocket open and streams
        # frames down it forever, so the network never goes idle and the wait burns its
        # whole timeout under load. The DOM being ready is the real signal; what the
        # page then needs is waited for explicitly below.
        self.page.goto(f"http://{host or self.host}/", wait_until="domcontentloaded")
        if cards:
            self.page.wait_for_selector(".card", state="attached", timeout=20000)
        else:
            # Another app (the installer): no cards, and its picker populates from an
            # API call, so the select filling in is the thing to wait on.
            try:
                self.page.wait_for_selector("#rp-release option",
                                            state="attached", timeout=15000)
            except Exception:
                self.page.wait_for_timeout(2500)
        self.page.wait_for_timeout(900)

    # -- names --------------------------------------------------------------

    def resolve(self, value):
        """Expand {name} against modules bound by an earlier step.

        The DEVICE names a module, not the run file: an added RipplesEffect may land
        as "Ripples-2". A step binds the name it got, later steps refer to it.
        """
        if not isinstance(value, str):
            return value
        def sub(m):
            key = m.group(1)
            if key not in self.bindings:
                raise KeyError(f"step refers to {{{key}}}, which no earlier step bound")
            return self.bindings[key]
        return re.sub(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", sub, value)

    # -- locators -----------------------------------------------------------
    #
    # get_by_test_id over the data- contract, per Playwright's hierarchy. The picker's
    # rows are the one place with real text, so they are found by text.

    def card(self, module: str):
        """A module's card.

        Scoped to `.card` because the left NAV buttons carry `data-module` too, so the
        bare test id matches a nav button and a card with the same name. The test-id
        contract still holds; the class says which of the two this means.
        """
        return self.page.get_by_test_id(module).and_(self.page.locator(".card"))

    def nav(self, root: str):
        """A top-level nav button. Only ONE root's subtree is in the DOM at a time
        (renderCards: "One root visible at a time"), so reaching a card in another
        root means clicking its nav entry first, not scrolling to it."""
        return self.page.get_by_test_id(root).and_(self.page.locator(".nav-item"))

    @staticmethod
    def _css(value: str) -> str:
        """Escape a value for use inside a CSS attribute selector.

        A module or control name reaches these selectors from the device, so a quote or
        a backslash in one would break the selector rather than fail to match. app.js
        escapes the same way (cssEscape) when it queries its own DOM.
        """
        return value.replace("\\", "\\\\").replace('"', '\\"')

    def control(self, module: str, control: str, kind: str = ""):
        """A control input inside its module's card.

        Scoped by BOTH ids: `data-mid` names the module and `data-key` the control,
        and the pair is what app.js itself uses to find a row. A key alone collides
        (every module has an `on`).
        """
        prefix = kind or ""
        return self.page.locator(
            f'{prefix}[data-mid="{self._css(module)}"][data-key="{self._css(control)}"]')

    # -- primitives ---------------------------------------------------------

    def _settle(self, seconds: float) -> None:
        """Dwell, so the eye can follow. Skipped when nothing is recording.

        A watched progress element is sampled throughout the dwell whether or not the run is
        paced, since a state that comes and goes between two steps is lost otherwise: @xref{wait_for}.
        """
        for sel in tuple(self._watched):
            self._note_text(sel)          # correctness, so it happens whether or not anyone is watching
        if seconds <= 0 or not self.paced:
            return
        if not self._watched:
            self.page.wait_for_timeout(int(seconds * 1000))
            return
        # Sliced, so a watched element is read several times across a long dwell rather than
        # once at each end, which is where a short-lived state hides.
        end = self._now() + seconds
        while self._now() < end:
            self.page.wait_for_timeout(120)
            for sel in tuple(self._watched):
                self._note_text(sel)

    def _ready(self, locator, timeout: int = 4000) -> bool:
        """Wait for ATTACHMENT, then scroll into view.

        Attached rather than visible: scrolling it on screen is this function's job,
        so demanding it already be in the viewport is circular and fails on anything
        below the fold. That exact mistake silently skipped three steps of a take.
        """
        try:
            locator.first.wait_for(state="attached", timeout=timeout)
        except Exception:
            return False
        try:
            locator.first.scroll_into_view_if_needed(timeout=2000)
        except Exception:
            pass
        self.page.wait_for_timeout(240)
        return True

    def caption(self, text: str):
        """Narration over the current screen, as an overlay in the page.

        Returns a CONTEXT MANAGER, and the caller wraps the action in it. That is not
        a stylistic choice: show_overlay(duration=...) BLOCKS for its whole duration
        (measured: 6000ms returns after 6006ms). Calling it before the action paid
        every caption twice over as dead wall-clock, and it froze the screen while the
        words were up so the action always happened after they had gone.

        Entered around the action, the words are on screen WHILE the thing they
        describe happens, and the caption costs nothing of its own. The `with` block is
        the overlay's whole lifetime, so there is no duration to pass: the words stay up
        for exactly as long as the step they narrate takes, including its hold.
        """
        if not self.screencast:
            return _NullOverlay()
        safe = (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))
        return self.screencast.show_overlay(
            '<div style="position:fixed;left:50%;bottom:44px;transform:translateX(-50%);'
            'max-width:78%;padding:14px 22px;border-radius:10px;'
            'background:rgba(12,14,20,.82);color:#f2f5fa;'
            'font:500 25px/1.35 -apple-system,Segoe UI,Roboto,sans-serif;'
            'text-align:center;backdrop-filter:blur(8px);'
            'box-shadow:0 8px 30px rgba(0,0,0,.45)">' + safe + '</div>')

    def chapter(self, title: str, description: str | None = None,
                seconds: float = 2.0) -> None:
        if self.screencast:
            self.screencast.show_chapter(title, description=description,
                                         duration=int(seconds * 1000))
            self._settle(seconds)

    # -- actions ------------------------------------------------------------

    def tap(self, locator, dwell: float = 0.25) -> bool:
        """Move the real pointer onto an element, then press it there.

        NOT locator.click(): that dispatches the event at the element without moving
        the mouse, so nothing travels across the screen and show_actions has no motion
        to animate. A viewer then sees cards appear and values change with no cursor
        anywhere near them, which is the whole thing the pointer exists to show.
        """
        if not self._ready(locator):
            return False
        box = locator.first.bounding_box()
        if not box:
            return False
        x, y = box["x"] + box["width"] / 2, box["y"] + box["height"] / 2
        self.page.mouse.move(x, y)          # the travel show_actions animates
        self._settle(dwell)
        self.page.mouse.click(x, y)
        self.page.wait_for_timeout(280)
        return True

    def open_card(self, module: str) -> bool:
        """Bring a module on screen: a nav root, or a tab within the open root.

        Two affordances, because the UI has two. A top-level module (Layouts, Effects)
        is a NAV entry and selecting it re-renders the whole card area; a child sharing
        a parent with siblings (one Layer among several) is a TAB inside that. Trying
        the nav first is what a person does: you cannot click a tab in a root that is
        not open.
        """
        nav = self.nav(module)
        if nav.count():
            if not self.tap(nav):
                return False
            # renderCards() replaces the subtree wholesale, so wait for the new card
            # rather than a fixed sleep. The wait is the STEP'S OUTCOME: a nav click that
            # misses, or a root that never renders, has to read as a failure rather than
            # as a completed step with nothing behind it.
            try:
                self.card(module).first.wait_for(state="attached", timeout=4000)
            except Exception:
                return False
            self.page.wait_for_timeout(420)
            return True

        tab = self.page.locator(f'.tab[data-tab-mid="{self._css(module)}"]')
        if not self._ready(tab):
            return False
        if not self.tap(tab):
            return False
        self.page.wait_for_timeout(420)
        return True

    def reveal(self, module: str) -> bool:
        """Make sure a module's card is in the DOM, opening its root if it is not.

        A step naming a module in a root that is not open would otherwise fail with
        "never appeared" while the module is perfectly fine: it is simply not rendered.
        """
        if self.card(module).count():
            return True
        path = self._path_to(module)
        if not path:
            return False
        # Walk the WHOLE path, not only the root. A container renders one child card at
        # a time (renderChildTabs), so a Layer that is not the active tab is absent from
        # the DOM exactly like a closed root is, and opening the root alone left the
        # step failing on an attach timeout. Each ancestor is opened in turn: the root
        # by its nav entry, every level below it by its tab.
        #
        # The root is opened WHETHER OR NOT it is the module itself: a top-level module
        # is its own root, and skipping that case left reveal returning False while the
        # module was one click away.
        for name in path:
            self.open_card(name)
        return bool(self.card(module).count())

    def _path_to(self, name: str) -> list[str]:
        """Every ancestor of a module, root first, then the module itself.

        The whole chain, because each level has its own affordance: the root is a nav
        entry, everything below it is a tab in its parent's strip, and only the active
        one is rendered.
        """
        def walk(node, trail):
            trail = trail + [node.get("name")]
            if node.get("name") == name:
                return trail
            for kid in node.get("children", []):
                got = walk(kid, trail)
                if got:
                    return got
            return []
        for root in state(self.host).get("modules", []):
            got = walk(root, [])
            if got:
                return got
        return []

    def add_module(self, parent: str, type_name: str) -> str | None:
        """Add through the + tab and the type picker, returning the name given.

        The picker is a native <dialog> rendered IN the page, so it records. A native
        <select> popup could not: the OS compositor draws it and no capture reaches it
        (playwright#38200, closed as not planned).
        """
        before = all_names(state(self.host).get("modules", []))
        card = self.card(parent)
        if not self._ready(card):
            return None

        # DIRECT children only, for both affordances. A container's card CONTAINS its
        # children's cards, so `.card-footer button` under the Effects card also matches
        # the Layer card's own add button, and `.first` was the WRONG one: the Layer's,
        # which offers effects where a layer was wanted. app.js scopes the same way for
        # the same reason (renderModuleTree: "Scope to direct children of this card").
        # The strip lives in the card's own .card-children wrapper (renderChildTabs
        # appends it there, and createCard appends that to the card), so this is the
        # full path to THIS card's + tab. No unscoped fallback: `.tab-add` anywhere
        # under the card is a child card's + button, which is the exact mix-up this
        # scoping exists to prevent.
        add = card.locator("> .card-children > .tab-strip > .tab-add")
        footer = card.locator("> .card-footer > button")

        if add.count():
            self.tap(add)
        elif footer.count():
            self.tap(footer)
        else:
            return None

        # NO PICKER when the container accepts exactly one type: openTypePicker skips it
        # and adds straight away ("One candidate = no choice to make, so don't stage a
        # picker to ask a question with one answer"). Effects is that case, since Layer
        # is the only type carrying the `layer` role.
        #
        # Which case applies is KNOWABLE rather than guessable: count the types whose
        # role this parent accepts. Treating any exception as "no picker needed" also
        # swallowed the tap missing the + entirely, and that failure then surfaced
        # seconds later as a bare "did not complete".
        expect_picker = self._candidate_count(parent) != 1
        if expect_picker:
            if not self._pick(display_name_for(self.host, type_name)):
                return None
        else:
            self.page.wait_for_timeout(600)      # the add is immediate; let it land
        return self._await_new(before, type_name)

    def _candidate_count(self, parent: str) -> int:
        """How many registered types this container accepts as a child.

        One means the UI adds without asking; anything else opens the picker. Read from
        the same /api/types the picker itself is built from, so the two cannot disagree.
        """
        mod = find_module(self.host, parent)
        if not mod:
            return 0
        roles = {r.strip() for r in (mod.get("acceptsChildRoles") or "").split(",")
                 if r.strip()}
        # ONE fetch, used for both questions. /api/types builds a throwaway instance of
        # every registered type to read its controls, so asking twice is a heavy probe
        # run twice on a board.
        try:
            payload = requests.get(f"http://{self.host}/api/types", timeout=5).json()
        except requests.RequestException:
            return 0
        types = payload.get("types", [])
        if not roles:
            for t in types:
                if t.get("name") == mod.get("type"):
                    roles = {r.strip() for r in (t.get("acceptsChildRoles") or "").split(",")
                             if r.strip()}
                    break
        if not roles:
            return 0
        return sum(1 for t in types if t.get("role") in roles)

    def replace_module(self, module: str, type_name: str) -> str | None:
        """Swap a card's type in place, via its own ✎ button and the same picker."""
        before = all_names(state(self.host).get("modules", []))
        card = self.card(module)
        if not self._ready(card):
            return None
        # By NAME, not by position: `.card-btn` matched both ✎ and ×, and `.first`
        # relied on their order in the DOM. The buttons carry aria-labels, so the role
        # tier names them the way Playwright's hierarchy asks for.
        replace_btn = card.get_by_role("button", name="Replace with another type")
        if not replace_btn.count():
            replace_btn = card.locator(".card-actions .card-btn")
        if not self.tap(replace_btn):
            return None
        if not self._pick(display_name_for(self.host, type_name)):
            return None
        # The NEW module, not the first of that type in the tree. find_by_type returns
        # whichever matches first anywhere, so with a FireEffect already present the
        # binding pointed at someone else's module and the run's own delete step then
        # removed it. Same snapshot-and-poll add_module uses, for the same reason.
        return self._await_new(before, type_name)

    def clear_children(self, module: str) -> bool:
        """Delete every child of a container, leaving it empty.

        States the INTENT ("this layer starts empty") rather than naming a module to
        remove, which is what makes it repeatable: a run that hard-codes
        `delete_module MoonLive-2` succeeds once and fails on every later run, so it
        cannot also be a test. Already-empty is success, not a no-op to report.
        """
        mod = find_module(self.host, module)
        if not mod:
            return False
        for child in list(mod.get("children", [])):
            name = child.get("name")
            if not name:
                continue
            if not self.delete_module(name):
                return False
        return True

    def delete_module(self, module: str) -> bool:
        """Press × twice: the UI arms on the first click and commits on the second."""
        card = self.card(module)
        if not self._ready(card):
            return False
        # DIRECT child only: a container's card contains its children's cards, and the
        # file editor reuses .card-btn-del (app.js), so a descendant search can arm the
        # wrong × entirely. add_module already scopes this way.
        btn = card.locator("> .card-title .card-actions .card-btn-del")
        if not self.tap(btn):
            return False
        self._settle(0.5)                  # the armed ✓ is worth seeing
        if not self.tap(btn, dwell=0.15):
            return False
        for _ in range(20):
            if not find_module(self.host, module):
                return True
            time.sleep(0.25)
        return False

    def _pick(self, wanted: str) -> bool:
        """Type into the picker's search box, select the row, and commit.

        Three acts, because the widget has three: the search narrows, a row click only
        SELECTS (it enables the create button, which starts disabled), and the create
        button is what actually commits. Clicking the row and walking away left the
        dialog open, and its backdrop then swallowed the next step's click.
        """
        search = self.page.locator(".type-picker-search")
        try:
            search.wait_for(state="visible", timeout=3000)
        except Exception:
            return False
        # Typed, not filled: the list narrows key by key, which is the shot.
        search.type(wanted, delay=85 if self.paced else 0)
        self.page.wait_for_timeout(480)

        # By TEXT, which is the tier Playwright's hierarchy asks for: picker rows are
        # the one place in this UI carrying real user-facing text.
        #
        # EXACTLY, though, and against the row's own text nodes. A row renders as
        # [emoji span][name][role span] with no separator, so its full text reads
        # "Noiseeffect" and a substring match on "Noise" also hits "NoiseMeter" and
        # "PolarNoise". A MoonLive SCRIPT of the same name sits in the same list
        # (mlScriptItems adds them) and sorts first, so a loose match picked the script
        # and no module of the wanted type was created.
        rows = self.page.locator(".type-picker-item")
        idx = rows.evaluate_all("""(els, want) => els.findIndex(e =>
            [...e.childNodes].filter(n => n.nodeType === 3)
                .map(n => n.textContent).join('').trim() === want)""", wanted)
        if idx is None or idx < 0:
            self._dismiss_picker()
            return False

        self.tap(rows.nth(idx))
        self.page.wait_for_timeout(300)

        create = self.page.locator(".type-picker-actions .create")
        if not create.count():
            self._dismiss_picker()
            return False
        # The create button is what COMMITS. A tap that misses it leaves the dialog open
        # with nothing created, and add_module/replace_module only notice via _await_new;
        # pick_file has no second check at all, so the miss has to be reported here.
        if not self.tap(create):
            self._dismiss_picker()
            return False
        self.page.wait_for_timeout(420)
        # Whatever happened, do not leave a modal over the page: its backdrop
        # intercepts every later click and the failure surfaces steps away.
        self._dismiss_picker()
        return True

    def _dismiss_picker(self) -> None:
        """Close the type picker if one is still open, by Escape then by removal."""
        for _ in range(3):
            if not self.page.locator("dialog.type-picker-modal").count():
                return
            self.page.keyboard.press("Escape")
            self.page.wait_for_timeout(260)
        # A dialog that ignores Escape would block everything after it; drop it.
        self.page.evaluate(
            "document.querySelectorAll('dialog.type-picker-modal').forEach(d => d.remove())")

    def _await_new(self, before: set, type_name: str) -> str | None:
        """Poll state for the module the click created.

        Polled rather than slept: a single short wait raced the device, returned None,
        and every later step then hunted a card literally named "None".
        """
        for _ in range(24):
            def walk(ms):
                for m in ms:
                    if m.get("name") not in before and m.get("type") == type_name:
                        return m.get("name")
                    got = walk(m.get("children", []))
                    if got:
                        return got
                return None
            got = walk(state(self.host).get("modules", []))
            if got:
                return got
            time.sleep(0.25)
        return None

    def hero(self, seconds: float = 6.0) -> bool:
        """Fill the frame with the 3D preview alone, and hold.

        The preview is what the video is ABOUT, so the UI around it is chrome here
        rather than content. The canvases are reparented into a host that owns the
        viewport instead of being cropped in the edit: cropping a 534px canvas out of
        a 720p frame throws away most of the pixels and upscales what is left.

        Nothing is driven. Whatever the rig is already rendering is the shot.
        """
        self.page.add_style_tag(content="""
            body > *:not(#preview-host) { display: none !important; }
            html, body { margin: 0 !important; padding: 0 !important;
                         background: #000 !important; overflow: hidden !important; }
            #preview-host { position: fixed; inset: 0; display: grid;
                            place-items: center; background: #000; }
            #preview-host canvas { width: 100vmin !important; height: 100vmin !important; }
        """)
        moved = self.page.evaluate("""() => {
            const host = document.createElement('div');
            host.id = 'preview-host';
            document.body.appendChild(host);
            let n = 0;
            for (const id of ['preview', 'preview-labels']) {
                const c = document.getElementById(id);
                if (c) { host.appendChild(c); n++; }
            }
            return n;
        }""")
        if not moved:
            # RESTORE before returning. Leaving the blanking rule applied hid every
            # card, so the next step failed on a missing element rather than here.
            self.page.reload(wait_until="domcontentloaded")
            self.page.wait_for_selector(".card", state="attached", timeout=20000)
            return False
        self.page.wait_for_timeout(700)      # one repaint at the new size
        self._settle(seconds)

        # RESTORE the page. The style rule hides every element except the preview host,
        # so a later step looking for a card finds one that is display:none and fails
        # for a reason nothing in the run file explains. A reload is the whole restore:
        # moving the canvases back by hand first was redundant with it.
        self.page.reload(wait_until="domcontentloaded")
        self.page.wait_for_selector(".card", state="attached", timeout=20000)
        self.page.wait_for_timeout(900)
        return True

    def pick_file(self, module: str, control: str, filename: str) -> bool:
        """Choose a file for a filepath control, through its picker dialog.

        The MoonLive script control is a `button.fileedit-pick` that opens the same
        type-picker widget the module list uses, so the row is matched the same way:
        exactly, on the row's own text nodes.
        """
        btn = self.page.locator(
            f'button.fileedit-pick[data-mid="{self._css(module)}"]'
            f'[data-key="{self._css(control)}"]')
        # WAIT for the control. A file control is rendered when the card is rebuilt
        # after the module is registered, which is later than the module appearing in
        # state: tapping too early found no button, the pick silently did nothing, and
        # the run typed a script into an editor for a file that was never chosen.
        try:
            btn.first.wait_for(state="visible", timeout=6000)
        except Exception:
            return False
        if not self.tap(btn):
            return False
        return self._pick(filename)

    def type_script(self, text: str, delay: int = 45) -> bool:
        """Type into the MoonLive editor, character by character.

        The claim this shot makes is that TEXT becomes native code on the device, so
        the typing has to be visible: filling the field would show a finished script
        appearing from nowhere. The editor is `.fm-editor-body`, a plain textarea with
        no data-mid contract of its own, which is why this is selector-based.
        """
        ta = self.page.locator("textarea.fm-editor-body")
        if not self.tap(ta):
            return False
        self.page.keyboard.press("ControlOrMeta+A")
        self.page.keyboard.press("Backspace")
        self.page.keyboard.type(text, delay=delay if self.paced else 0)
        self.page.wait_for_timeout(700)

        # SAVE it. Typing alone changes a textarea; the device only sees the script
        # when the editor's Save button posts it, and that is the moment the whole shot
        # exists to show: text becomes running code. Leaving it unsaved also left the
        # card mid-edit, which is why the run's own delete step then failed.
        save = self.page.locator("button.fm-editor-save")
        if not save.count():
            return False
        if not self.tap(save):
            return False
        self.page.wait_for_timeout(1600)     # compile + first frame
        return True

    # -- selector actions ---------------------------------------------------
    #
    # The device UI is addressed through its module contract (data-module, data-mid),
    # which is what keeps those lookups resilient. The INSTALLER has no modules: it is
    # an ordinary page, so a run there names elements directly. Kept separate rather
    # than loosening the module actions, so the resilient path stays the default and
    # the raw one is visibly the exception.

    def click(self, selector: str) -> bool:
        """Click any element, by CSS selector."""
        return self.tap(self.page.locator(selector))

    def follow_link(self, selector: str, seconds: float = 2.5) -> bool:
        """Point at a link, open where it leads in this page, dwell, then come back.

        A link that opens a new tab cannot be filmed: the recorder captures one page, so the
        destination a viewer is being told about never appears. Following it in place shows the
        real page at the real URL, which is the whole point of demonstrating the button, and the
        viewer sees the same content either way. The pointer still travels to the link first, so
        the cause of the navigation is visible rather than a page that simply changes.
        """
        # The match that actually carries a link. `data-module` sits on the nav entry as well as
        # the card, so a card-scoped selector can still resolve to a nav button with no href.
        all_matches = self.page.locator(selector)
        el = next((m for m in (all_matches.nth(i) for i in range(all_matches.count()))
                   if m.get_attribute("href")), None)
        if el is None or not self._ready(el):
            return False
        box = el.bounding_box()
        if box:                      # point at it, so the navigation has a visible cause
            self.page.mouse.move(box["x"] + box["width"] / 2, box["y"] + box["height"] / 2)
            self._settle(0.5)
        # Resolved against the page: the `{ }` link is relative, so it follows whatever host the
        # UI is served from, and `goto` needs the absolute form.
        href = urljoin(self.page.url, el.get_attribute("href"))
        back = self.page.url
        try:
            # `commit` rather than `domcontentloaded`: a JSON response has no DOM to wait on, and
            # the `{ }` button leads to exactly that. The dwell below is what the viewer sees.
            resp = self.page.goto(href, wait_until="commit", timeout=20000)
            # A 404 renders as a page and films as success, so the status is what decides.
            reached = resp is None or resp.ok
            if not reached:
                self.failures.append(f"follow_link: {href} answered {resp.status}")
            self._settle(seconds)
        except Exception:
            reached = False          # an offline docs host is a bad take, not a crash
        finally:
            # Unconditional: a step that leaves the driver on another page makes every step after
            # it fail against the wrong document, which reads as a broken UI rather than a bad link.
            try:
                self.page.goto(back, wait_until="domcontentloaded", timeout=20000)
                self.open_app_wait()
            except Exception:
                reached = False
        return reached

    def start_process(self, command: str, name: str = "") -> bool:
        """Start a command alongside the run, so the interface can be filmed while it is driven.

        A scenario drives the device over REST while this drives the browser, which is the only way
        to film what a scenario does: the run file cannot perform those mutations itself without
        becoming a second copy of the scenario, and the two would then drift.
        The process is left running; `wait_process` collects it.
        """
        # `{host}` is the device this run is recording, so a command drives the same device the
        # camera is pointed at rather than whatever it would default to.
        command = command.replace("{host}", self.host)
        try:
            # Discarded rather than piped: nothing here reads it, and a full pipe buffer blocks the
            # child forever. A live scenario run is exactly the chatty case that would hit it.
            proc = subprocess.Popen(shlex.split(command), cwd=str(ROOT),
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as e:
            self.failures.append(f"start_process: {command!r} did not start ({e})")
            return False
        # One key for both actions, so a command started without a name is still findable.
        key = name or command
        running = self._processes.get(key)
        if running is not None and running.poll() is None:
            # Overwriting would orphan the live one, and wait_process could never account for it.
            proc.kill()
            self.failures.append(f"start_process: {key!r} is already running")
            return False
        self._processes[key] = proc
        return True

    def wait_process(self, name: str = "", timeout: float = 600.0) -> bool:
        """Wait for a started command to finish, so a take cannot end mid-scenario.

        A non-zero exit is a failed take rather than an exception: the run reports it the way it
        reports a step that did not complete, and the recording is refused.
        """
        proc = self._processes.get(name)
        if proc is None:
            self.failures.append(f"wait_process: nothing named {name!r} was started")
            return False
        end = self._now() + timeout
        while proc.poll() is None and self._now() < end:
            self._settle(1.0)        # paced, so the wait is the shot rather than dead air
            if not self.paced:
                # _settle returns at once when nothing is recording, so without this the loop spins
                # at full speed and the timeout is the only thing that ever ends it.
                time.sleep(1.0)
        if proc.poll() is None:
            proc.kill()
            self.failures.append(f"wait_process: {name!r} ran past {timeout}s")
            return False
        return proc.returncode == 0

    def open_app_wait(self) -> None:
        """Wait for the device UI to be driveable again after a navigation."""
        try:
            self.page.wait_for_selector(".card", state="attached", timeout=20000)
        except Exception:
            pass
        self.page.wait_for_timeout(700)

    def type_into(self, selector: str, text: str, delay: int = 180) -> bool:
        """Type into any field, slowly enough to watch a list narrow as it filters.

        The field is emptied first, so what it holds afterwards is what the run asked for.
        A field can arrive already filled: the installer prefills the SSID from the last
        install, and typing on top of that provisioned a device for "MoonModulesMoonModules",
        which joins nothing. Selecting what is there means the first keystroke replaces it,
        the way typing into a focused field does for a person.
        """
        el = self.page.locator(selector)
        if not self.tap(el):
            return False
        # Select-all rather than fill(): the keystrokes still happen, so a field that filters
        # a list as it is typed still shows that, which is the point of typing slowly here.
        self.page.keyboard.press("ControlOrMeta+a")
        self.page.keyboard.type(text, delay=delay if self.paced else 0)
        self.page.wait_for_timeout(420)
        return True

    def choose_option(self, selector: str, label: str) -> bool:
        """Pick from a plain <select> by its visible label, or a prefix of one.

        By LABEL, not index: a reordered option list would otherwise silently select
        something else and the take would be wrong in a way nothing catches.

        A trailing `*` matches on PREFIX, which is what a label carrying a clock needs.
        The installer renders each release through Intl.RelativeTimeFormat, so its
        options read "v4.0.0 - 3 weeks ago": an exact label goes stale within the week,
        and the run then fails for a reason that has nothing to do with the UI.
        """
        el = self.page.locator(selector)
        if not self._ready(el):
            return False
        box = el.first.bounding_box()
        if box:                       # point at it, so the change has a visible cause
            self.page.mouse.move(box["x"] + box["width"] / 2,
                                 box["y"] + box["height"] / 2)
            self._settle(0.4)
        if label.endswith("*"):
            value = el.first.evaluate(
                """(sel, p) => {
                    const o = [...sel.options].find(
                        o => o.textContent.trim().startsWith(p));
                    return o ? o.value : null;
                }""", label[:-1])
            if value is None:
                return False
            el.first.select_option(value=value)
        else:
            try:
                el.first.select_option(label=label)
            except Exception:
                return False
        self.page.wait_for_timeout(500)
        return True

    def goto(self, url: str) -> bool:
        """Open a page. A run that drives another app starts by going there."""
        self.page.goto(url, wait_until="domcontentloaded")
        self.page.wait_for_timeout(1200)
        return True

    def scroll_to(self, selector: str) -> bool:
        """Bring an element into view without clicking it."""
        el = self.page.locator(selector)
        if not self._ready(el):
            return False
        box = el.first.bounding_box()
        if box:
            self.page.mouse.move(box["x"] + box["width"] / 2,
                                 box["y"] + box["height"] / 2)
            self._settle(0.3)
        return True

    # -- controls -----------------------------------------------------------

    def set_number(self, module: str, control: str, value) -> bool:
        """Type into a control's box, the way a person does."""
        el = self.control(module, control, "input")
        if not self._ready(el):
            return False
        box = el.first.bounding_box()
        if box:                            # travel to the box before pressing in it
            self.page.mouse.move(box["x"] + box["width"] / 2,
                                 box["y"] + box["height"] / 2)
            self._settle(0.25)
        el.first.click(click_count=3)      # select what is there, so typing replaces it
        self.page.wait_for_timeout(160)
        self.page.keyboard.type(str(value), delay=130 if self.paced else 0)
        self.page.keyboard.press("Enter")
        self.page.wait_for_timeout(420)
        return True

    def set_text(self, module: str, control: str, value: str) -> bool:
        for kind in ('input[type="text"]', "textarea"):
            el = self.page.locator(
                f'{kind}[data-mid="{self._css(module)}"][data-key="{self._css(control)}"]')
            if not el.count():
                continue
            if not self._ready(el):
                continue
            box = el.first.bounding_box()
            if box:
                self.page.mouse.move(box["x"] + box["width"] / 2,
                                     box["y"] + box["height"] / 2)
                self._settle(0.25)
            el.first.click()
            self.page.keyboard.press("ControlOrMeta+A")
            self.page.keyboard.type(value, delay=50 if self.paced else 0)
            self.page.keyboard.press("Tab")
            self.page.wait_for_timeout(380)
            return True
        return False

    def drag_slider(self, module: str, control: str, to: float,
                    seconds: float = 1.8) -> bool:
        """Press, travel and release along the track, so the handle moves on camera."""
        el = self.control(module, control, 'input[type="range"]')
        if not self._ready(el):
            return False
        box = el.first.bounding_box()
        if not box:
            return False
        y = box["y"] + box["height"] / 2
        frac = float(el.first.evaluate("""e => {
            const min = Number(e.min || 0), max = Number(e.max || 100);
            return max > min ? (Number(e.value) - min) / (max - min) : 0;
        }"""))
        # INSET the travel by half a thumb. A range input's thumb is centered on the
        # track but its center never reaches the very edge, so a press at width*1.0
        # landed BESIDE the thumb: Chromium started no drag, the mouse moved with
        # nothing captured, and the value stayed put while the call reported success.
        # A click at the same x works, which is what proved the press was the problem.
        THUMB = 8.0
        span = max(1.0, box["width"] - 2 * THUMB)
        x0 = box["x"] + THUMB + span * frac
        x1 = box["x"] + THUMB + span * max(0.0, min(1.0, to))

        # The travel is UNANNOTATED, and that is the whole reason a drag used to cost
        # 33 seconds instead of 2. show_actions animates EVERY mouse move, so a 40-step
        # travel queued 40 annotations of 700ms each and the handle crawled. The press
        # and the release are the events worth naming; the path between them is motion,
        # not a sequence of actions.
        annotate = self.screencast.show_actions(cursor="pointer", duration=90) \
            if self.screencast else None

        self.page.mouse.move(x0, y)
        self.page.mouse.down()
        if annotate:
            annotate.__enter__()
        try:
            # Enough samples to read as continuous at video framerate, not one per
            # pixel: 14 over ~2s is a move every 140ms, which the encoder renders as
            # smooth travel while costing 14 round trips instead of 40.
            steps = 14
            per_ms = max(20, int(seconds * 1000 / steps))
            for i in range(1, steps + 1):
                self.page.mouse.move(x0 + (x1 - x0) * i / steps, y)
                self.page.wait_for_timeout(per_ms)
        finally:
            if annotate:
                annotate.__exit__(None, None, None)
        self.page.mouse.up()
        self.page.wait_for_timeout(280)

        # VERIFY the handle actually moved, and fall back to a click if it did not.
        # A drag that silently changes nothing is the failure this whole format exists
        # to catch: it returned True, the caption claimed a blend, and the value was
        # untouched. The click path is a real user gesture too (a range input jumps to
        # a clicked position), so the shot survives; it just loses the travel.
        try:
            lo = float(el.first.get_attribute("min") or 0)
            hi = float(el.first.get_attribute("max") or 100)
            want = lo + (hi - lo) * max(0.0, min(1.0, to))
            tolerance = max(2.0, (hi - lo) * 0.06)
            if abs(float(el.first.input_value()) - want) > tolerance:
                self.page.mouse.click(x1, y)
                self.page.wait_for_timeout(420)
                # REPORT a fallback that also missed. Returning True here made the one
                # failure this check exists to catch invisible again: only a step with
                # its own `expect` noticed, and most steps carry none.
                if abs(float(el.first.input_value()) - want) > tolerance:
                    return False
        except Exception:
            return False
        return True

    def choose(self, module: str, control: str, value: str) -> bool:
        """Pick from a <select>.

        The popup is drawn by the OS compositor and cannot be captured, so what the
        recording shows is the pointer on the control and the value changing.
        """
        el = self.control(module, control, "select")
        if not self._ready(el):
            return False
        try:
            el.first.select_option(label=value)
        except Exception:
            try:
                el.first.select_option(value=value)
            except Exception:
                return False
        self.page.wait_for_timeout(420)
        return True

    def click_control(self, module: str, control: str) -> bool:
        return self.tap(self.control(module, control, "button"))

    # -- the dispatcher -----------------------------------------------------

    # How each action reads its arguments. The dispatcher below is one line because
    # this is the only thing that differs between them: which keys a step carries and
    # what type each is. Actions that CREATE a module are named in _CREATES, so the
    # dispatcher knows to bind the result.
    _ARGS = {
        "chapter":        lambda a: (a.get("title", ""), a.get("description"),
                                     float(a.get("seconds", 2.0))),
        "wait":           lambda a: (float(a.get("seconds", 1.0)),),
        "wait_for":       lambda a: (a["selector"], a.get("text"),
                                     float(a.get("timeout", 120.0))),
        "open_card":      lambda a: (a["module"],),
        "add_module":     lambda a: (a["parent"], a["type"]),
        "replace_module": lambda a: (a["module"], a["type"]),
        "delete_module":  lambda a: (a["module"],),
        "clear_children": lambda a: (a["module"],),
        "set_number":     lambda a: (a["module"], a["control"], a["value"]),
        "set_text":       lambda a: (a["module"], a["control"], a["value"]),
        "drag_slider":    lambda a: (a["module"], a["control"], float(a["to"]),
                                     float(a.get("seconds", 1.8))),
        "choose":         lambda a: (a["module"], a["control"], a["value"]),
        "click_control":  lambda a: (a["module"], a["control"]),
        "click":          lambda a: (a["selector"],),
        "follow_link":    lambda a: (a["selector"], float(a.get("seconds", 2.5))),
        "start_process":  lambda a: (a["command"], a.get("name", "")),
        "wait_process":   lambda a: (a.get("name", ""), float(a.get("timeout", 600.0))),
        "type_into":      lambda a: (a["selector"], a["text"],
                                     int(a.get("delay", 180))),
        "choose_option":  lambda a: (a["selector"], a["value"]),
        "goto":           lambda a: (a["url"],),
        "scroll_to":      lambda a: (a["selector"],),
        "hero":           lambda a: (float(a.get("seconds", 6.0)),),
        "pick_file":      lambda a: (a["module"], a["control"], a["value"]),
        "type_script":    lambda a: (a["text"], int(a.get("delay", 45))),
    }
    _CREATES = {"add_module", "replace_module"}

    def _do_chapter(self, title, description, seconds) -> bool:
        self.chapter(title, description, seconds)
        return True

    def _do_wait(self, seconds) -> bool:
        # Paced OR not: a wait is the run file asking for time, which a test can skip
        # but a caption cannot. _settle already no-ops when nothing is recording.
        self._settle(seconds)
        return True

    def wait_for(self, selector: str, text: str | None = None, timeout: float = 120.0) -> bool:
        """Hold until the page says so, rather than for a guessed number of seconds.

        A step whose length the device decides (a flash, a reboot, a network join) has no
        honest fixed duration: too short cuts the shot, too long pads every take. This waits
        on the page's own evidence, so the same run file works on a fast link and a slow one.

        A state already passed counts as seen. A progress element reports a sequence, and a
        step polling it starts after the step before it finished, so a state shorter than that
        gap is over before anyone looks: the erase on an S3 lasts about twelve seconds and the
        step waiting for it begins later than that. Every text this element shows is therefore
        remembered as it goes by, and the wait is satisfied by the history as well as by the
        present. Otherwise a run file would have to name a timeout per state that is really a
        guess about the hardware, which is the guesswork this action exists to remove.
        """
        self._watch(selector)
        end = self._now() + timeout
        while self._now() < end:
            seen = self._note_text(selector)
            if text is None:
                # Non-empty, because the installer blanks this element before it fills it in:
                # an attached-but-empty element is the state before the thing being waited for.
                if seen:
                    return True
            elif any(text.lower() in s for s in self._seen_text.get(selector, ())):
                return True
            # Paced, the dwell IS the poll and the wait is visible in the recording. Unpaced,
            # _settle returns at once, so the sleep has to come from here or this spins a core.
            if self.paced:
                self._settle(0.5)
            else:
                self.page.wait_for_timeout(100)
                self._note_text(selector)
        raise TimeoutError(f"wait_for: {selector!r} never showed {text!r} within {timeout}s"
                           + (f" (it showed: {sorted(self._seen_text.get(selector, ()))})"
                              if self._seen_text.get(selector) else ""))

    def _watch(self, selector: str) -> None:
        """Sample a progress element from now on, including between steps.

        A state is missed in the gap between one step finishing and the next starting, which no
        amount of remembering inside a wait can recover. So a watched selector is sampled by
        every paced dwell in the run, which is what makes the record continuous.
        """
        self._watched.add(selector)
        self._note_text(selector)

    def _note_text(self, selector: str) -> str | None:
        """Read an element and remember what it said, for a wait that arrives late."""
        try:
            el = self.page.query_selector(selector)
        except Exception:
            return None         # an element mid-render, or a page mid-navigation
        if el is None:
            return None
        try:
            txt = (el.text_content() or "").strip()
        except Exception:
            return None
        self._seen_text.setdefault(selector, set()).add(txt.lower())
        return txt

    def _act(self, a: str, args: dict) -> tuple[bool, str | None]:
        """Perform one action. Returns (completed, name-of-anything-created)."""
        method_name = ACTIONS.get(a)
        if not method_name:
            raise ValueError(f"unknown action: {a}")
        method = getattr(self, method_name)
        result = method(*self._ARGS[a](args))
        if a in self._CREATES:
            return result is not None, result
        return bool(result), None

    def perform(self, step: Step) -> bool:
        """Do one step, narrate it, then check it. Returns False on a failure.

        Verification is the same read whichever caller is driving: a test turns it
        into an assertion, a recording turns it into a warning that the take is bad.
        """
        a = step.action
        # A binding a previous step failed to make surfaces HERE otherwise, one step
        # after the step that actually broke, pointing at the reference rather than
        # the cause. Report it against the step that could not resolve, and stop:
        # continuing would chase a module that was never created.
        try:
            args = {k: self.resolve(v) for k, v in step.args.items()}
        except KeyError as e:
            self.failures.append(f"{a}: {e.args[0]}")
            return False

        ok = True
        created: str | None = None

        # Only ONE root's subtree is in the DOM (renderCards), so a step naming a
        # module in a closed root must open it first. Done here rather than in each
        # action: every action that names a module needs it, which makes it one rule
        # instead of a branch per action.
        target = args.get("module") or args.get("parent")
        if target and a not in ("chapter", "wait", "open_card", "click", "type_into",
                                "choose_option", "goto", "scroll_to"):
            self.reveal(target)

        # The action runs INSIDE the caption's overlay, so the words are on screen while
        # the thing they describe happens. show_overlay blocks for its duration, so
        # raising it before the action froze the screen and the words could never
        # coincide with the change they narrate.
        #
        # The hold runs inside it too: it is the dwell the run asks for on this step, so
        # the words have to still be up during it.
        if step.caption:
            with self.caption(step.caption):
                ok, created = self._act(a, args)
                self._settle(step.hold)
        else:
            ok, created = self._act(a, args)
            self._settle(step.hold)        # video-only: _settle is a no-op unpaced

        if step.bind and created:
            self.bindings[step.bind] = created

        if not ok:
            # Name the module, not the whole arg dict: a failed add is read as "could
            # not add NoiseEffect under Layer", which says where to look.
            what = args.get("module") or args.get("parent") or ""
            detail = f" ({args.get('type')})" if args.get("type") else ""
            self.failures.append(f"{a} on {what!r}{detail} did not complete")

        if ok and step.expect:
            e = {k: self.resolve(v) for k, v in step.expect.items()}
            # Settle before reading: the device applies on its next frame, and a read
            # issued in the same millisecond as the keystroke sees the old value.
            self.page.wait_for_timeout(500)
            got = control_value(self.host, e["module"], e["control"])
            if not _same(got, e["value"]):
                ok = False
                self.failures.append(
                    f'{e["module"]}.{e["control"]} is {got!r}, expected {e["value"]!r}')

        self._settle(step.hold)        # video-only: _settle is a no-op unpaced
        return ok

    def run_all(self, run: Run) -> list[str]:
        """Perform every step. Returns the failures, empty when the run was clean.

        STOPS at an unresolved binding. A step naming `{ripples}` when nothing bound it
        means the step that should have created it failed, so everything after is
        chasing a module that does not exist: the run cannot recover, and continuing
        only records minutes of a take nobody can use.
        """
        for step in run.steps:
            before = len(self.failures)
            self.perform(step)
            if len(self.failures) > before and any(
                    "which no earlier step bound" in f for f in self.failures[before:]):
                self.failures.append("stopped: the rest of the run depends on that step")
                break
        return self.failures


# The ONE inventory of actions: name -> the Driver method that performs it.
#
# Hand-kept lists drifted twice while this was built (the dispatcher and the test's
# KNOWN_ACTIONS), each time surfacing as a confusing mid-run failure. The dispatcher
# calls through this, the test derives its vocabulary from it, and RUNS.md's table is
# checked against it. No caption duration here: a caption lives exactly as long as the
# `with` block around its step, so there is nothing to estimate.
ACTIONS: dict[str, str] = {
    "chapter":        "_do_chapter",
    "wait":           "_do_wait",
    "wait_for":       "wait_for",
    "open_card":      "open_card",
    "add_module":     "add_module",
    "replace_module": "replace_module",
    "delete_module":  "delete_module",
    "clear_children": "clear_children",
    "set_number":     "set_number",
    "set_text":       "set_text",
    "drag_slider":    "drag_slider",
    "choose":         "choose",
    "click_control":  "click_control",
    # Selector actions: another MoonLight surface, with no module contract of its own.
    "click":          "click",
    "follow_link":    "follow_link",
    "start_process":  "start_process",
    "wait_process":   "wait_process",
    "type_into":      "type_into",
    "choose_option":  "choose_option",
    "goto":           "goto",
    "scroll_to":      "scroll_to",
    # Shot actions: a framing, and the two halves of the MoonLive editor.
    "hero":           "hero",
    "pick_file":      "pick_file",
    "type_script":    "type_script",
}


def _same(got, want) -> bool:
    """Compare a control's value with what the run expected.

    Numbers cross the API as int or float and a run file writes whichever reads best,
    so 32 and 32.0 are the same answer; everything else compares as text, which keeps
    "true"/True and 5/"5" from being spurious failures.
    """
    if got is None:
        return False
    try:
        return abs(float(got) - float(want)) < 1e-6
    except (TypeError, ValueError):
        return str(got).strip().lower() == str(want).strip().lower()
