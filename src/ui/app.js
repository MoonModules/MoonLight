// MoonLight Web UI: all logic in one hand-maintained file per CLAUDE.md.
// Loaded as <script type="module"> so it can import the shared install-picker
// component used by both the device UI (here, OTA flash) and the GitHub Pages
// installer (first flash via Web Serial). Module loading is deferred by
// default; entry-point is the WS init at the bottom: no ordering surprises.
import { installPicker } from "/install-picker.js";
import { preview } from "/preview3d.js";
import { isNewer, parse } from "/semver.js";
import { applyMigrations, collectFiles, restoreDirs, diffRestore } from "/migrate.js";

// Sections (top to bottom):
//   1. State + storage
//   2. WebSocket (with keepalive, visibility pause, bfcache, exponential backoff)
//   3. REST helpers + module mutations
//   4. Render pipeline: render() → renderNav() → renderCards() → createCard() → createControl()
//   5. State patching (no-rebuild contract): updateValues() + updateModuleControls()
//   6. Type picker
//   7. Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile)
//   (3D WebGL preview lives in preview3d.js: imported as `preview`)
//   8. Status bar wiring (device name, sys stats, theme, reboot)
//   9. Boot
//
// Load-bearing invariants:
//   - dragTs[mid:key] cooldown: ignore WS pushes for a control the user has touched
//     in the last 1s. Prevents slider snap-back during drag.
//   - ctrl.hidden: skip rendering hidden controls (plan-10 feature). Persistence still
//     loads them: toggling visibility doesn't lose state.
//   - No-rebuild contract: WS state updates patch values in place via querySelector.
//     We only rebuild the DOM on structural changes (add/delete/move) and explicit
//     select-driven defineControls rebuilds.

// ---------------------------------------------------------------------------
// 1. State + storage
// ---------------------------------------------------------------------------

let state = null;
let selectedModule = null;
let availableTypes = [];        // populated from GET /api/types after first connection
let ws = null;
const WS_RETRY_MIN_MS = 200;     // first reconnect is quick so a dropped initial connect (common on Safari,
                                 // whose first attempt can lose a contended socket) comes back near-instantly
let wsRetryMs = WS_RETRY_MIN_MS; // exponential backoff: 200 → 400 → 800 → … → 5000 ceiling
let wsHeartbeat = null;
let wsReconnectTimer = null;     // the pending reconnect setTimeout: tracked so pagehide can cancel it
let wsPaused = false;            // gated by document.visibilityState
let wsUnloading = false;         // true once the page starts unloading (refresh/navigate): suppresses the
                                 // reconnect on the closing socket so the departing page doesn't error-log

const dragTimers = {};           // per-control debounce timers (clearTimeout handles)
const dragTs = {};               // per-control last-touched timestamp (ms): a short post-interaction cooldown
// Control types whose value the user can edit: updateModuleControls suppresses a
// WS state push for one of these while the user is mid-edit. The read-only types
// (display/display-int/time/progress) and the composite `list` are absent on
// purpose: they always reflect the latest push.
const EDITABLE_CONTROL_TYPES = new Set(
    ["uint8", "uint16", "int16", "int32", "pin", "bool", "text", "textarea", "filepath", "password", "select",
     "palette", "ipv4"]);

// localStorage keys per ui.md
const LS_SELECTED  = "mm_selectedRoot";
const LS_THEME     = "mm_theme";
const LS_TABS      = "mm_selectedTabs";   // { [containerName]: childName }: the open tab per container
const LS_EXPANDED  = "mm_expanded";       // [moduleName, …]: modules whose "controls" <details> is open
const LS_TA_SIZE   = "mm_textareaSizes";  // { "<module>:<control>": heightPx }: user-dragged textarea heights
const LS_CARDS_W   = "mm_cardsWidth";     // px: dragged width of the docked card column (right side)

// The open tab per container, persisted so a reload doesn't dump you back on the first child.
let selectedTabs = {};
try { selectedTabs = JSON.parse(lsRead(LS_TABS, "{}")) || {}; } catch { selectedTabs = {}; }

// Which modules have their "controls" disclosure open: VIEW-ONLY state the backend knows nothing about, so
// it lives here (like selectedTabs), NOT in the module state. Persisting it means a full-state rebuild (or a
// page reload) restores the open/closed expander instead of snapping it shut. Value/structure/picker state
// all come from the backend, so those need no client persistence.
let expandedSet = new Set();
try { expandedSet = new Set(JSON.parse(lsRead(LS_EXPANDED, "[]")) || []); } catch { expandedSet = new Set(); }
function saveExpanded() { localStorage.setItem(LS_EXPANDED, JSON.stringify([...expandedSet])); }

// The height a user dragged each textarea to: again VIEW-ONLY state the backend doesn't own (like the tab
// and expander state above), keyed by "<module>:<control>". Persisting it means a resized script/config box
// keeps its size across a full-state rebuild and a page reload instead of snapping back to the 2-row default.
let textareaSizes = {};
try { textareaSizes = JSON.parse(lsRead(LS_TA_SIZE, "{}")) || {}; } catch { textareaSizes = {}; }
function saveTextareaSize(key, heightPx) { textareaSizes[key] = heightPx; localStorage.setItem(LS_TA_SIZE, JSON.stringify(textareaSizes)); }

// The width the user dragged the docked card column to: VIEW-ONLY state (like the tab/expander/textarea
// state above). Applied as the --cards-width CSS custom property (the #main flex-basis reads it, clamped in
// CSS so it can't crowd out the preview or vanish); restored on boot. Only meaningful in docked mode, where
// the cards sit beside the preview; PiP/narrow mode makes them full-width and hides the handle.
function applyCardsWidth(px) { document.documentElement.style.setProperty("--cards-width", px + "px"); }
(function restoreCardsWidth() {
    const w = parseInt(lsRead(LS_CARDS_W, ""), 10);
    if (Number.isFinite(w) && w > 0) applyCardsWidth(w);
})();

// Wire the card-column resize handle (index.html #cards-resize). Dragging it left/right sets --cards-width
// live and persists the final value. The handle sits at the LEFT edge of #main, so dragging left widens the
// cards (they grow toward the preview); width = the pane's right edge minus the pointer x. Bounded by the
// same clamp the CSS enforces, and coalesced through requestAnimationFrame so a drag doesn't thrash layout.
function setupCardsResize() {
    const handle = document.getElementById("cards-resize");
    const main = document.getElementById("main");
    if (!handle || !main) return;
    const MIN = 280, MAX = 900;
    let dragging = false, raf = 0, pendingW = 0;
    const onMove = (clientX) => {
        // #main's right edge is fixed (the workspace's right edge); width grows as the pointer moves left.
        const right = main.getBoundingClientRect().right;
        pendingW = Math.max(MIN, Math.min(MAX, Math.round(right - clientX)));
        if (raf) return;
        raf = requestAnimationFrame(() => { raf = 0; applyCardsWidth(pendingW); });
    };
    const stop = () => {
        if (!dragging) return;
        dragging = false;
        handle.classList.remove("dragging");
        document.body.classList.remove("cards-resizing");
        if (pendingW > 0) localStorage.setItem(LS_CARDS_W, String(pendingW));
        window.removeEventListener("pointermove", onPointerMove);
        window.removeEventListener("pointerup", stop);
        window.removeEventListener("pointercancel", stop);
    };
    const onPointerMove = (e) => { if (dragging) onMove(e.clientX); };
    handle.addEventListener("pointerdown", (e) => {
        // Only resize in docked mode: the handle is CSS-hidden otherwise, but guard anyway.
        if (!document.querySelector(".workspace")?.classList.contains("mode-docked")) return;
        e.preventDefault();
        dragging = true;
        pendingW = main.getBoundingClientRect().width;
        handle.classList.add("dragging");
        document.body.classList.add("cards-resizing");
        window.addEventListener("pointermove", onPointerMove);
        window.addEventListener("pointerup", stop);
        window.addEventListener("pointercancel", stop);
    });
    // Double-click resets to the default width (a common resize-handle affordance).
    handle.addEventListener("dblclick", () => { applyCardsWidth(480); localStorage.setItem(LS_CARDS_W, "480"); });
}

function lsRead(key, defaultVal) {
    const v = localStorage.getItem(key);
    return v !== null ? v : defaultVal;
}

// MEASURED time by default, not derived rate. A module's tickTimeUs is what it actually cost;
// the fps beside it is that number inverted, which reads as a frame rate the device could hit if
// this module were the only thing running. On a card that is a claim about the whole pipeline made
// from one part of it, so the honest default is the microseconds. The toggle still cycles to fps
// for anyone comparing against a target rate.
let timingMode = "ms";   // the RESTING state; a press peeks at fps and release restores it
let theme      = lsRead(LS_THEME, "dark");

// ---------------------------------------------------------------------------
// 2. WebSocket
// ---------------------------------------------------------------------------

function connectWs() {
    if (wsReconnectTimer) { clearTimeout(wsReconnectTimer); wsReconnectTimer = null; }
    if (ws) {
        try { ws.close(); } catch {}
        ws = null;
    }
    const url = `ws://${location.host}/ws`;
    ws = new WebSocket(url);
    const sock = ws;   // captured so a stale socket's late callback (after a reconnect swapped `ws`) is a no-op
    ws.binaryType = "arraybuffer";

    ws.onopen = () => {
        if (sock !== ws) return;   // a newer socket already took over: ignore this stale open
        wsRetryMs = WS_RETRY_MIN_MS;                        // reset backoff
        setWsDot(true);
        // Keepalive ping every 25s: Safari kills idle WebSockets otherwise
        clearInterval(wsHeartbeat);
        wsHeartbeat = setInterval(() => {
            if (ws && ws.readyState === WebSocket.OPEN) ws.send("ping");
        }, 25000);
    };

    ws.onmessage = (e) => {
        if (sock !== ws || wsPaused) return;   // ignore a stale socket's late frame
        // Binary never arrives here: the preview has its own `/wsp` connection (see
        // connectPreview below), so this socket carries only control-plane JSON. A binary frame
        // would mean an older firmware still multiplexing both, so keep handling it.
        if (e.data instanceof ArrayBuffer) {
            preview.onBinaryMessage(e.data);
            return;
        }
        try {
            const data = JSON.parse(e.data);
            if (!data) return;
            // The device sends a FULL {modules:[...]} state on connect / after a structural change,
            // then a {patch:[...]} of only-changed leaves each second (diff-on-the-wire: the whole
            // module tree is ~34 KB of mostly-unchanging metadata that must NOT be re-serialized every
            // second on the render thread; see HttpServerModule buildStatePatch). Apply a patch onto
            // the existing `state` in place, then refresh the DOM. A patch before any full state is
            // ignored (we have nothing to patch); the device resyncs on connect so this self-corrects.
            if (Array.isArray(data.patch)) {
                if (state && Array.isArray(state.modules)) {
                    applyStatePatch(data.patch);
                    updateValues();
                    // A targetFps slider move arrives as a PATCH, and the preview controller aims
                    // at whatever it last heard: without this, the advertised detail/smoothness
                    // chooser only takes effect on the next reconnect.
                    preview.setTargetFps(previewTargetFps(state));
                }
                return;
            }
            // The same /ws also carries WLED-compatibility {state,info} frames for the native WLED app
            // (see HttpServerModule's WLED shim). Those aren't our module-state shape: ignore anything
            // without a `modules` array, or it would clobber `state` and blank the module view.
            if (!Array.isArray(data.modules)) return;
            state = data;
            // Defer the DOM rebuild while the user is mid-interaction: a full state arrives on every
            // (re)connect, and rebuilding under an open dropdown makes it unselectable. The state is
            // already stored above, so updateValues() below still shows fresh values; the structural
            // render happens on the next full state once the interaction ends.
            if (userIsEditing()) { updateValues(); preview.setTargetFps(previewTargetFps(state)); return; }
            restoreSelectedRoot();   // before the render that reads it: this may be the FIRST state
            renderCards();     // a full state may add/remove/reshape cards (structural resync): full render
            // The nav is built from the same tree, so a structural change (a module added or removed)
            // must rebuild it too, or the sidebar keeps entries the state no longer has. AFTER
            // renderCards, because its fallback may reassign selectedModule and the nav highlights it.
            renderNav();
            updateValues();
            preview.setTargetFps(previewTargetFps(state));
        } catch {
            // ignore malformed messages
        }
    };

    ws.onclose = () => {
        if (sock !== ws) return;   // a stale socket closing after we already moved on: leave the live one alone
        clearInterval(wsHeartbeat);
        wsHeartbeat = null;
        if (wsUnloading) return;   // the page is going away, don't reconnect (and don't touch the DOM)
        setWsDot(false);
        // A long-dead socket may mean the device switched images under this tab (a suspended
        // tab restored from memory never re-runs the page-load check). Once the backoff has
        // ripened, ask; the update overlay owns that conversation during an install, so skip
        // while it is up.
        if (wsRetryMs >= 5000 && !document.querySelector(".fw-overlay")) {
            moonbaseHandoff();   // async; reconnects continue unless it flips the page
        }
        // Exponential backoff with 5s ceiling; track the timer so pagehide can cancel a pending reconnect.
        wsReconnectTimer = setTimeout(connectWs, wsRetryMs);
        wsRetryMs = Math.min(wsRetryMs * 2, 5000);
    };

    ws.onerror = () => { /* onclose will fire next */ };
}

function setWsDot(connected) {
    const dot = document.getElementById("ws-dot");
    if (!dot) return;
    dot.className = connected ? "ws-dot connected" : "ws-dot disconnected";
}

// Visibility / bfcache hooks: ONE handler per event (two handlers for the same event is the
// split-rule trap). wsPaused gates message handling for the interval where a socket is open but
// the tab just hid.
document.addEventListener("visibilitychange", () => {
    wsPaused = (document.visibilityState === "hidden");
    if (document.hidden) {
        closePreviewSocket();                      // stop the stream and the bad measurements NOW
        wsHideTimer = setTimeout(() => {
            hiddenByVisibility = true;
            wsUnloading = true;                    // suppress the auto-reconnect while hidden
            // A backoff reconnect armed BEFORE the tab hid would otherwise fire afterwards and
            // open a fresh control socket on a hidden tab, exactly the work this closes.
            if (wsReconnectTimer) { clearTimeout(wsReconnectTimer); wsReconnectTimer = null; }
            if (ws) { try { ws.close(); } catch {} }
        }, WS_HIDE_GRACE_MS);
    } else {
        clearTimeout(wsHideTimer);
        wsHideTimer = null;
        if (hiddenByVisibility) {
            hiddenByVisibility = false;
            wsUnloading = false;
            connectWs();                           // control first: the full state repaints the UI
        }
        // the preview follows the pane's own wants-frames state
        if (previewWanted) connectPreview();
    }
});
window.addEventListener("pageshow", (e) => {
    if (e.persisted) {
        // Safari restored from bfcache: re-establish state
        wsPaused = false;
        wsUnloading = false;   // a bfcache-restored page is live again: allow reconnects
        if (!ws || ws.readyState !== WebSocket.OPEN) connectWs();
    }
});
// Close the socket cleanly as the page unloads (a refresh, a navigation, or a bfcache suspend). Without
// this the departing page's socket is torn down abnormally by the browser, which logs a "connection lost"
// error to the console every refresh. Sending a normal (1000) close first, and marking wsUnloading so
// onclose skips its reconnect, makes the handover silent. pagehide (not beforeunload) fires for bfcache too.
window.addEventListener("pagehide", () => {
    wsUnloading = true;
    clearInterval(wsHeartbeat);
    if (wsReconnectTimer) { clearTimeout(wsReconnectTimer); wsReconnectTimer = null; }   // no reconnect after unload
    if (ws) { try { ws.close(1000); } catch { /* already closing */ } }
    if (wsPreview) { try { wsPreview.close(1000); } catch { /* already closing */ } }   // same silent handover for /wsp
});

// ---------------------------------------------------------------------------
// 3. REST helpers + module mutations
// ---------------------------------------------------------------------------

async function init() {
    applyTheme(theme);
    setupStatusBarButtons();
    setupUpdateBadge();
    setupCardsResize();
    // Open the WebSocket FIRST, before any HTTP fetch. The device pushes a full {modules} state on connect
    // (handleWebSocketUpgrade → requestFullResync), so the WS is the primary state source: the /api/state
    // fetch below is only a first-paint shortcut. Connecting first matters most on Safari: it opens more
    // parallel connections up front and is quicker to give up on a contended one, so if the WS is opened
    // LAST (after several awaited fetches + the page's file loads) it lands in the most-saturated moment of
    // the device's small socket pool and Safari abandons it: the "basic UI shows but the WS never goes
    // live" symptom. Opening it first lets it grab an uncontended slot; Chrome tolerated the old order, so
    // this fixes Safari without regressing Chrome.
    connectWs();
    preview.init();
    preview.setupLayout();
    // Open the preview channel only while the pane wants frames, and close it the moment it does
    // not, the device then builds nothing at all for a dismissed preview.
    // The pull model's uplink: tiny request messages ([0x51][stride][fps] standing,
    // [0x52][stride] one-shot) on the socket the pane already holds.
    preview.onSendRequest((bytes) => {
        if (wsPreview && wsPreview.readyState === WebSocket.OPEN)
            wsPreview.send(Uint8Array.from(bytes));
    });
    preview.onWantsFrames((wanted) => {
        previewWanted = wanted;
        if (wanted) { wspRetryMs = WSP_RETRY_MIN_MS; connectPreview(); }
        else disconnectPreview();
    });
    // First-paint shortcut: render from a one-shot /api/state so the cards appear immediately instead of
    // waiting for the WS's first full-state push. The WS then keeps everything live. If this fetch fails
    // (a contended slot), it's non-fatal: the WS full state fills in the moment it lands. Since the WS is
    // opened FIRST, its full-state push can beat this await; when it has (state already set), DON'T let the
    // REST snapshot overwrite the newer, live WS state: just skip the commit.
    try {
        const resp = await fetch("/api/state");
        if (resp.status === 404 && await moonbaseHandoff()) return;
        if (resp.ok && (!state || !Array.isArray(state.modules))) {
            const snap = await resp.json();
            if (snap && Array.isArray(snap.modules)) {
                state = snap;
                restoreSelectedRoot();
                // Default to the first root AS LISTED, not as scheduled: otherwise a device with no
                // saved selection opens on a card that is not the one the nav highlights at the top.
                if (!selectedModule && state.modules.length > 0) {
                    selectedModule = navRoots(state.modules)[0].name;
                }
                renderNav();
                renderCards();
                updateStatusBar();
            }
        }
    } catch { /* non-fatal: the WS full state renders the UI when it arrives */ }
    // /api/types arrived in plan-11; fetch in parallel. When it arrives, the reset-to-default buttons (whose
    // defaults come from this payload) need to appear: but a full renderCards() rebuilds the DOM, which
    // would blow away a control the user is mid-edit (this fires ~1 s after first paint, so that's rare but
    // possible). Skip the re-render while an editable field is focused / a native select is open; the reset
    // buttons then appear on the next structural render instead of interrupting the edit.
    fetch("/api/types").then(r => r.json()).then(j => {
        availableTypes = j.types || [];
        if (state && !userIsEditing()) renderCards();
    }).catch(() => {});
}

// The PREVIEW socket, a second connection, deliberately.
//
// Preview frames are LOSSY and large (hundreds of KB/s on a big layout); control-plane state is
// small and must not be delayed. Sharing one TCP connection makes the small messages queue behind
// the large ones, textbook head-of-line blocking, which showed up as a flickering connection dot
// and a UI that stopped responding while a big preview streamed. Separate connections is the
// standard remedy, and it lets the device raise preview resolution instead of capping it to
// protect the control plane.
//
// The device streams only to clients on this channel, so CLOSING this socket stops the work at the
// source: a dismissed preview costs the device nothing.
let wsPreview = null;
let wspRetryTimer = null;   // pending preview reconnect, cancelled on close/hide
let previewWanted = false;              // does the pane currently want frames?
const WSP_RETRY_MIN_MS = 1000, WSP_RETRY_MAX_MS = 15000;
let wspRetryMs = WSP_RETRY_MIN_MS;

// A Select's value is normally the option INDEX, but a label-persisted Select (a NIC list, an
// audio device list) sends the option STRING; resolve either to the index both render paths
// need. Unknown label -> 0, so a vanished option shows the first row rather than a blank box.
function selectIndex(ctrl) {
    if (typeof ctrl.value === "string") {
        const i = (ctrl.options || []).indexOf(ctrl.value);
        return i >= 0 ? i : 0;
    }
    return ctrl.value ?? 0;
}

function connectPreview() {
    if (wsPreview && (wsPreview.readyState === WebSocket.OPEN ||
                      wsPreview.readyState === WebSocket.CONNECTING)) return;
    const p = new WebSocket(`ws://${location.host}/wsp`);
    wsPreview = p;
    p.binaryType = "arraybuffer";
    p.onmessage = (e) => {
        if (p !== wsPreview || wsPaused) return;   // a stale socket's late frame is a no-op
        if (e.data instanceof ArrayBuffer) preview.onBinaryMessage(e.data);
    };
    // RECONNECT. A dropped preview socket must come back on its own: WiFi hiccups, a device
    // reboot, or the transient close a browser does when a tab is backgrounded all end the socket
    // while the pane is still visible, and without a retry the preview stayed dead until the user
    // pressed refresh (observed on an S3 over WiFi, 2026-08-25). Backoff so a device that REFUSES
    // the upgrade (its preview cap is full) is not hammered; reset on a successful open.
    p.onopen = () => {
        if (p !== wsPreview) return;
        wspRetryMs = WSP_RETRY_MIN_MS;
        preview.adaptStart();   // the client-side controller runs only while this socket lives
    };
    p.onclose = () => {
        if (p !== wsPreview) return;   // superseded by a newer socket, nothing to do
        wsPreview = null;
        preview.adaptStop();
        if (!previewWanted) return;    // the pane was dismissed; staying closed is correct
        // Tracked so closePreviewSocket can cancel it: an untracked retry armed just before a
        // tab-hide would reopen the preview on a hidden tab and undo the hibernation.
        wspRetryTimer = setTimeout(() => {
            wspRetryTimer = null;
            if (previewWanted && !wsPreview && !document.hidden) connectPreview();
        }, wspRetryMs);
        wspRetryMs = Math.min(wspRetryMs * 2, WSP_RETRY_MAX_MS);
    };
    p.onerror = () => { try { p.close(); } catch {} };   // onclose above does the retry
}

/// Close the socket without touching `previewWanted`, the pane's intent survives a tab-hide,
/// which is what lets the return path reopen it.
function closePreviewSocket() {
    if (wspRetryTimer) { clearTimeout(wspRetryTimer); wspRetryTimer = null; }
    if (!wsPreview) return;
    const p = wsPreview;
    wsPreview = null;
    preview.adaptStop();
    try { p.close(); } catch {}
}

function disconnectPreview() {
    previewWanted = false;    // the pane was dismissed: intent gone too
    closePreviewSocket();
}

// TAB VISIBILITY → socket hibernation. A hidden tab must cost the device NOTHING: its throttled
// timers make it the worst client shape, subscribed but draining at a trickle, and its ~0 fps
// measurements would coarsen the shared preview for everyone (coarsest request wins). So:
//   hidden  → the preview socket closes IMMEDIATELY (the expensive stream, and the bad measurer);
//             the control socket follows after a grace, so a quick alt-tab never pays the ~30 KB
//             full-state resync. With both closed the device does nothing but render effects.
//   visible → control reconnects first (the resync repaints the UI), then the preview via the
//             normal wants-frames path, whose controller restarts fresh at full detail.
const WS_HIDE_GRACE_MS = 10000;
let wsHideTimer = null;
let hiddenByVisibility = false;


// The user's Preview targetFps, from the state tree, the client-side controller aims for it.
function previewTargetFps(st) {
    let v = 0;
    const walk = (ms) => { for (const m of ms || []) {
        if (m.type === "PreviewDriver")
            for (const c of m.controls || []) if (c.name === "targetFps") v = c.value;
        walk(m.children);
    } };
    walk(st && st.modules);
    return v;
}

// Is the user mid-interaction with the page? A full renderCards() rebuilds the DOM, so it destroys
// an open native select, a field being typed into, or TEXT THE USER HAS SELECTED, the control or
// the selection vanishes from under the cursor.
// ONE home for the test, because both re-render triggers need it: the /api/types arrival above and
// the WebSocket full state. The WS case is the one users hit: a full state is sent on every connect,
// so a browser that drops and reconnects (a big layout on modest hardware) re-renders repeatedly,
// and a dropdown can become impossible to click before it disappears.
//
// A selection counts because copying a value out of the UI (a device name, an IP, a measurement)
// is a normal thing to do and rebuilding under it collapses the highlight mid-drag, which reads as
// the page fighting the user. It is deliberately narrow: a COLLAPSED selection (a plain caret, or
// the empty range a click leaves behind) is not an interaction and must not park the UI on stale
// structure. Live VALUES keep flowing either way, since updateValues() patches in place.
function userIsEditing() {
    const el = document.activeElement;
    if (el && (el.matches("input, textarea") || el.closest("select")
               || document.querySelector('select[data-open="true"]'))) return true;
    const sel = window.getSelection();
    return !!(sel && !sel.isCollapsed && sel.rangeCount > 0 && String(sel).length > 0);
}

// The message for a failed fetch Response: the server's own `{"error": …}` body (JSON, e.g.
// "not enough space (N free)") when present, else a bare `HTTP <status>`. Every /api/* handler
// returns errors as that JSON shape, so this is the one place the extraction lives.
async function errorMessage(res) {
    try {
        const j = await res.json();
        if (j && j.error) return j.error;
    } catch (_) { /* non-JSON error body: fall through to the status code */ }
    return `HTTP ${res.status}`;
}

async function sendControl(moduleName, controlName, value) {
    // Eagerly update the local `state` to what we just sent: the standard controlled-input
    // pattern. Without this, `state` keeps the OLD value until the device echoes the change back in a
    // value patch (up to a tick1s later, or folded into a full resync for a control that triggers a
    // rebuild like a driver's ledsPerPin). In that gap, an unrelated WS frame runs updateModuleControls,
    // and once the dragTs edit-guard expires (>1s after the last keystroke: trivial if the user pauses)
    // it writes the stale `state` value straight back into the field the user just changed (the value
    // "reverses"; a manual refresh shows the correct value because it refetches). The client knows what
    // it sent, so update `state` now; any later echo just confirms it.
    const mod = (state && Array.isArray(state.modules))
        ? allModules().find(m => m.name === moduleName) : null;
    if (mod) {
        const ctrl = Array.isArray(mod.controls) && mod.controls.find(c => c.name === controlName);
        if (ctrl) ctrl.value = value;
        // Siblings on the same target move with it. The device already does this (followTargets runs
        // on every surface write), but the browser would not see it until the next 1 Hz push, so a
        // second fader bound to the same control lagged a second behind the one being dragged.
        if (ctrl && ctrl.target) {
            for (const c of mod.controls)
                if (c !== ctrl && c.target === ctrl.target) c.value = value;
            updateModuleControls(mod);
        }
    }
    // Toggling expert mode changes which controls RENDER (the `advanced` ones), not just a value: so
    // re-render the cards. Structural change, same as an add/remove; the value write above already landed.
    if (moduleName === "System" && controlName === "mode") renderCards();
    // Best-effort by design: failures are not retried here. Non-ok responses +
    // network errors are logged to console so a user with devtools open can see
    // what went wrong (e.g. a control value the device-side validator rejected).
    try {
        const res = await fetch("/api/control", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({module: moduleName, control: controlName, value: value})
        });
        if (!res.ok) {
            console.warn(`[control] POST ${moduleName}.${controlName} failed (status=${res.status})`);
        }
        // The Firmware card's `image` picks WHICH PARTITION every other control describes, so the
        // device rebinds version/build/firmware/partition to the other image and the card has to
        // be redrawn from the new values. Refetched AFTER the POST rather than re-rendered before
        // it (the mode case above), because the new values only exist once the device has
        // switched. The routine WS push cannot carry it: renderCards is suppressed while the user
        // is interacting, and operating this select is exactly that.
        else if (moduleName === "Firmware" && controlName === "image") refetchState();
        // Four writes change what a MoonCloud card shows: pressing `send` publishes a message,
        // pressing `send update` re-sends this device's report, and either member's `consent` decides
        // whether that member reads at all. Typing in `message` or toggling `shareName` changes only
        // the device, and re-reading after those showed exactly what was already on screen.
        else if (mod?.type === "MoonTalkModule" ? (controlName === "send" || controlName === "consent")
               : mod?.type === "MoonStatsModule" ? (controlName === "consent" || controlName === "send update")
               : false) {
            // Switching consent OFF changes nothing on the server (a report already sent stays
            // sent), so the card rebuilds from what it has and nothing is re-read. The rebuild is
            // what hides the data.
            const turnedOn = Boolean(value);
            if (!turnedOn) { moonCloudGeneration++; refetchState(); return; }

            // Switching ON: ONE read, once. A Stats report leaves from tick1s rather than during
            // this write, so reading immediately would show the totals without this device's own
            // install, the one row the reader is looking for. A Talk message is sent inside the
            // write, so its read needs no delay. An earlier shape read three times to cover the
            // uncertainty and rebuilt the card three times with it.
            moonCloudGeneration++;
            setTimeout(() => {
                moonCloudStatsCache.clear();
                moonTalkCache = null;
                refetchState();
            // The wait is for `consent` alone: that report leaves from tick1s, so an immediate read
            // would show the totals without the very row the reader is looking for. `send update`
            // sends inside this write, like MoonTalk's `send`, so it re-reads with no delay.
            }, mod?.type === "MoonStatsModule" && controlName === "consent" ? 2000 : 0);
        }
    } catch (e) {
        console.warn(`[control] POST ${moduleName}.${controlName} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function refetchState() {
    try {
        const r = await fetch("/api/state");
        state = await r.json();
        renderNav();
        renderCards();
    } catch {}
}

// --- Editable-list row ops (the client half of the editable List primitive) ---
// Same best-effort fetch style as sendControl: non-ok + network errors are logged,
// not retried. After a successful mutation the server persists + re-runs prepare and
// pushes fresh state over the WS; we also refetchState() to match the module add/
// delete/move ops (which don't wait for the WS push either). The row `id` is a number;
// it goes into the URL path as-is. `c` (control name) may contain any chars, so encode.
async function listAddRow(moduleName, ctrlName) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}`, {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: "{}"
        });
        if (!res.ok) { console.warn(`[list] add ${moduleName}.${ctrlName} failed (status=${res.status})`); return null; }
        const j = await res.json();
        refetchState();
        return j && typeof j.id === "number" ? j.id : null;
    } catch (e) {
        console.warn(`[list] add ${moduleName}.${ctrlName} failed (error=${e && e.message ? e.message : e})`);
        return null;
    }
}

async function listDeleteRow(moduleName, ctrlName, id) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}/${id}`, {method: "DELETE"});
        if (!res.ok) { console.warn(`[list] delete ${moduleName}.${ctrlName}#${id} failed (status=${res.status})`); return; }
        refetchState();
    } catch (e) {
        console.warn(`[list] delete ${moduleName}.${ctrlName}#${id} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function listMoveRow(moduleName, ctrlName, id, to) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}/${id}`, {
            method: "PATCH",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({to: to})
        });
        if (!res.ok) { console.warn(`[list] move ${moduleName}.${ctrlName}#${id}→${to} failed (status=${res.status})`); return; }
        refetchState();
    } catch (e) {
        console.warn(`[list] move ${moduleName}.${ctrlName}#${id} failed (error=${e && e.message ? e.message : e})`);
    }
}

async function listSetField(moduleName, ctrlName, id, field, value) {
    try {
        const res = await fetch(`/api/list/${encodeURIComponent(moduleName)}/${encodeURIComponent(ctrlName)}/${id}`, {
            method: "PATCH",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify({field: field, value: value})
        });
        if (!res.ok) { console.warn(`[list] set ${moduleName}.${ctrlName}#${id}.${field} failed (status=${res.status})`); return false; }
        // A FIELD edit does NOT refetch: the value is already applied server-side and the WS push
        // reconciles it, so rebuilding here would collapse the open row and snap an in-progress edit
        // back (a `channels` change also reshapes the row's fields: that structural update rides the
        // WS push, guarded by the dragTs cooldown, not a rebuild-on-every-keystroke). Structural ops
        // (add/delete/move) DO refetch: they change the row set. Return true so the caller can decide.
        return true;
    } catch (e) {
        console.warn(`[list] set ${moduleName}.${ctrlName}#${id}.${field} failed (error=${e && e.message ? e.message : e})`);
        return false;
    }
}

/// Create a module, optionally under a chosen name, and return the name it got.
///
/// `id` names the new module. The endpoint treats it as IDEMPOTENT (a module of that name already
/// there is success, not a rename), and answers that case without a `name`: so a caller that wants
/// a fresh module reads the absence of a name as "taken" and asks again with another. Returns null
/// when nothing was created, for either reason.
async function addModule(type, parentName, id) {
    if (!type) return null;
    const body = {type: type};
    if (parentName) body.parent_id = parentName;
    if (id) body.id = id;
    let name = null;
    try {
        const r = await fetch("/api/modules", {
            method: "POST",
            headers: {"Content-Type": "application/json"},
            body: JSON.stringify(body)
        });
        name = (await r.json()).name || null;   // the created module's final name; absent if taken
    } catch {}
    // A name already in use: the caller decides whether to retry, and re-fetching state here would
    // cost a round trip per attempt.
    if (id && !name) return null;
    // Select the new module's tab BEFORE the re-render so renderCards shows it active (the tab strip
    // reads selectedTabs[parent]); then scroll it into view and focus its first control so a keyboard
    // user lands on it. Without this the view stays on the previously-active tab and the new module
    // is added out of sight.
    if (name && parentName) {
        selectedTabs[parentName] = name;
        localStorage.setItem(LS_TABS, JSON.stringify(selectedTabs));   // persist like the tab-click path
    }
    await refetchState();
    if (name) focusModule(name);
    return name;
}

// Bring a module's card into view and focus its first control (added via the "+" flow).
function focusModule(name) {
    const card = queryByName(`.card[data-module="${cssEscape(name)}"]`, "data-module", name);
    if (!card) return;
    // A child card can wrap its controls in a collapsed <details> (.card-controls-collapse): open it
    // FIRST so the card is at its expanded height, THEN scroll: scrolling a still-collapsed card lands
    // on its pre-expansion geometry and the focused control ends up mispositioned.
    const collapse = card.querySelector("details.card-controls-collapse");
    if (collapse) collapse.open = true;
    card.scrollIntoView({ block: "nearest", behavior: "smooth" });
    // Focus the first real control input, NOT the tab strip / header buttons: and NOT the status row,
    // which is a `.control-row` with only spans (a freshly added driver leads with a status, so picking
    // the first `.control-row` would find no input and focus nothing). Query for the input directly
    // inside any control row so the status row is skipped.
    const first = card.querySelector(".control-row input, .control-row select, .control-row textarea, .control-row button");
    if (first) first.focus({ preventScroll: true });
}

async function deleteModule(name) {
    await fetch("/api/modules/" + encodeURIComponent(name), {method: "DELETE"});
    refetchState();
}

// move to absolute index (0..siblings.length-1). Called from drag-and-drop.
async function moveModuleTo(name, toIndex) {
    await fetch("/api/modules/" + encodeURIComponent(name) + "/move", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({to: toIndex})
    });
    refetchState();
}

// swap a module for another type at the same position. The replacement starts
// with its own default control values: a clean swap, not a value carry-over.
/// Swap a module for another type, optionally renaming it.
///
/// `newName` is for a caller that knows what the slot now holds: replacing one script with another
/// leaves a card labeled after the old script unless the name travels with it. Omitted, the device
/// keeps a custom name and refreshes a default one, which is what a plain type swap wants.
async function replaceModule(name, newType, newName) {
    if (!newType) return;
    const body = {type: newType};
    if (newName) body.name = newName;
    await fetch("/api/modules/" + encodeURIComponent(name) + "/replace", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify(body)
    });
    // AWAITED, because a caller may address the slot straight afterwards: the replace can rename it
    // (a module still carrying its old type's default name takes the new type's), and only the
    // refreshed state says what it is called now.
    await refetchState();
}

async function rebootDevice() {
    try {
        await fetch("/api/reboot", {method: "POST"});
    } catch { /* connection may drop mid-response: that's the device restarting */ }
    // WS will reconnect on its own via onclose backoff
}

// ---------------------------------------------------------------------------
// 4. Render pipeline
// ---------------------------------------------------------------------------

// The order roots are LISTED in, which is deliberately not the order they RUN in.
//
// main.cpp orders by dependency: Filesystem before anything that writes a file, System before the
// modules that read its identity: and that order is load-bearing, so it cannot be reshuffled to
// suit a menu. But it reads as an implementation detail to someone using the device: the first
// thing they see is System, and the lights are at the bottom.
//
// So the nav states its own order, grouped by what a user is looking for: the thing they reach for
// most (Control), then the light pipeline in pipeline order (where the lights are → what color →
// how it gets out), then the device itself. A root NOT named here still appears, after these, in
// scheduler order: so adding a module never makes it invisible, it just lands at the end until
// someone decides where it belongs.
// GROUPS, not one flat list: the nav draws a rule between them, so the grouping has to be the
// data rather than an index the renderer counts to.
const NAV_GROUPS = [
    ["Control"],
    ["Layouts", "Effects", "Drivers"],                          // the light pipeline, in pipeline order
    ["System", "File Manager", "Network", "Services", "Firmware"],
];
const NAV_ORDER = NAV_GROUPS.flat();

/// Roots in nav order, grouped: each entry is an array of the modules present from one NAV_GROUP,
/// with anything NAV_GROUPS does not name appended as a final group in scheduler order. Empty
/// groups are dropped, so a build without (say) Services never draws a rule around nothing.
function navGroups(modules) {
    const byName = new Map(modules.map(m => [m.name, m]));
    const groups = NAV_GROUPS.map(g => g.map(n => byName.get(n)).filter(Boolean));
    const unlisted = modules.filter(m => !NAV_ORDER.includes(m.name));
    if (unlisted.length) groups.push(unlisted);
    return groups.filter(g => g.length);
}

/// Flat nav order: the same modules navGroups returns, ungrouped. Used where only the ORDER
/// matters (picking the default selection), so the two can never disagree about what comes first.
function navRoots(modules) {
    return navGroups(modules).flat();
}

function renderNav() {
    const nav = document.getElementById("nav");
    if (!nav || !state) return;
    nav.innerHTML = "";

    // One entry per root module. Clicking selects that root: only the selected
    // root's card subtree is rendered (one root visible at a time).
    const list = document.createElement("div");
    list.className = "nav-list";
    navGroups(state.modules).forEach((group, gi) => {
        // A rule BETWEEN groups, never before the first or after the last. `<hr>` rather than a
        // styled div: it is the element that means "thematic break", so a screen reader announces
        // the grouping instead of reading one long undifferentiated list.
        if (gi > 0) {
            const rule = document.createElement("hr");
            rule.className = "nav-sep";
            list.appendChild(rule);
        }
        for (const mod of group) {
            const item = document.createElement("button");
            item.type = "button";
            item.className = "nav-item";
            item.textContent = mod.name;
            item.dataset.module = mod.name;
            if (mod.name === selectedModule) item.classList.add("active");
            item.addEventListener("click", () => selectModule(mod.name));
            list.appendChild(item);
        }
    });
    nav.appendChild(list);
    nav.appendChild(buildNavFooter());
}

// Footer pinned to the bottom of the side nav: copyright + social links.
function buildNavFooter() {
    const footer = document.createElement("footer");
    footer.className = "nav-footer";

    const links = document.createElement("div");
    links.className = "nav-social";
    const SOCIAL = [
        ["GitHub",  "https://github.com/MoonModules/projectMM",
         "M12 .5C5.65.5.5 5.65.5 12a11.5 11.5 0 0 0 7.86 10.92c.58.1.79-.25.79-.56v-2c-3.2.7-3.88-1.54-3.88-1.54-.53-1.34-1.3-1.7-1.3-1.7-1.06-.72.08-.71.08-.71 1.17.08 1.79 1.2 1.79 1.2 1.04 1.79 2.73 1.27 3.4.97.1-.76.41-1.27.74-1.56-2.55-.29-5.24-1.28-5.24-5.69 0-1.26.45-2.29 1.19-3.1-.12-.29-.52-1.46.11-3.05 0 0 .97-.31 3.18 1.18a11 11 0 0 1 5.8 0c2.2-1.49 3.17-1.18 3.17-1.18.63 1.59.23 2.76.11 3.05.74.81 1.19 1.84 1.19 3.1 0 4.42-2.69 5.39-5.25 5.68.42.36.8 1.08.8 2.18v3.23c0 .31.21.67.8.56A11.5 11.5 0 0 0 23.5 12C23.5 5.65 18.35.5 12 .5Z"],
        ["Discord", "https://discord.gg/TC8NSUSCdV",
         "M20.32 4.37A19.8 19.8 0 0 0 15.45 2.9a13.6 13.6 0 0 0-.62 1.27 18.3 18.3 0 0 0-5.67 0A13 13 0 0 0 8.54 2.9 19.7 19.7 0 0 0 3.67 4.37C.57 8.96-.27 13.44.15 17.85a19.9 19.9 0 0 0 6 3.03c.49-.66.92-1.36 1.29-2.1-.71-.27-1.39-.6-2.03-.99.17-.12.34-.25.5-.38a14.2 14.2 0 0 0 12.18 0c.16.13.33.26.5.38-.64.39-1.32.72-2.03.99.37.74.8 1.44 1.29 2.1a19.8 19.8 0 0 0 6-3.03c.5-5.1-.85-9.55-3.58-13.48ZM8.02 15.13c-1.18 0-2.15-1.08-2.15-2.41 0-1.33.95-2.42 2.15-2.42 1.2 0 2.17 1.1 2.15 2.42 0 1.33-.95 2.41-2.15 2.41Zm7.96 0c-1.18 0-2.15-1.08-2.15-2.41 0-1.33.95-2.42 2.15-2.42 1.2 0 2.17 1.1 2.15 2.42 0 1.33-.95 2.41-2.15 2.41Z"],
        ["Reddit",  "https://reddit.com/r/moonmodules",
         "M22 12c0-1.1-.9-2-2-2-.55 0-1.04.22-1.4.58a9.8 9.8 0 0 0-5.1-1.55l.87-4.1 2.85.6a1.5 1.5 0 1 0 .15-1l-3.18-.67a.5.5 0 0 0-.59.38l-.97 4.57a9.8 9.8 0 0 0-5.16 1.55A2 2 0 1 0 4 13.66a3.9 3.9 0 0 0-.05.6c0 3.3 3.86 5.98 8.62 5.98 4.76 0 8.62-2.68 8.62-5.98 0-.2-.02-.4-.05-.6.53-.36.86-.96.86-1.66ZM8 13.5a1.5 1.5 0 1 1 3 0 1.5 1.5 0 0 1-3 0Zm8.32 4.07c-1.04 1.04-3.02 1.12-3.6 1.12-.58 0-2.57-.08-3.6-1.12a.4.4 0 0 1 .56-.56c.65.65 2.05.88 3.04.88.99 0 2.39-.23 3.04-.88a.4.4 0 0 1 .56.56ZM16 15a1.5 1.5 0 1 1 0-3 1.5 1.5 0 0 1 0 3Z"],
        ["YouTube", "https://www.youtube.com/@MoonModulesLighting",
         "M23.5 6.5a3 3 0 0 0-2.12-2.12C19.5 3.87 12 3.87 12 3.87s-7.5 0-9.38.51A3 3 0 0 0 .5 6.5C0 8.38 0 12 0 12s0 3.62.5 5.5a3 3 0 0 0 2.12 2.12c1.88.51 9.38.51 9.38.51s7.5 0 9.38-.51a3 3 0 0 0 2.12-2.12C24 15.62 24 12 24 12s0-3.62-.5-5.5ZM9.6 15.6V8.4l6.2 3.6-6.2 3.6Z"],
    ];
    for (const [name, url, path] of SOCIAL) {
        const a = document.createElement("a");
        a.href = url;
        a.target = "_blank";
        a.rel = "noopener";
        a.title = name;
        a.setAttribute("aria-label", name);
        a.innerHTML = `<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="${path}"/></svg>`;
        links.appendChild(a);
    }
    footer.appendChild(links);

    // Diagnostic bundle download. Fetches /api/state + /api/system from
    // the *same* origin we're on (the device itself): sidesteps Chrome's
    // mixed-content blocker that prevents the install page (HTTPS Pages)
    // from doing the same fetch against the device (HTTP LAN). Output is
    // a single JSON blob the user can attach to a bug report.
    const diag = document.createElement("a");
    diag.href = "#";
    diag.className = "nav-diag-link";
    diag.textContent = "Download diagnostics";
    diag.addEventListener("click", async (ev) => {
        ev.preventDefault();
        try {
            const [stateResp, systemResp] = await Promise.all([
                fetch("/api/state"),
                fetch("/api/system"),
            ]);
            const [stateJson, systemJson] = await Promise.all([
                stateResp.json(),
                systemResp.json(),
            ]);
            const bundle = {
                capturedAt: new Date().toISOString(),
                origin: location.origin,
                state: stateJson,
                system: systemJson,
            };
            const blob = new Blob([JSON.stringify(bundle, null, 2)],
                                  { type: "application/json" });
            // Devicename comes from system.deviceName if present, else
            // falls back to the hostname (e.g. "MM-BD3C.local") so the
            // filename is still useful when SystemModule's wire shape
            // doesn't include the name field.
            const devName = (systemJson && systemJson.deviceName)
                || location.hostname || "device";
            const fname = `MoonLight-diag-${devName}-${Date.now()}.json`;
            const a = document.createElement("a");
            const blobUrl = URL.createObjectURL(blob);
            a.href = blobUrl;
            a.download = fname;
            a.click();
            // Defer the revoke so the browser has time to start the download.
            // Revoking immediately after click() is technically race-safe on
            // recent Chrome / Firefox (the click navigation is synchronous)
            // but Safari has been observed dropping downloads under a fast
            // revoke. A few seconds is the canonical workaround.
            setTimeout(() => URL.revokeObjectURL(blobUrl), 4000);
        } catch (e) {
            alert(`Diagnostic capture failed: ${e && e.message ? e.message : e}`);
        }
    });
    footer.appendChild(diag);

    const copy = document.createElement("div");
    copy.className = "nav-copyright";
    copy.textContent = `© ${new Date().getFullYear()} MoonModules`;
    footer.appendChild(copy);

    return footer;
}

function selectModule(name) {
    // Surface demo sweep (REMOVABLE): leaving a surface re-arms it, so re-opening demos again while
    // a re-render of the SAME surface (a pad click) does not.
    if (name !== selectedModule) surfaceDemoShownFor = null;
    selectedModule = name;
    localStorage.setItem(LS_SELECTED, name);
    document.querySelectorAll(".nav-item").forEach((el) => {
        el.classList.toggle("active", el.dataset.module === name);
    });
    renderCards();
    closeNavDrawer();
}

/// The module holding `name` as a child, or null at the top level.
///
/// Used after a replace to resolve a slot whose name was disambiguated: the parent bounds the
/// search to the siblings the swap happened among.
/// Restore the root the user was last on, once the tree is known.
///
/// Called from BOTH state arrivals, because either can be first: the WebSocket full state and the
/// /api/state fetch race on load, and only the fetch used to consult this. When the socket won, the
/// selection stayed null and renderCards fell back to the first root, so a refresh landed on Control
/// instead of the Layer or File Manager the user left open. Intermittent exactly as a race is, and
/// self-correcting after any nav click, which sets the selection in memory.
///
/// A saved name that is no longer in the tree is ignored, leaving the fallback to pick.
function restoreSelectedRoot() {
    if (selectedModule) return;                       // an explicit choice this session wins
    if (!state || !Array.isArray(state.modules) || !state.modules.length) return;
    const saved = lsRead(LS_SELECTED, null);
    if (saved && state.modules.some(m => m.name === saved)) selectedModule = saved;
}

function findParentOf(name, modules) {
    if (!modules) modules = state.modules;
    for (const m of modules) {
        const kids = m.children || [];
        if (kids.some(k => k.name === name)) return m;
        const found = findParentOf(name, kids);
        if (found) return found;
    }
    return null;
}

function findModule(name, modules) {
    if (!modules) modules = state.modules;
    for (const m of modules) {
        if (m.name === name) return m;
        if (m.children) {
            const found = findModule(name, m.children);
            if (found) return found;
        }
    }
    return null;
}

// How much the reader wants to see: the System module's `mode` control, 0 user, 1 expert, 2
// developer. A control carries the mode it needs (`minMode`) and renders when the reader is at
// least there. Read live from state so a change takes effect on the next render with no reload;
// 0 when System or the control isn't present yet, so a device mid-boot shows the show and nothing
// that needs explaining.
function uiMode() {
    const sys = state ? findModule("System") : null;
    const c = sys && sys.controls && sys.controls.find(c => c.name === "mode");
    return c ? (c.value | 0) : 0;
}

function renderCards() {
    const main = document.getElementById("main");
    if (!main || !state) return;
    renderedMode_ = uiMode();   // what these cards are being built for
    main.innerHTML = "";

    // One root visible at a time: render only the selected root's subtree.
    // Falls back to the first root if the selection is missing or stale.
    let root = selectedModule ? findModule(selectedModule) : null;
    if (!root && state.modules.length > 0) {
        root = navRoots(state.modules)[0];   // the first NAV entry, matching what is highlighted
        selectedModule = root.name;
    }
    if (root) renderModuleTree(root, main, 0);
    // Surface demo sweep (REMOVABLE): delete this line and the block above it to remove the feature.
    // `main` is the root of the rendered subtree, so it holds the encoder strips, the pad grid and
    // the fader strips wherever they sit in the card nesting.
    if (root && main.querySelector(".list-pads-fixed")) startSurfaceDemo(main, root.name);
}

// ============================================================================
// Surface demo sweep: REMOVABLE BLOCK
//
// A hardware control desk sweeps its motorised faders and rings when it powers up, both to show it
// is alive and to show what it has. This does the same for three seconds after the surface appears.
//
// It is deliberately isolated so it can be deleted whole: this block, plus the single
// `startSurfaceDemo()` call at the end of renderCards(). It is a VIEW-ONLY animation: it never
// calls sendControl, so it cannot write a swept value to the device, and the first WebSocket patch
// after it finishes restores whatever the device actually holds.
// ============================================================================
const SURFACE_DEMO_MS = 1000;
let surfaceDemoUntil = 0;
/// The module the sweep last ran for. renderCards() fires on every state push and every mutation (a
/// pad click refetches and re-renders), so "the surface was rendered" is NOT "the surface was
/// opened": without this the sweep replayed on every button press.
let surfaceDemoShownFor = null;

function startSurfaceDemo(root, moduleName) {
    // The sweep is pure decoration, so it is the first thing to drop for someone who asked for less
    // motion. Checked before the once-per-surface flag, so the flag is not burned by a skipped run.
    if (window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    if (surfaceDemoShownFor === moduleName) return;   // already demoed this surface
    // Every bank is queried from the SAME root. The encoder and fader strips sit on the top-level
    // card while the pad grid is inside a nested one, so scoping to the card that contains the pads
    // found the pads and nothing else.
    const q = (sel) => [...root.querySelectorAll(sel)];
    // Every bank is re-queried each frame, for the same reason the pads are: a state push mid-sweep
    // re-renders the card and replaces these nodes. A list captured once would keep writing values to
    // detached elements while the visible controls sat frozen: which is what stopped the knobs and
    // faders a few hundred ms into a refresh, while the (live-queried) pads ran the full period.
    const liveInputs = () => [
        ...q(".control-encoder .encoder-input"),
        ...q(".control-fader .fader-input"),
    ];
    const knobs  = q(".control-encoder .encoder-input");
    const faders = q(".control-fader .fader-input");
    // Pads are looked up LIVE each frame rather than captured: a state push mid-sweep re-renders the
    // card and replaces these nodes, and a captured list would spend the rest of the run toggling
    // classes on detached elements: which is why the chase stopped partway through.
    const livePads = () => q(".list-pad:not(.list-pad-empty)");
    if (!knobs.length && !faders.length && !livePads().length) return;
    surfaceDemoShownFor = moduleName;

    // No snapshot: the demo starts on the first render, BEFORE the caller assigns input.value, so a
    // snapshot here captures the browser's default (50) and restoring it would overwrite every real
    // value. The device state is the truth, so the sweep ends by replaying it.
    const start = performance.now();
    surfaceDemoUntil = start + SURFACE_DEMO_MS;

    const step = (now) => {
        const t = (now - start) / SURFACE_DEMO_MS;
        if (t >= 1) {
            // Put the real values back by re-applying device state, which also repaints the knobs
            // and readouts through the normal patch path.
            surfaceDemoUntil = 0;
            updateValues();
            livePads().forEach(p => p.classList.remove("list-pad-demo"));
            return;
        }
        // A travelling wave rather than every control moving together: the phase offset per column is
        // what makes it read as a sweep across the desk.
        liveInputs().forEach((el, i) => {
            const lo = Number(el.min) || 0, hi = Number(el.max) || 255;
            const phase = t * 1.5 * Math.PI * 2 - i * 0.5;
            el.value = Math.round(lo + (hi - lo) * (0.5 + 0.45 * Math.sin(phase)));
            redrawRangeDecorations(el);
        });
        // The pads chase in sequence, one lit at a time, so the grid shows its extent. Paced by TIME
        // rather than by pad count, so the chase runs at the same speed and for the full duration
        // whatever number of presets exist.
        const pads = livePads();
        if (pads.length) {
            const lit = Math.floor(t * SURFACE_DEMO_MS / 120) % pads.length;
            pads.forEach((p, i) => p.classList.toggle("list-pad-demo", i === lit));
        }
        requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
}

/// True while the sweep is running: updateValues() skips patching the swept inputs, so a WebSocket
/// push mid-demo does not fight the animation for the same DOM node.
function surfaceDemoRunning() { return surfaceDemoUntil > performance.now(); }
// ======================= end REMOVABLE BLOCK ================================

function renderModuleTree(mod, parentEl, depth) {
    const { card, childrenEl } = createCard(mod, depth);
    parentEl.appendChild(card);
    // Children render inside this card's .card-children wrapper, not as flat
    // siblings. childrenEl is null for modules that don't accept children.
    if (!childrenEl || !mod.children || mod.children.length === 0) return;

    // One rule: a TOP-LEVEL module shows its children one at a time behind a tab strip, so its card
    // stays short however many children it has. Deeper levels stack as before. The tabs are derived
    // from mod.children on every render, so adding a layer adds its tab: there is no tab registry
    // to keep in sync, which is the whole of the "dynamic" requirement.
    if (depth === 0) {
        // The "+" tab takes over the add affordance, so hide the footer's duplicate button. The
        // footer element stays: it is what the tab's handler walks up to find this card's mod.
        const addBtn = card.querySelector(".card-footer > .add-btn");
        if (addBtn) addBtn.style.display = "none";
        renderChildTabs(mod, childrenEl, depth);
        return;
    }
    for (const child of mod.children) {
        renderModuleTree(child, childrenEl, depth + 1);
    }
}

// Drag a tab onto another to reorder: the tab strip IS the child list, so reordering tabs reorders
// the modules. Deliberately the same insert-semantics + moveModuleTo() call the card drag uses
// (attachDragHandlers): one reorder path, so a tab drag and a card drag can't drift apart.
function attachTabDragHandlers(tab, child, parent) {
    tab.draggable = true;

    tab.addEventListener("dragstart", (e) => {
        e.stopPropagation();                       // don't let the enclosing card's dragstart claim it
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", child.name);
        tab.classList.add("dragging");
    });
    tab.addEventListener("dragend", () => {
        tab.classList.remove("dragging");
        document.querySelectorAll(".tab.drag-over").forEach(t => t.classList.remove("drag-over"));
    });
    tab.addEventListener("dragover", (e) => {
        const src = document.querySelector(".tab.dragging");
        if (!src || src === tab) return;
        if (src.parentElement !== tab.parentElement) return;   // same strip only: not another container's
        e.preventDefault();
        tab.classList.add("drag-over");
    });
    tab.addEventListener("dragleave", () => tab.classList.remove("drag-over"));
    tab.addEventListener("drop", (e) => {
        e.preventDefault();
        e.stopPropagation();
        tab.classList.remove("drag-over");
        const srcName = e.dataTransfer.getData("text/plain");
        if (!srcName || srcName === child.name) return;
        // Re-find the index by NAME, not from the captured render-time object: state is replaced on
        // every WS push, so `parent` here would be stale within ~1 s (same trap the card drag notes).
        const live = findModule(parent.name);
        const targetIdx = ((live && live.children) || []).findIndex(c => c.name === child.name);
        if (targetIdx < 0) return;
        moveModuleTo(srcName, targetIdx);
    });
}

// A tab carries its module's fault severity as a dot, because a tab that can HIDE an error is worse
// than no tab: a driver failing on a background tab must still be visible from the strip.
function applyTabDot(tab, mod) {
    // Touch the DOM only when the dot actually changes. This runs for every module on every
    // state patch (updateTabDot), and removing or inserting a node inside the tab collapses any
    // selection the user is holding in that card, so an unconditional remove-and-recreate made
    // text impossible to select and copy: the same rule setText follows for text nodes.
    const want = (mod.severity === "error" || mod.severity === "warning") ? mod.severity : null;
    const old = tab.querySelector(".tab-dot");
    const have = old ? old.className.replace("tab-dot tab-dot-", "") : null;
    if (have === want) return;                  // nothing to do: leave the DOM alone
    if (old) old.remove();
    if (!want) return;
    const dot = document.createElement("span");
    dot.className = "tab-dot tab-dot-" + want;
    tab.appendChild(dot);
}

// Patch-path twin of applyTabDot + the disabled greying: the tab strip is built in renderCards(), which the
// WS value patch deliberately never re-runs: so without this, a fault (or an enable/disable) on a background
// tab would stay invisible until the next full render. (The UI has two render paths; a rule must live in both.)
function updateTabDot(mod) {
    const tab = queryByName(`.tab[data-tab-mid="${cssEscape(mod.name)}"]`, "data-tab-mid", mod.name);
    if (!tab) return;
    applyTabDot(tab, mod);
    tab.classList.toggle("tab--disabled", mod.enabled === false);   // grey a disabled module's tab title
}

// Tab strip + the one selected child. `active` falls back to the first child whenever the remembered
// tab is gone (the driver was deleted) or was never set, so the selection can never dangle.
function renderChildTabs(mod, childrenEl, depth) {
    const names = mod.children.map(c => c.name);
    let active = selectedTabs[mod.name];
    if (!names.includes(active)) active = names[0];

    const strip = document.createElement("div");
    strip.className = "tab-strip";
    strip.setAttribute("role", "tablist");

    for (const child of mod.children) {
        const tab = document.createElement("button");
        // Grey a disabled child's tab title (mirrors the card's card--disabled), so it reads as inactive
        // from the strip without opening it. Derived purely from child.enabled: no backend round-trip.
        tab.className = "tab" + (child.name === active ? " tab-active" : "")
                              + (child.enabled === false ? " tab--disabled" : "");
        tab.type = "button";
        tab.setAttribute("role", "tab");
        tab.setAttribute("aria-selected", child.name === active ? "true" : "false");
        // Name + the module's own status dot, so a driver erroring on a background tab is still
        // visible without opening it: a tab that can hide a fault is worse than no tab.
        tab.dataset.tabMid = child.name;   // so updateTabDot can find it on the WS patch path
        tab.textContent = child.name;
        applyTabDot(tab, child);
        attachTabDragHandlers(tab, child, mod);
        tab.addEventListener("click", () => {
            selectedTabs[mod.name] = child.name;
            localStorage.setItem(LS_TABS, JSON.stringify(selectedTabs));
            renderCards();
        });
        strip.appendChild(tab);
    }

    // "+" lives at the END OF THE STRIP, where a new tab appears: not in the card footer below the
    // panel, which would read as "add something to the open driver" rather than "add a driver".
    // createCard still renders the footer button for non-tabbed containers; here we hide it and put
    // the affordance where the tabs are.
    if (acceptsNewChildren(mod)) {
        const addTab = document.createElement("button");
        addTab.className = "tab tab-add";
        addTab.type = "button";
        addTab.textContent = "+";
        addTab.title = "add " + rolesAcceptedBy(mod).join(" / ");
        addTab.setAttribute("aria-label", addTab.title);
        addTab.addEventListener("click", () => {
            // THIS card's own footer: a plain querySelector would match the first .card-footer in the
            // subtree, which belongs to a nested child's card (Effects would then offer the Layer's
            // effects instead of another layer). Scope to direct children of this card.
            const card = childrenEl.parentElement;
            const footer = [...card.children].find(el => el.classList.contains("card-footer"));
            // Anchored to the TAB, which is what the user clicked. The footer's own add button
            // is display:none at this depth (renderModuleTree hides it), and a hidden
            // element's rect is all zeros, which pinned the modal to the top of the window.
            if (footer) openTypePicker(mod, addTab);
        });
        strip.appendChild(addTab);
    }
    childrenEl.appendChild(strip);

    const panel = document.createElement("div");
    panel.className = "tab-panel";
    panel.setAttribute("role", "tabpanel");
    childrenEl.appendChild(panel);
    const child = mod.children.find(c => c.name === active);
    if (child) renderModuleTree(child, panel, depth + 1);
}

// ---- MoonBase update flow ----
// On a MoonBase device the app cannot flash itself: one app slot, and it is running from it.
// Instead it reboots into the MoonBase factory image, which installs into the app slot and reboots
// back: same IP throughout (same MAC, same DHCP lease). The whole cycle runs behind one
// full-screen overlay, shown BEFORE the reboot so there is no dead gap. GET /moonbase is the
// identity probe: MoonBase answers it with its live install status; the app 404s it: so "404 again
// after an install ran" means the new firmware is up.
//
// The marker is the `image` control, which the module publishes only where there are two images to
// choose between. It was the `moonbase` control until that one was folded into the generic set as
// `version`, which left this testing for a name nothing emits: the card silently lost its tab
// strip AND its way into MoonBase, on a device that has one.
function deviceHasMoonBase(mod) {
    return (mod.controls || []).some(c => c.name === "image");
}

// Ask before installing a MoonBase, in the app's own chrome rather than the browser's.
//
// A native confirm() is the one box in this UI that cannot be styled, and it looked it: a white
// system panel over a dark card. This is the same native <dialog> the file editor uses, so Esc and
// the backdrop close it for free. Resolves true to install, false to abandon.
function askMoonBaseInstall() {
    return new Promise((resolve) => {
        const dlg = document.createElement("dialog");
        dlg.className = "mb-ask";
        const h = document.createElement("h3");
        h.textContent = "Install a new MoonBase?";
        const p1 = document.createElement("p");
        // What is actually at stake, rather than a bare "are you sure": the device has no recovery
        // image for the few seconds this takes.
        p1.textContent = "MoonBase is the recovery image. While it is being written the device "
                       + "has no recovery image, so do not power it off until this finishes.";
        const p2 = document.createElement("p");
        p2.className = "mb-ask-calm";
        p2.textContent = "The device checks the image first and refuses anything that is not a "
                       + "MoonBase image for this board, so a wrong file costs nothing.";
        const row = document.createElement("div");
        row.className = "mb-ask-row";
        const cancel = document.createElement("button");
        cancel.className = "fm-tool";
        cancel.textContent = "Cancel";
        const go = document.createElement("button");
        go.className = "fm-tool mb-ask-go";
        go.textContent = "Install";

        const close = (value) => { dlg.close(); dlg.remove(); resolve(value); };
        cancel.addEventListener("click", () => close(false));
        go.addEventListener("click", () => close(true));
        // Esc and the backdrop both reach here, so a dismissed dialog never leaves the promise
        // pending and never installs anything.
        dlg.addEventListener("close", () => { dlg.remove(); resolve(false); }, { once: true });

        row.append(cancel, go);
        dlg.append(h, p1, p2, row);
        document.body.appendChild(dlg);
        dlg.showModal();
        go.focus();
    });
}

// Byte counts out of a device status line, or null when it carries none.
//
// ONE PARSER for one wire shape. Every image writer reports progress as "<phase>: N of M bytes",
// and the phase word differs by writer (`flashing`, `writing MoonBase`, MoonBase's own
// `downloading`), so matching the numbers rather than the phrase is what lets one reader serve
// them all. Two hand-written regexes had already drifted apart on whether `bytes` was required.
//
// An ERROR line can carry the same shape ("upload ended early (12 of 34 bytes)"), so it is
// rejected here rather than left to the caller's check order.
function installProgress(text) {
    if (!text || text.startsWith("error")) return null;
    const m = /(\d+) of (\d+) bytes/.exec(text);
    if (!m) return null;
    const read = parseInt(m[1], 10), total = parseInt(m[2], 10);
    return total > 0 ? { read, total } : null;
}

// Raise the overlay over an install the DEVICE is performing, and follow it to the end.
//
// Every in-place install (a URL or a release, either image) answers 202 and then works on its own
// task, so the only way to know how far it has got is to ask. The device puts its byte counts in
// its status line, which is where both the text and the bar come from: one watcher, whichever
// image is being written.
//
// TWO ENDINGS, because the two installs end differently and getting this wrong is what made an
// overlay hang on a board where the install had actually succeeded:
//   MoonBase   leaves the app running, so the status falls back to rest and the card is redrawn.
//   the app    reboots into the image just written, so the device goes SILENT and comes back on
//              the same address. Silence after work is therefore success, not a stall.
//
// Polls the device rather than the cached `state`: across a reboot the WebSocket is down and the
// cache is frozen at the last frame before it went, which reads as an install that stopped.
//
// Not to be confused with moonbaseUpdateFlow, which carries an install across TWO reboots (into
// MoonBase and back) and has its own retry and handoff handling.
function watchInstall(kind) {
    const ui = showUpdateOverlay();
    ui.status(`Installing ${kind.label.toLowerCase()}\u2026`);
    const deadline = Date.now() + 300000;
    // Only treat an ending as SUCCESS once work was actually seen: the first poll can land before
    // the install task has written anything, and rest or silence then means "not started yet".
    let sawWork = false;
    let silent = 0;

    // The device's own Firmware status, or null when it is not answering.
    const probe = async () => {
        const ctl = new AbortController();
        const timer = setTimeout(() => ctl.abort(), 2500);
        try {
            const r = await fetch("/api/modules/Firmware", { cache: "no-store", signal: ctl.signal });
            if (!r.ok) return null;
            const mod = await r.json();
            return (mod && mod.status) || "";
        } catch (_) {
            return null;
        } finally {
            clearTimeout(timer);
        }
    };

    const tick = async () => {
        if (Date.now() > deadline) { ui.fail("Install timed out."); return; }
        const text = await probe();

        if (text === null) {
            // Not answering. During a rebooting install that is the reboot itself.
            silent++;
            if (kind.reboots && sawWork) {
                ui.status("Rebooting\u2026");
                if (silent > 90) { ui.fail(MOONBASE_SILENT_MSG); return; }
            } else if (silent >= 4) {
                ui.status("The device is not answering\u2026");
            }
        } else {
            if (text.startsWith("error")) { ui.fail(text); return; }
            const p = installProgress(text);
            if (p) {
                ui.status(`Installing ${fmSize(p.total)}\u2026`);
                ui.progress(p.read, p.total);
                sawWork = true;
            } else if (text && text !== "idle") {
                // A WORKING PHASE counts as work even with no byte counts. The device reports
                // counts only when it knows the content length, and a chunked response (any
                // proxy, or a server that does not send Content-Length) leaves it with none: the
                // status then walks checking -> erasing -> writing -> idle without ever matching,
                // so an install that SUCCEEDED sat behind the overlay until the five-minute
                // timeout. The bar stays indeterminate there, which is honest: the total is
                // genuinely unknown.
                ui.status(`Installing ${kind.label.toLowerCase()}\u2026`);
                sawWork = true;
            }
            // ANSWERING AGAIN AFTER SILENCE is the rebooting install's success: the image it
            // wrote is the one now running.
            if (kind.reboots && sawWork && silent > 0) {
                ui.status(`${kind.label} installed`);
                await fwSleep(1200);
                location.reload();
                return;
            }
            // Back to rest with no error is the non-rebooting install's success.
            if (!kind.reboots && sawWork && (text === "" || text === "idle")) {
                ui.status(`${kind.label} installed`);
                await fwSleep(1200);
                location.reload();
                return;
            }
            silent = 0;
        }
        await fwSleep(1000);
        tick();
    };
    tick();
}

// POST a file and paint a REAL progress bar while it uploads.
//
// fetch() cannot report upload progress at all (its streaming request bodies are not available
// here), so this is the one place an XMLHttpRequest earns its keep: xhr.upload.onprogress is the
// only way to know how far a browser-pushed install has got. The device-fetched installs report
// through the byte counts in their status line instead, so every install has a bar.
function uploadWithProgress(route, file, ui) {
    return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", route);
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.upload.addEventListener("progress", (e) => {
            if (e.lengthComputable) ui.progress(e.loaded, e.total);
        });
        xhr.addEventListener("load", () => {
            // The device answers before it reboots, so a 2xx is the real success signal. Anything
            // else carries the device's own reason in the JSON body.
            if (xhr.status >= 200 && xhr.status < 300) { resolve(); return; }
            let msg = `HTTP ${xhr.status}`;
            try { msg = JSON.parse(xhr.responseText).error || msg; } catch (_) {}
            reject(new Error(msg));
        });
        xhr.addEventListener("error", () => reject(new Error("the connection dropped")));
        xhr.addEventListener("abort", () => reject(new Error("canceled")));
        xhr.send(file);
    });
}

function showUpdateOverlay() {
    const ov = document.createElement("div");
    ov.className = "fw-overlay";
    const box = document.createElement("div");
    box.className = "fw-overlay-box";
    const h = document.createElement("h2");
    h.textContent = "Updating firmware\u2026";
    const msg = document.createElement("p");
    msg.className = "fw-overlay-msg";
    const bar = document.createElement("progress");   // no value = indeterminate sweep
    const dismiss = document.createElement("button");
    dismiss.textContent = "close";
    dismiss.style.display = "none";
    dismiss.addEventListener("click", () => ov.remove());
    const cancel = document.createElement("button");
    cancel.textContent = "cancel install";
    let uploadCtl = null;   // the in-flight file upload's AbortController, set by the flow
    cancel.addEventListener("click", () => {
        // A URL install hears POST /cancel; a file upload cancels by dropping its connection
        // (MoonBase's single-connection server is busy receiving it), so abort the fetch.
        if (uploadCtl) uploadCtl.abort();
        fetch("/api/firmware/cancel", { method: "POST" }).catch(() => {});
    });
    box.append(h, msg, bar, cancel, dismiss);
    ov.appendChild(box);
    document.body.appendChild(ov);
    return {
        status(text) { msg.textContent = text; },
        progress(read, total) { if (total > 0) { bar.max = total; bar.value = read; } },
        fail(text) { msg.textContent = text; bar.remove(); cancel.remove(); dismiss.style.display = ""; },
        setUpload(ctl) { uploadCtl = ctl; },
    };
}

const fwSleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Probe GET /moonbase once: "up" (200 + status text), "app" (404: the application is answering),
// or "silent" (no answer: the device is mid-reboot). Never throws.
async function probeMoonBase() {
    // Bounded: against a powered-off device an untimed fetch hangs for up to a minute on the
    // TCP connect, which starves the poll loop of the very silence it is trying to measure.
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), 2500);
    try {
        const r = await fetch("/moonbase", { cache: "no-store", signal: ctl.signal });
        if (r.ok) return { state: "up", text: await r.text() };
        return { state: "app" };
    } catch (_) {
        return { state: "silent" };
    } finally {
        clearTimeout(timer);
    }
}

// When this app page finds MoonBase answering underneath it (a cached or suspended tab over
// a device that switched images), say so and load MoonBase's real page: the cache-busting
// query makes the browser fetch it instead of re-serving this one. True when the handoff ran.
async function moonbaseHandoff() {
    if ((await probeMoonBase()).state !== "up") return false;
    document.body.innerHTML = "";
    const note = document.createElement("p");
    note.style.cssText = "font:16px system-ui;margin:3rem auto;max-width:30rem;text-align:center";
    note.textContent = "This device is in MoonBase (maintenance mode) \u2014 opening its page\u2026";
    document.body.appendChild(note);
    setTimeout(() => location.replace("/?" + Date.now()), 1200);
    return true;
}

// The one-click cycle. opts is {url} (device installs it unattended off the NVS-staged URL) or
// {file} (the browser holds the image and pushes it to MoonBase once MoonBase answers).
const MOONBASE_SILENT_MSG = "MoonBase did not answer. If its WiFi fell back, join the " +
                            "MoonBase access point and open http://4.3.2.1";

// Poll until MoonBase serves (a 200 on the probe). False on deadline: the device never came up,
// or came up unreachable (AP fallback).
async function waitForMoonBase(deadline) {
    while (Date.now() < deadline) {
        await fwSleep(2000);
        if ((await probeMoonBase()).state === "up") return true;
    }
    return false;
}

async function moonbaseUpdateFlow(opts) {
    const ui = showUpdateOverlay();   // opens before the reboot: no dead gap
    try {
        ui.status("Restarting into MoonBase\u2026");
        // The kickoff response can be cut off by the reboot; a dead socket here is not a failure.
        let res = null;
        try {
            res = opts.url
                ? await fetch("/api/firmware/url", {
                      method: "POST", headers: { "Content-Type": "application/json" },
                      body: JSON.stringify({ url: opts.url }) })
                : await fetch("/api/firmware/moonbase", { method: "POST" });
        } catch (_) {}
        if (res && !res.ok && res.status !== 202) throw new Error(await errorMessage(res));

        if (opts.file) {
            // The upload needs MoonBase serving before the browser can push the image.
            if (!(await waitForMoonBase(Date.now() + 120000))) throw new Error(MOONBASE_SILENT_MSG);
            ui.status(`Installing ${fmSize(opts.file.size)}\u2026`);
            const uploadCtl = new AbortController();
            ui.setUpload(uploadCtl);
            try {
                const r = await fetch("/api/firmware/upload", {
                    method: "POST", headers: { "Content-Type": "application/octet-stream" },
                    body: opts.file, signal: uploadCtl.signal });
                if (!r.ok) throw new Error(await r.text());
            } catch (err) {
                if (err instanceof Error && err.name === "AbortError") {
                    // The user's cancel, not a failure: the dropped connection aborts the
                    // write on the device, which stays waiting in MoonBase.
                    ui.fail("Install canceled \u2014 the device is waiting in MoonBase.");
                    return;
                }
                if (err instanceof Error && err.message.startsWith("error")) throw err;
                // A dead socket mid-upload (power cut, WiFi drop) is not a verdict: the watch
                // loop below sees where the device lands and retries the upload from there.
            } finally {
                ui.setUpload(null);
            }
        }

        // Watch until the app answers again. On the unattended URL path MoonBase installs BEFORE
        // it ever serves, so a successful cycle is silence followed by the new app's 404 on the
        // probe; MoonBase answering (200) is the upload path's install window, and on the URL
        // path its failure parking spot (the probe body is its status). The dying OLD app also
        // 404s the probe for a moment, so "app" only counts as done once the device has been
        // seen away, or after a grace period long enough that the kickoff reboot must have run.
        const watchStart = Date.now();
        let deadline = watchStart + 300000;
        let sawAway = false;
        let silentStreak = 0;
        let retries = 0;
        while (Date.now() < deadline) {
            await fwSleep(2000);
            const probe = await probeMoonBase();
            if (probe.state !== "silent") silentStreak = 0;
            if (probe.state === "silent") {
                sawAway = true;
                // A reboot is a few silent probes; many in a row is a device that lost power or
                // the network. Say so instead of freezing on the last byte count.
                if (++silentStreak >= 4) {
                    ui.status("The device is not answering \u2014 waiting for it to come back\u2026");
                }
            } else if (probe.state === "up" && probe.text === "idle" && sawAway) {
                // MoonBase is up with NOTHING running after the cycle already started: the
                // install was interrupted (a power cut lands exactly here: blank otadata boots
                // MoonBase, and the staged URL was already consumed). The browser still holds
                // the payload, so the one click survives the cut: hand it over again.
                if (retries >= 2) throw new Error("the install keeps getting interrupted");
                retries++;
                deadline += 180000;   // a retry restarts the ~40 s install; extend the watch
                ui.status("The install was interrupted \u2014 retrying\u2026");
                try {
                    if (opts.url) {
                        await fetch("/api/firmware/url", { method: "POST", body: opts.url });
                    } else {
                        await fetch("/api/firmware/upload", {
                            method: "POST", headers: { "Content-Type": "application/octet-stream" },
                            body: opts.file });
                    }
                    // The response itself is not consumed: MoonBase reboots on success and the
                    // probes below see the outcome either way.
                } catch (_) {}
            } else if (probe.state === "up") {
                sawAway = true;   // MoonBase answering means the old app is gone
                if (probe.text === "canceled") {
                    ui.fail("Install canceled \u2014 the device is waiting in MoonBase.");
                    return;
                }
                if (probe.text.startsWith("error")) throw new Error(probe.text);
                // The unattended install reports "downloading: N of M bytes"; render it the
                // way the file path reads, with a real bar.
                const dl = installProgress(probe.text);
                if (dl) {
                    ui.status(`Installing ${fmSize(dl.total || dl.read)}\u2026`);
                    ui.progress(dl.read, dl.total);
                } else {
                    // "idle" is MoonBase's quiescent state; mid-cycle it means the install
                    // has not started yet, which deserves better words than "Idle".
                    ui.status(!probe.text || probe.text === "idle"
                              ? "Preparing the install\u2026" : probe.text);
                }
            } else if (probe.state === "app" && (sawAway || Date.now() - watchStart > 20000)) {
                ui.status("Done \u2014 reloading\u2026");
                await fwSleep(1000);
                location.reload();
                return;
            }
        }
        throw new Error("timed out waiting for the new firmware");
    } catch (err) {
        ui.fail("Update failed: " + err.message);
    }
}

// Which surface bank a control belongs to, or null for an ordinary row. A surface strip is
// inline-flex, so consecutive strips pack onto one line and a wide card would run the switches
// straight into the encoders; a break closes a bank where the kind changes. BOTH render paths use
// this (createCard builds a card from scratch, syncVisibleControls rebuilds it live when a hidden
// flag flips), because a render rule that lives in only one of them is right until the other runs.
function surfaceBank(ctrl) {
    return ctrl.switchRow ? "switch" : ctrl.encoder ? "encoder" : ctrl.fader ? "fader" : null;
}

function surfaceBreak() {
    const br = document.createElement("div");
    br.className = "surface-break";
    return br;
}

/// The live editors on each module's script, by module name.
///
/// A card's status row and its script editor are built independently, so neither can reach the
/// other directly; this is the seam between them. A SET per module, because a module can have two
/// editors open on the same file at once (the card's pane and the modal it expands into) and both
/// have to mark the failing line.
const mlEditors = new Map();

/// Register an editor under a module, and return its own removal.
///
/// Handing back the unregister rather than exposing a remove(name, ed) keeps the two halves from
/// drifting: the modal in particular learns its module from whoever opened it, and cannot then
/// unregister under a different name.
function mlEditorAdd(name, ed) {
    if (!name) return () => {};
    if (!mlEditors.has(name)) mlEditors.set(name, new Set());
    mlEditors.get(name).add(ed);
    return () => {
        const set = mlEditors.get(name);
        if (!set) return;
        set.delete(ed);
        if (!set.size) mlEditors.delete(name);
    };
}

/// The editors on a module that are still on the page.
///
/// The sweep runs from setStatusText, which a card only calls when it HAS a status: a module that
/// never reports one leaves its detached editors in the map. They are inert (no timers, no DOM
/// work: painting is driven by the editor's own events), so this is housekeeping rather than a
/// leak, and the entries go when the module does.
///
/// renderCards rebuilds every card by clearing its host, which orphans an inline editor without
/// ever calling dispose: registering on create with no counterpart leaked one editor per re-render,
/// and each leaked one kept re-running the highlighter on detached DOM every time a status arrived.
/// Testing the DOM is what makes that impossible to get wrong, since it asks the only question that
/// matters (is this editor still showing?) rather than trusting every teardown path to report.
function mlLiveEditors(name) {
    const set = mlEditors.get(name);
    if (!set) return [];
    for (const ed of set) if (!ed.isMounted()) set.delete(ed);
    if (!set.size) { mlEditors.delete(name); return []; }
    return [...set];
}

/// Write a module's status, and mark the line it names in every editor showing that script.
///
/// Both render paths (createCard and updateModuleControls) come here, because a rule that lives in
/// only one of them is a rule that applies half the time. A compile failure arrives as
/// "message @<offset>"; an EDITOR turns that into a line and column, since only it holds the text
/// the offset counts into. Each returns the same rewritten text, so which one supplies it does not
/// matter; a module with no editor open shows the raw status unchanged.
function setStatusText(valEl, mod) {
    // Every editor on this module marks the line; each returns the same rewritten text, since they
    // hold the same file. Keeping the first answer rather than the last says that plainly: the
    // string does not depend on which editor supplied it.
    // Every editor on this module marks the line and returns the SAME rewritten text, since they
    // hold the same file: so mark them all, and take any one answer.
    let text = mod.status;
    for (const ed of mlLiveEditors(mod.name)) text = ed.markError(mod.status);
    // setText, not a bare assignment: this runs on every state push, and rewriting an unchanged
    // node throws away a selection the user may be holding on it.
    setText(valEl, text);
}

function createCard(mod, depth) {
    const card = document.createElement("div");
    card.className = "card";
    card.dataset.module = mod.name;
    card.dataset.depth = String(depth);

    // -- Title row: [enabled?] [name] [stats] [actions] --
    const title = document.createElement("div");
    title.className = "card-title";

    // The enabled toggle is built here but appended later: it joins the
    // right-hand action cluster (next to ✎ × ?) rather than sitting at the start
    // of the row, for visual grouping with the other per-card controls.
    // Rendered as a <button> styled as a 26×26 rounded box (matching .card-btn);
    // showing ✓ when on, blank when off. Stores its checked state in
    // data-checked so updateValues can sync from WS pushes. A native <input>
    // would not match the other buttons' frame and corner radius.
    // Only where it DOES something. A module that keeps running regardless says so
    // (`respectsEnabled`), and on Network, System or Firmware the switch was a control that
    // changed nothing: the scheduler ticks them whatever the flag says, so the device can be
    // reached to re-enable whatever else was turned off.
    // Built only where it does something; the name, tags and stats below are every card's.
    let enabled = null;
    if (mod.respectsEnabled !== false) {
        enabled = document.createElement("button");
        enabled.type = "button";
        enabled.className = "module-enabled";
        enabled.dataset.mid = mod.name;
        enabled.dataset.key = "enabled";
        enabled.setAttribute("aria-pressed", "true");
        enabled.title = "Enable / disable";
        const setEnabledUi = (on) => {
            enabled.dataset.checked = on ? "true" : "false";
            enabled.textContent = "⏻";
            enabled.classList.toggle("module-enabled--off", !on);
            enabled.setAttribute("aria-pressed", on ? "true" : "false");
            card.classList.toggle("card--disabled", !on);
            // Grey this module's TAB in the same click, alongside its card: so the tab title dims INSTANTLY
            // instead of waiting ~1s for the server's full-state round-trip. (updateTabDot still syncs it on the
            // patch path, idempotently, so this just makes the on/off button the immediate driver.) The tab
            // lives in the parent's strip, found by the same data-tab-mid updateTabDot uses.
            const tabEl = queryByName(`.tab[data-tab-mid="${cssEscape(mod.name)}"]`, "data-tab-mid", mod.name);
            if (tabEl) tabEl.classList.toggle("tab--disabled", !on);
        };
        setEnabledUi(mod.enabled === undefined ? true : !!mod.enabled);
        enabled.addEventListener("click", () => {
            const next = enabled.dataset.checked !== "true";
            setEnabledUi(next);
            // Stamp dragTs so a WS state push older than this click can't revert
            // the toggle before the server has acknowledged. updateValues reads
            // dragTs[mod.name + ":enabled"] on line ~952 and suppresses stale
            // patches within the 1s cooldown.
            dragTs[mod.name + ":enabled"] = Date.now();
            sendControl(mod.name, "enabled", next);
        });
    }

    const name = document.createElement("span");
    name.className = "card-name";
    name.textContent = mod.name;
    title.appendChild(name);

    // Emoji tags (role + curated) shown after the name: same set used by the
    // type picker's chip filter, so visual identity is consistent across views.
    const emojiList = emojiListForMod(mod);
    if (emojiList.length) {
        const emojiEl = document.createElement("span");
        emojiEl.className = "card-name-emoji";
        renderEmojiTags(emojiEl, emojiList);
        title.appendChild(emojiEl);
    }

    // Flex spacer so the name stays left and everything else groups on the right.
    const spacer = document.createElement("span");
    spacer.className = "card-spacer";
    title.appendChild(spacer);

    // Built from expert up: in user mode the line is empty and there is nothing to hold.
    //
    // HOLD TO PEEK, the password field's gesture. Time is what the line says, because a tick time
    // is the honest per-module figure: fps inverts it, so a module ticking in 22 us reads as 45K
    // fps and invites being read as a frame rate it is not. The rate is one press away for anyone
    // who wants it, and it goes back on release rather than leaving every card in a mode.
    if (uiMode() >= 1) {
        const stats = document.createElement("span");
        stats.className = "card-stats";
        stats.dataset.mid = mod.name;
        stats.dataset.key = "stats";
        stats.title = formatStatsTitle(mod);
        stats.textContent = formatStats(mod);
        const peekAll = (mode) => {
            timingMode = mode;
            document.querySelectorAll(".card-stats[data-mid]").forEach(el => {
                const m = findModule(el.dataset.mid);
                if (m) { el.textContent = formatStats(m); el.title = formatStatsTitle(m); }
            });
        };
        const show = () => peekAll("fps");
        const hide = () => peekAll("ms");
        stats.addEventListener("mousedown", show);
        stats.addEventListener("mouseup", hide);
        stats.addEventListener("mouseleave", hide);
        stats.addEventListener("touchstart", (e) => { e.preventDefault(); show(); });
        stats.addEventListener("touchend", hide);
        // A cancelled touch (a scroll takes over, a call arrives) fires no touchend, which left
        // the peek latched until the next gesture.
        stats.addEventListener("touchcancel", hide);
        title.appendChild(stats);
    }

    // Enable checkbox joins the right-hand action cluster, before ✎/×.
    if (enabled) title.appendChild(enabled);

    // Delete / replace buttons for user-managed children (any role a container
    // accepts, minus modules that opted out via userEditable=false). Top-level
    // modules are fixed in main.cpp; code-wired children declare userEditable
    // false or carry a role no container accepts. See isUserEditableChild.
    if (isUserEditableChild(mod, depth)) {
        const actions = createActionButtons(mod);
        title.appendChild(actions);
    }

    // The ASSIGN toggle, on the control surface's own card. Entering the mode is a property of the
    // whole desk, so it belongs here rather than on each of its 24 controls. Added directly to the
    // title row because the surface is a TOP-LEVEL module and never reaches createActionButtons,
    // which exists for cards a user can delete or replace.
    if (mod.type === "ControlModule") {
        const assignBtn = document.createElement("button");
        assignBtn.className = "card-btn assign-toggle" + (assignMode ? " active" : "");
        // 🔗 for "link a control to what it drives", ✓ while the mode is on. Single glyph like the
        // ✎ and × beside it, so the action row stays one row of 26px boxes.
        assignBtn.textContent = assignMode ? "✓" : "🔗";
        assignBtn.title = assignMode ? "Done assigning"
                                     : "Choose what each fader, knob and switch drives";
        assignBtn.addEventListener("click", (e) => {
            e.stopPropagation();
            setAssignMode(!assignMode);
        });
        title.appendChild(assignBtn);
    }

    // Help link → the module's spec page on the rendered docs site, far right of
    // the row. docPath comes from /api/types (relative to docs/moonmodules/, e.g.
    // "core/services.md#audio" or "light/effects.md#fire"); omitted if none.
    // The site is Material for MkDocs at moonmodules.org/projectMM/ (flat URLs, so
    // foo.md → foo.html; the MkDocs heading slugs match these #anchors), reached
    // via the same /MoonLight/ subpath the installer uses. Convert only the `.md`
    // extension that sits right before the optional `#anchor` (suffix-anchored),
    // so a docPath that ever contained ".md" mid-string wouldn't be mangled.
    const docPath = docPathForType(mod.type);
    if (docPath) {
        const help = document.createElement("a");
        help.className = "card-help";
        help.textContent = "?";
        help.title = "Open module documentation";
        help.target = "_blank";
        help.rel = "noopener";
        const htmlPath = docPath.replace(/\.md(#.*)?$/, ".html$1");
        help.href = "https://moonmodules.org/projectMM/moonmodules/" + htmlPath;
        title.appendChild(help);
    }

    // `api` → this ONE module's JSON in a new tab, for issue reports: a user pastes the state of
    // the card that misbehaves instead of the whole /api/state tree, and the report carries the
    // control values, the type and the live telemetry without anyone having to ask for them.
    // On EVERY card, unlike ✎/× (user-editable children only) and ? (types with a doc page):
    // the card most worth reporting is as likely to be a fixed top-level module as a child.
    const api = document.createElement("a");
    api.className = "card-api";
    // A glyph, not the word "api": this is a diagnostic aid, and the card's own content should
    // carry the visual weight. Braces read as JSON at a glance and match the ?/× glyph style.
    api.textContent = "{ }";
    api.title = "Open this module's JSON (for issue reports)";
    api.target = "_blank";
    api.rel = "noopener";
    // Relative, so it follows whatever host the UI is served from (device IP, mDNS name or a
    // desktop build on localhost) instead of hard-coding one.
    api.href = "/api/modules/" + encodeURIComponent(mod.name);
    title.appendChild(api);

    card.appendChild(title);

    // -- Controls --
    // Child-hosting modules deeper in the tree (Effects, Layer, Drivers, Layouts)
    // collapse their own controls so the children are the focus by default.
    // Modules that merely host a code-wired child (Network → Improv) keep their
    // controls expanded: the parent's settings are the main point, the code-wired
    // child is informational. Leaf modules render controls inline (no wrapper).
    // EXCEPTION: a top-level module (depth 0: the selected root, e.g. System,
    // Network, or a container like Services/Drivers) never collapses its own
    // controls, even when it accepts children. It's the card the user is looking
    // at, so its settings should be visible, not hidden behind a "controls" disclosure.
    // Use the SAME predicate the row loop renders by (controlRendersGenerically), not a bare
    // !c.hidden: else the disclosure could open for a module whose only "visible" controls render
    // elsewhere (e.g. FileManager's filesystem/lastSaved render inside its own panel, not generically).
    const hasVisibleControls = mod.controls && mod.controls.some(c => controlRendersGenerically(mod, c));
    const wrapInDetails = depth > 0 && acceptsNewChildren(mod) && hasVisibleControls;
    const controlsHost = wrapInDetails ? (() => {
        const d = document.createElement("details");
        d.className = "card-controls-collapse";
        // Restore the open/closed state from localStorage, so a full-state rebuild (or a page reload) keeps
        // the expander as the user left it instead of snapping shut. Persist it on toggle: same pattern as
        // the selected tab (LS_TABS). VIEW-only state; nothing to do with the module's backend state.
        d.open = expandedSet.has(mod.name);
        d.addEventListener("toggle", () => {
            if (d.open) expandedSet.add(mod.name); else expandedSet.delete(mod.name);
            saveExpanded();
        });
        const s = document.createElement("summary");
        s.textContent = "controls";
        d.appendChild(s);
        card.appendChild(d);
        return d;
    })() : card;
    if (mod.status) {
        const row = document.createElement("div");
        row.className = "control-row";
        row.dataset.statusMid = mod.name;
        const label = document.createElement("span");
        label.className = "control-label";
        label.textContent = "status";
        const val = document.createElement("span");
        val.className = "status-value";
        val.dataset.sev = mod.severity || "status";
        setStatusText(val, mod);
        row.appendChild(label);
        row.appendChild(val);
        controlsHost.appendChild(row);
    }

    if (mod.controls) {
        let bank = null;
        for (const ctrl of mod.controls) {
            if (!controlRendersGenerically(mod, ctrl)) continue;
            const kind = surfaceBank(ctrl);
            if (bank && kind !== bank) controlsHost.appendChild(surfaceBreak());
            bank = kind;
            const row = createControl(mod.name, mod.type, ctrl);
            if (row) controlsHost.appendChild(row);
        }
    }

    // File Manager: a modern expand/collapse folder tree + inline text editor. Browsing is UI-side
    // over /api/dir; a create/delete is a POST/DELETE /api/dir?path= call (the path rides the request,
    // no persisted control). Only the `show hidden` toggle renders as a raw control; the tree is the rest.
    if (mod.type === "FileManagerModule") {
        renderFileManager(mod, controlsHost);
    }

    // Contributing earns the answer back on the card that asked for consent, so the charts render
    // here rather than anywhere else. They are drawn empty until consent is on.
    if (mod.type === "MoonStatsModule") {
        renderMoonCloudStats(controlsHost, mod);
    }

    // The board itself, under the controls that post to it: a message box with no way to read the
    // replies is half a chat.
    if (mod.type === "MoonTalkModule") {
        renderMoonTalk(controlsHost, mod);
    }

    // FirmwareUpdate card hosts the shared install picker. Mount once per
    // card-build. The picker reads SystemModule.firmware (already in
    // /api/state) to filter to OTA-compatible releases. On install, the
    // device fetches the binary via /api/firmware/url: no browser CORS in
    // the data path. See docs/explanation/architecture/mooninstaller.md § Firmware vs board.
    if (mod.type === "FirmwareUpdateModule") {
        // A nudge where someone has just thought about versions, which is the moment the aggregate
        // is worth something to them. Shown only while Stats consent is off, so it disappears the
        // moment it is acted on. Both halves are named: reading it should not be how someone
        // discovers that consenting also shares.
        const statsConsent = allModules()
            .find(m => m.type === "MoonStatsModule")?.controls
            ?.find(c => c.name === "consent")?.value;
        if (statsConsent === false) {
            const nudge = document.createElement("div");
            nudge.className = "mooncloud-nudge";
            nudge.textContent = "Turn on MoonCloud stats: see what everyone else is running for ideas, so we know what to build next. ";
            // The reasons live in one place, and it is a page rather than a paragraph here: a nudge
            // long enough to make the case stops being a nudge.
            const why = document.createElement("a");
            why.href = "https://moonmodules.org/projectMM/mooncloud.html#why-you-might-like-this";
            why.target = "_blank";
            why.rel = "noopener";
            why.textContent = "Why you might like this";
            nudge.appendChild(why);
            controlsHost.appendChild(nudge);
        }

        // TWO IMAGES, ONE PANEL. A device installs the app it runs and MoonBase the recovery
        // image, described by the same four controls and installed the same three ways (a
        // release, a URL, a file). The device's `image` control says which, and everything here
        // reads it: one implementation, so the two cannot drift apart.
        //
        // The release row is install-picker's, the SAME component the web installer renders, so
        // the release list, the compatible-firmware filtering, the remembered selection and the
        // "2 weeks ago" labels have one home. The URL, File and image rows ride its
        // installRowExtras seam, which exists for exactly this.
        checkFirmwareUpdate(true);
        const ctrlValue = (name) => {
            const c = (mod.controls || []).find(x => x.name === name);
            return c ? c.value : null;
        };
        const hasMoonBase = deviceHasMoonBase(mod);
        const kind = (hasMoonBase && ctrlValue("image") === 1) ? "moonbase" : "app";
        const KINDS = {
            app: {
                label: "App",
                urlRoute: "/api/firmware/url",
                uploadRoute: "/api/firmware/upload",
                confirm: null,
                reboots: true,
            },
            moonbase: {
                label: "MoonBase",
                urlRoute: "/api/firmware/moonbase-update-url",
                uploadRoute: "/api/firmware/moonbase-update",
                confirm: askMoonBaseInstall,
                reboots: false,
            },
        };
        const k = KINDS[kind];

        // WHICH IMAGE, as a tab strip: the control chooses what every row is about, which reads
        // as a tab rather than as one setting among the settings it governs. The device holds the
        // selection (the control is marked hidden there), so this renders a fact rather than
        // owning one. Inserted after the card's title, ahead of the rows it governs.
        if (hasMoonBase) {
            const strip = document.createElement("div");
            strip.className = "tab-strip";
            strip.setAttribute("role", "tablist");
            ["app", "moonbase"].forEach((tabKind, i) => {
                const tab = document.createElement("button");
                tab.type = "button";
                tab.className = tabKind === kind ? "tab tab-active" : "tab";
                tab.setAttribute("role", "tab");
                tab.setAttribute("aria-selected", tabKind === kind ? "true" : "false");
                tab.textContent = KINDS[tabKind].label;
                tab.addEventListener("click", () => {
                    if (tabKind !== kind) sendControl(mod.name, "image", i);
                });
                strip.appendChild(tab);
            });
            const titleRow = controlsHost.querySelector(":scope > .card-title");
            controlsHost.insertBefore(strip, titleRow ? titleRow.nextSibling : controlsHost.firstChild);
        }

        const status = document.createElement("span");
        status.className = "fw-upload-status";
        const say = (t) => { status.textContent = t; };

        // Hand a URL to the device. Every install route answers 202 and works on its own task, so
        // the overlay reports progress rather than this response.
        const installUrl = async (url) => {
            if (kind === "app" && hasMoonBase) {
                // The app is written FROM MoonBase, so this reboots twice and moonbaseUpdateFlow
                // carries it across the handover.
                moonbaseUpdateFlow({ url });
                return;
            }
            const res = await fetch(k.urlRoute, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ url }),
            });
            if (!res.ok) throw new Error(await errorMessage(res));
            watchInstall(k);
        };

        // -- the rows that ride the picker's seam ---------------------------------------
        const extras = document.createElement("div");

        const urlRow = document.createElement("div");
        urlRow.className = "control-row fw-upload-row";
        const urlLabel = document.createElement("span");
        urlLabel.className = "control-label";
        urlLabel.textContent = "URL";
        const urlField = document.createElement("input");
        urlField.type = "url";
        urlField.className = "fw-url-field";
        urlField.placeholder = kind === "moonbase"
            ? "http://…/shared-moonbase-<chip>.bin" : "http://…/firmware.bin";
        const urlBtn = document.createElement("button");
        urlBtn.className = "fm-tool";
        urlBtn.textContent = "Install";
        urlBtn.addEventListener("click", async () => {
            const url = urlField.value.trim();
            if (!url) return;
            if (k.confirm && !await k.confirm()) return;
            urlBtn.disabled = true;
            say("installing…");
            try {
                await installUrl(url);
            } catch (err) {
                say("install failed: " + err.message);
            } finally {
                urlBtn.disabled = false;
            }
        });
        urlRow.append(urlLabel, urlField, urlBtn);
        extras.appendChild(urlRow);

        const fileRow = document.createElement("div");
        fileRow.className = "control-row fw-upload-row";
        const fileLabel = document.createElement("span");
        fileLabel.className = "control-label";
        fileLabel.textContent = "File";
        const upBtn = document.createElement("button");
        upBtn.className = "fm-tool fm-tool--icon";
        upBtn.textContent = "↥";
        upBtn.title = `Install ${k.label.toLowerCase()} from a file on your computer`;
        const upInput = document.createElement("input");
        upInput.type = "file";
        upInput.accept = ".bin,application/octet-stream";
        upInput.style.display = "none";
        upInput.addEventListener("change", async () => {
            const file = (upInput.files || [])[0];
            upInput.value = "";                   // re-picking the same file re-fires change
            if (!file) return;
            if (k.confirm && !await k.confirm()) return;
            if (kind === "app" && hasMoonBase) {
                // The app cannot flash itself: the browser pushes this file to MoonBase.
                moonbaseUpdateFlow({ file });
                return;
            }
            upBtn.disabled = true;
            // The overlay, like every other install. This is the one whose progress the DEVICE
            // cannot report (the request stays open for the transfer), so the bar is driven from
            // the upload itself, but a user should not have to know which install they picked to
            // know where the progress appears.
            const ui = showUpdateOverlay();
            ui.status(`Installing ${fmSize(file.size)}…`);
            try {
                await uploadWithProgress(k.uploadRoute, file, ui);
                if (k.reboots) {
                    ui.status("Rebooting…");
                    await fwSleep(4000);
                } else {
                    ui.status(`${k.label} installed`);
                    await fwSleep(1200);
                }
                location.reload();
            } catch (err) {
                ui.fail("Install failed: " + err.message);
            } finally {
                upBtn.disabled = false;
            }
        });
        upBtn.addEventListener("click", () => upInput.click());
        fileRow.append(fileLabel, upBtn, upInput);
        extras.appendChild(fileRow);
        extras.appendChild(status);

        // -- the shared picker ----------------------------------------------------------
        const mount = document.createElement("div");
        mount.className = "install-picker-host";
        controlsHost.appendChild(mount);
        installPicker.init({
            container: mount,
            // MoonBase ships ONE image per chip rather than one per variant, so on that tab the
            // picker offers the shared-moonbase assets and matches on the CHIP the device
            // reports. Same component, same release list, a different asset family.
            moonbaseOnly: kind === "moonbase",
            // Both tabs read the SAME control: the device sets `firmware` to the variant on the
            // app tab and to the chip on the MoonBase tab, precisely because that is what names
            // the release asset in each case. Re-deriving the chip from SystemModule here was a
            // second mechanism for a job the device had already done, with a JS transform
            // ("ESP32-S3" -> "esp32s3") that merely coincided with the build system's naming.
            ownFirmwareKey: moonbaseAssetKeyFrom(ctrlValue("firmware")),
            // Device already knows its deviceModel (SystemModule): picker is for releases +
            // firmware compatibility only. A board picker here would invite the user to
            // mis-narrow the firmware list.
            enableBoardPicker: false,
            installRowExtras: extras,
            // After the Install button, not before: these are other ways to install, so putting
            // them above would separate that button from the two dropdowns it acts on.
            extrasAfterInstall: true,
            onInstall: async (_firmware, _manifestUrl, binaryUrl, entry) => {
                // A desktop build has no OTA: it updates by downloading a new release and being
                // replaced by hand. So hand the browser the file rather than POSTing to a route
                // that would 501 here.
                if (entry && entry.isDesktop) {
                    const a = document.createElement("a");
                    a.href = binaryUrl;
                    a.rel = "noopener";
                    a.target = "_blank";
                    document.body.appendChild(a);
                    a.click();
                    a.remove();
                    return;
                }
                if (k.confirm && !await k.confirm()) return;
                await installUrl(binaryUrl);
            },
        });

        // BELOW everything, because it belongs to neither image: rebooting into MoonBase is not
        // an install, it is how a user reaches the recovery image to work there by hand.
        if (hasMoonBase) {
            const mbRow = document.createElement("div");
            mbRow.className = "control-row fw-upload-row";
            const mbBtn = document.createElement("button");
            mbBtn.className = "fm-tool";
            mbBtn.textContent = "Restart in MoonBase";
            mbBtn.title = "Reboot into MoonBase, the maintenance image; its own page has the way back";
            mbBtn.addEventListener("click", async () => {
                const ui = showUpdateOverlay();
                ui.status("Restarting into MoonBase…");
                try { await fetch("/api/firmware/moonbase", { method: "POST" }); } catch (_) {}
                if (await waitForMoonBase(Date.now() + 120000)) { location.reload(); return; }
                ui.fail(MOONBASE_SILENT_MSG);
            });
            mbRow.appendChild(mbBtn);
            controlsHost.appendChild(mbRow);
        }
    }

    // -- Children block + footer --
    // The .card-children wrapper lives inside this card so the parent's border
    // encloses its children; renderModuleTree recurses into it. The "+ add module"
    // footer only appears on parents that accept user-created children: a parent
    // hosting only code-wired children (e.g. Network → Improv) renders the
    // children block but no add button.
    let childrenEl = null;
    if (hasNestedChildren(mod)) {
        childrenEl = document.createElement("div");
        childrenEl.className = "card-children";
        childrenEl.dataset.depth = String(depth + 1);
        card.appendChild(childrenEl);

        if (acceptsNewChildren(mod)) {
            // -- Footer: + add module --
            const footer = document.createElement("div");
            footer.className = "card-footer";
            const addBtn = document.createElement("button");
            addBtn.className = "add-btn";
            addBtn.textContent = "+ add module";
            addBtn.addEventListener("click", () => {
                // The picker is a modal, so the button stays where it is: it used to be hidden
                // and restored through a MutationObserver because the picker was appended into
                // the footer and took the button's place.
                openTypePicker(mod, addBtn);
            });
            footer.appendChild(addBtn);
            card.appendChild(footer);
        }
    }

    // -- Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile) --
    // Same gate as the delete/replace buttons: a user-managed child is also
    // reorderable within its parent.
    if (isUserEditableChild(mod, depth)) {
        attachDragHandlers(card, mod);
    }

    return { card, childrenEl };
}

// Wire a button as a press-twice confirm: the first click arms it (adds the
// `armed` class, optional armed label/title), a second click runs `onConfirm`.
// Disarms after 3s or when the pointer leaves. Used by the delete and reboot
// buttons: no browser confirm() popup. `armedText` is optional (delete swaps
// × → ✓; reboot keeps its glyph). The pre-arm title is captured live so a
// title updated elsewhere (e.g. reboot's crashed-state text) restores correctly.
function armPressTwice(btn, onConfirm, opts = {}) {
    let armed = false;
    let disarmTimer = null;
    let savedText = "";
    let savedTitle = "";
    const disarm = () => {
        // Only restore label/title if we actually armed: otherwise a mouseleave before the first
        // click would write the initial empty savedText/savedTitle over the button, collapsing a
        // text-sized button (e.g. "🗑 delete") to an empty sliver.
        if (!armed) return;
        armed = false;
        btn.classList.remove("armed");
        if (opts.armedText !== undefined) btn.textContent = savedText;
        btn.title = savedTitle;
        if (disarmTimer) { clearTimeout(disarmTimer); disarmTimer = null; }
    };
    btn.addEventListener("click", () => {
        // Disarm before running the action so a stray second click can't fire
        // it twice (e.g. two /api/reboot requests).
        if (armed) { disarm(); onConfirm(); return; }
        armed = true;
        savedText = btn.textContent;
        savedTitle = btn.title;
        btn.classList.add("armed");
        if (opts.armedText !== undefined) btn.textContent = opts.armedText;
        if (opts.armedTitle !== undefined) btn.title = opts.armedTitle;
        disarmTimer = setTimeout(disarm, 3000);
    });
    btn.addEventListener("mouseleave", disarm);
}

// Compact byte formatter: "B" under 1 KB, "KB" otherwise (one decimal under 10 KB).
function fmtBytes(n) {
    if (n < 1024) return n + "B";
    const k = n / 1024;
    return (k < 10 ? k.toFixed(1) : Math.round(k)) + "KB";
}

// Stats line: timing (🕒, fps or µs/ms per the global toggle) + memory
// (🧠 static, plus "+ dynamic" only when the module allocated heap).
// Timing is omitted entirely when the module has no measured loop time.
//
// EXPERT AND UP. A tick time and a byte count diagnose the firmware rather than the show: they
// say which module is spending the frame, which is a question a reader in user mode is not
// asking and cannot act on. The same gate the controls use, so one mode decides the whole card.
function formatStats(mod) {
    if (uiMode() < 1) return "";
    const us = (mod.tickTimeUs !== undefined) ? mod.tickTimeUs : 0;
    let timing = "";
    if (us > 0) {
        if (timingMode === "fps") {
            const fps = Math.round(1_000_000 / us);
            timing = "🕒 " + (fps >= 1000 ? Math.round(fps / 1000) + "K fps" : fps + " fps");
        } else {
            timing = "🕒 " + (us < 1000 ? us + " µs" : (us / 1000).toFixed(2) + " ms");
        }
    }
    const stat = mod.classSize || 0;
    const dyn = mod.dynamicBytes || 0;
    const mem = "🧠 " + fmtBytes(stat) + (dyn > 0 ? " + " + fmtBytes(dyn) : "");
    // Status chip: emitted by the engine when a module has something to say.
    // Severity picks the emoji: ℹ️ neutral (Eth: 192.168.1.210), ⚠️ degraded
    // (buffer reduced), ❌ error (No network). Tooltip carries the full text.
    const sev = mod.severity || "status";
    const sevEmoji = sev === "error" ? "❌" : sev === "warning" ? "⚠️" : "ℹ️";
    const statusChip = mod.status ? "  " + sevEmoji : "";
    const head = timing ? timing + "   " + mem : mem;
    return head + statusChip;
}
function formatStatsTitle(mod) {
    // The tooltip carries the OTHER unit, so both readings are available without the gesture: the
    // card shows one, hovering says what it is in the other. A per-module fps is named as what it
    // is, a rate this module alone could sustain, because read as a frame rate it is a claim about
    // the whole pipeline made from one part of it.
    const us = (mod.tickTimeUs !== undefined) ? mod.tickTimeUs : 0;
    if (us > 0) {
        const fps = Math.round(1_000_000 / us);
        const other = timingMode === "fps"
            ? (us < 1000 ? us + " µs" : (us / 1000).toFixed(2) + " ms") + " per tick"
            : fps.toLocaleString() + " fps if this module ran alone";
        return other + (timingMode === "fps" ? "  ·  release for time" : "  ·  hold for fps");
    }
    return "Hold to view fps";
}

function createActionButtons(mod) {
    const wrap = document.createElement("span");
    wrap.className = "card-actions";

    // Reorder is drag-and-drop only (works on desktop and mobile). The whole
    // card body is the drag source; the controls region excludes itself via
    // the mousedown gate in attachDragHandlers. No up/down buttons, no
    // dedicated drag handle.

    const replaceBtn = document.createElement("button");
    replaceBtn.className = "card-btn";
    replaceBtn.textContent = "✎";
    replaceBtn.title = "Replace with another type";
    // The glyph is the button's accessible name unless one is given: "✎" tells a
    // screen reader (and a role-based locator) nothing about what the button does.
    replaceBtn.setAttribute("aria-label", "Replace with another type");
    replaceBtn.addEventListener("click", () => {
        // Anchored to the button: the picker is a modal that opens under whatever was clicked,
        // not inside the cramped 26px action-button row.
        openReplacePicker(mod, replaceBtn);
    });
    wrap.appendChild(replaceBtn);

    // Delete: press × once to arm, again to confirm: see armPressTwice.
    const delBtn = document.createElement("button");
    delBtn.className = "card-btn card-btn-del";
    delBtn.textContent = "×";
    delBtn.title = "Delete";
    delBtn.setAttribute("aria-label", "Delete");
    armPressTwice(delBtn, () => deleteModule(mod.name),
                  {armedText: "✓", armedTitle: "Click again to delete"});
    wrap.appendChild(delBtn);

    return wrap;
}

function findParent(childName) {
    function walk(node, modules) {
        for (const m of modules) {
            if (m === node) return null;  // shouldn't happen, defensive
            if (m.children && m.children.some(c => c.name === childName)) return m;
            if (m.children) {
                const p = walk(node, m.children);
                if (p) return p;
            }
        }
        return null;
    }
    return walk(null, state.modules);
}

// Whether this module renders any nested children at all (a "+ add module"
// button included if it also accepts new ones via the UI). True whenever the
// module has at least one child today OR is one of the light-pipeline
// containers that users can add to. This lets code-wired children (e.g.
// ImprovProvisioning under Network) render without making the parent UI-addable.
function hasNestedChildren(mod) {
    return (mod.children && mod.children.length > 0) || acceptsNewChildren(mod);
}

// Roles this parent accepts as user-added children, from the device's
// `acceptsChildRoles` (per-type in /api/types: e.g. Layer → "effect,modifier").
// Domain-neutral: the UI no longer hardcodes which module types are containers;
// the device declares it via MoonModule::acceptsChildRoles(). "" → [] (accepts
// none), which is also the default for modules whose type isn't loaded yet.
function rolesAcceptedBy(parentMod) {
    const t = availableTypes.find(t => t.name === parentMod.type);
    const csv = (t && t.acceptsChildRoles) ? t.acceptsChildRoles : "";
    return csv ? csv.split(",") : [];
}

// Whether the "+ add module" affordance applies: derived from acceptsChildRoles
// being non-empty, so there's a single source of truth (no separate list).
function acceptsNewChildren(mod) {
    return rolesAcceptedBy(mod).length > 0;
}

// The set of child roles ANY loaded type accepts: the union of every type's
// acceptsChildRoles. A module is "user-managed as a child" iff its role is in
// this set, which is how the UI decides to show delete/replace/drag without
// hardcoding role names. Code-wired children (ImprovProvisioning,
//: roles no container declares) correctly fall outside it.
function allAcceptedChildRoles() {
    const roles = new Set();
    for (const t of availableTypes) {
        const csv = t.acceptsChildRoles || "";
        if (csv) csv.split(",").forEach(r => roles.add(r));
    }
    return roles;
}

// Whether the UI shows delete / replace / drag for this module. True when it's
// a nested module (depth > 0) whose role is one some container accepts AND it
// hasn't opted out via the device's userEditable=false (e.g. PreviewDriver).
// Replaces the old hardcoded `role === "effect" || "modifier"` gate: now any
// add-accepted role (driver, layout, …) is editable, and the child itself can
// veto via userEditable.
//
// We test mod.role against the UNION of all containers' acceptsChildRoles, not
// against this module's specific parent. That's exact while the role→container
// mapping is 1:1 (effect→Layer, driver→Drivers, layout→Layouts, layer→Effects) -
// a child of an add-accepted role is always under the one container that
// accepts it. If a role ever becomes accepted by more than one container, this
// would need the parent threaded in to scope the check to the actual parent.
function isUserEditableChild(mod, depth) {
    return depth > 0
        && mod.userEditable !== false
        && allAcceptedChildRoles().has(mod.role);
}

// ---------------------------------------------------------------------------
// Control rendering (9 types per ui.md)
// ---------------------------------------------------------------------------

// Look up the factory default for a given module type's control. Returns undefined when
// the type isn't in /api/types yet or the control has no default (display/progress).
function defaultFor(moduleType, ctrlName, ctrl) {
    // A default carried ON the control wins: it is the only one that exists for a module whose
    // controls are declared by data rather than by its C++ type (a MoonLive script's
    // `uint8_t bpm = 60;`). /api/types probes a fresh instance, which for those has no script and
    // so declares no controls at all.
    if (ctrl && ctrl.default !== undefined) return ctrl.default;
    if (!moduleType) return undefined;
    const t = availableTypes.find(t => t.name === moduleType);
    if (!t || !t.defaults) return undefined;
    return t.defaults[ctrlName];
}

// The module type's spec-page path (relative to docs/moonmodules/), from /api/types.
// Returns "" when the type isn't loaded yet or declares no doc path.
function docPathForType(moduleType) {
    if (!moduleType) return "";
    const t = availableTypes.find(t => t.name === moduleType);
    return (t && t.docPath) ? t.docPath : "";
}

// Curated emoji string for a live module: its role emoji plus the type's
// `tags` from /api/types, deduplicated, in role-first order. "" if the type
// isn't loaded yet. Used on the card title and in the type picker.
//
// The INSTANCE's own tags win when it has them. A scripted module answers from the script it
// loaded, so two MoonLive effects running different scripts read differently while sharing one
// entry in /api/types: the audio one shows 🎶, the moving-head one 🎯. A compiled module sends
// nothing here and keeps its type's answer.
/// The same set as an array, for the callers that render one span per emoji.
function emojiListForMod(mod) {
    if (!mod) return [];
    const t = availableTypes.find(t => t.name === mod.type) || {role: mod.role, tags: ""};
    return emojiTagsFor(mod.tags ? {...t, tags: mod.tags} : t);
}

/// Fill `el` with one span per emoji, each carrying its own tooltip. A tooltip belongs to a single
/// character, so the emoji cannot be one joined string. Both the card header and the picker rows
/// render through here, which is what keeps a chip explained the same way wherever it appears.
function renderEmojiTags(el, list) {
    el.textContent = "";
    for (const ch of list) {
        const one = document.createElement("span");
        one.textContent = ch;
        const label = EMOJI_LABEL[ch];
        if (label) one.title = label;
        el.appendChild(one);
    }
}

// Whether a control appears in the generic control list: false for controls the module marked
// `hidden`. A module that renders a control in its own panel instead (e.g. the File Manager's
// `filesystem` usage bar + `lastSaved` readout, drawn by renderFileManager) sets the hidden flag on
// the C++ side, so the generic list skips it and the value never shows twice. Used by BOTH render
// paths (renderCards's initial build + updateModuleControls's WS live-patch) so they agree.
function controlRendersGenerically(mod, ctrl) {
    if (ctrl.hidden) return false;
    if ((ctrl.minMode | 0) > uiMode()) return false;   // above the reader's mode
    return true;
}

function createControl(moduleName, moduleType, ctrl) {
    const row = document.createElement("div");
    row.className = "control-row";
    // Expert-only controls (only reachable here when expert mode is on: see controlRendersGenerically)
    // get a distinct treatment so they read as a different tier: a left accent stripe + muted label.
    // One class per tier, because the glyph says WHICH mode reveals it: a reader in expert mode
    // meets both marks and the developer one tells them what they are still not seeing.
    // The glyph is CSS ::after content, which a screen reader does not reliably announce, so the
    // tier also rides a title: the same fact reaches a reader who cannot see the mark.
    if ((ctrl.minMode | 0) === 1) {
        row.classList.add("control-expert");
        row.title = "Expert mode control";
    } else if ((ctrl.minMode | 0) >= 2) {
        row.classList.add("control-developer");
        row.title = "Developer mode control";
    }
    // A switch-row control renders as a strip like an encoder or fader, so switch N sits in the
    // same column as encoder N and fader N: a surface reads down a channel, not across a list.
    if (ctrl.switchRow) row.classList.add("control-switch");
    row.dataset.key = ctrl.name;

    const label = document.createElement("label");
    label.className = "control-label";
    // The mode select itself carries both glyphs (via CSS ::after), so it reads as the switch
    // that governs the 🔧 controls: the toggle is never `advanced` (it must always be reachable), so key
    // it by name rather than the flag.
    if (moduleName === "System" && ctrl.name === "mode") label.classList.add("control-label--mode");
    label.textContent = displayName(ctrl.name);
    // The display strip carries no label: it spans the card and shows whatever was last touched, so
    // a label naming one control would be wrong the moment another moved.
    if (ctrl.displayStrip) row.classList.add("control-display-strip");
    else row.appendChild(label);

    const key = moduleName + ":" + ctrl.name;
    const def = defaultFor(moduleType, ctrl.name, ctrl);

    // numberField: a numeric control that opted out of the slider (server sets it for a value where each
    // integer is a discrete identity, not a magnitude: a PHY/I2C address, a channel). Render a plain
    // number input, same shape as the `pin` case, whatever the underlying numeric type. The WS-patch path
    // (updateModuleControls) reads the input by [data-mid][data-key] the same way, so no extra patch case.
    const isNumericType = ctrl.type === "uint8" || ctrl.type === "uint16" || ctrl.type === "int16" ||
                          ctrl.type === "int32";
    if (ctrl.numberField && isNumericType) {
        const nMin = Number(ctrl.min ?? 0);
        const nMax = Number(ctrl.max ?? 65535);
        const input = document.createElement("input");
        input.type = "number";
        input.min = nMin;
        input.max = nMax;
        input.value = ctrl.value ?? 0;
        input.dataset.mid = moduleName;
        input.dataset.key = ctrl.name;
        input.addEventListener("input", () => {
            dragTs[key] = Date.now();
            let v = parseInt(input.value, 10);
            if (Number.isNaN(v)) return;   // mid-edit empty field: send nothing until digits arrive
            v = Math.max(nMin, Math.min(nMax, v));
            if (String(v) !== input.value) input.value = v;   // display always matches what's sent
            debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
        });
        input.addEventListener("change", () => {   // blur/Enter with a still-empty field: snap to min + send
            if (Number.isNaN(parseInt(input.value, 10))) {
                dragTs[key] = Date.now();
                input.value = nMin;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, nMin));
            }
        });
        row.appendChild(input);
        appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
        return row;
    }

    switch (ctrl.type) {
        case "uint8": {
            const input = document.createElement("input");
            input.type = "range";
            // A fader is the same range input stood on end (CSS `writing-mode: vertical-lr`), so the
            // value, the range and the WS live-update path are untouched: only the orientation and
            // the row's layout change. Consecutive faders sit side by side as one bank.
            // An encoder is the same range input drawn as a KNOB: the input itself is hidden and an
            // SVG dial sits on top, so the value, the range, the WS update path and keyboard access
            // are the range input's: only the appearance changes. Dragging vertically turns it,
            // which is the gesture a real encoder takes.
            if (ctrl.encoder) {
                input.classList.add("encoder-input");
                row.classList.add("control-encoder");
                row.appendChild(buildKnob(input, ctrl));
                row.appendChild(buildSegReadout(input));
                attachTargetPopup(row, input, ctrl);
            }
            if (ctrl.fader) {
                input.classList.add("fader-input");
                row.classList.add("control-fader");
                row.appendChild(buildSegReadout(input));
                // Right-click a fader to see (and later choose) what it drives: the same
                // configure-on-the-control rule the pads follow.
                attachTargetPopup(row, input, ctrl);
            }
            input.min = ctrl.min ?? 0;
            input.max = ctrl.max ?? 255;
            input.value = ctrl.value ?? 0;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            // The knob and the readout were built above, before these three lines ran, so they drew
            // against an empty input (the source of the 880 readouts). Refresh now that the value and
            // the bounds exist.
            //
            // And again once the ROW is complete: redrawRangeDecorations walks the input's siblings,
            // and the decorations are appended after this point, so the call above finds a
            // half-built row and every readout keeps whatever it first drew. That is the "all the
            // numbers read 50 while the faders sit at different heights" the UI showed until a
            // manual refresh rebuilt the card at a moment when the order happened to work out.
            redrawRangeDecorations(input);
            queueMicrotask(() => redrawRangeDecorations(input));
            const numInput = document.createElement("input");
            numInput.type = "number";
            numInput.className = "control-value-input";
            numInput.min = input.min;
            numInput.max = input.max;
            numInput.value = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                numInput.value = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            numInput.addEventListener("input", () => {
                dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                const v = Math.max(Number(input.min), Math.min(Number(input.max), parseInt(numInput.value) || 0));
                input.value = v;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            row.appendChild(numInput);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                numInput.value = def;
            });
            break;
        }
        case "uint16": {
            // Bounded (server sent an explicit max below the type ceiling) →
            // slider, like uint8/int16. Unbounded (max == 65535, the default for
            // port/universe-style values with no natural range) → plain number.
            const uMin = Number(ctrl.min ?? 0);
            const uMax = Number(ctrl.max ?? 65535);
            if (uMax < 65535) {
                const input = document.createElement("input");
                input.type = "range";
                input.min = uMin;
                input.max = uMax;
                input.value = Math.max(uMin, Math.min(uMax, Number(ctrl.value ?? 0)));
                input.dataset.mid = moduleName;
                input.dataset.key = ctrl.name;
                const numInput = document.createElement("input");
                numInput.type = "number";
                numInput.className = "control-value-input";
                numInput.min = uMin;
                numInput.max = uMax;
                numInput.value = input.value;
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    numInput.value = input.value;
                    debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
                });
                numInput.addEventListener("input", () => {
                    dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                    const v = Math.max(uMin, Math.min(uMax, parseInt(numInput.value) || 0));
                    input.value = v;
                    debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, v));
                });
                row.appendChild(input);
                row.appendChild(numInput);
                appendResetButton(row, moduleName, ctrl, def, () => {
                    input.value = def; numInput.value = def;
                });
            } else {
                const input = document.createElement("input");
                input.type = "number";
                input.value = ctrl.value ?? 0;
                input.dataset.mid = moduleName;
                input.dataset.key = ctrl.name;
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    // Sanitise: empty/garbage → 0, clamp into the uint16 range so a
                    // NaN or out-of-range value never reaches the device.
                    let v = parseInt(input.value, 10);
                    if (Number.isNaN(v)) v = 0;
                    v = Math.max(0, Math.min(65535, v));
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
                });
                row.appendChild(input);
                appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            }
            break;
        }
        case "pin": {
            // A GPIO pin: plain number input, never a slider (a pin has no range to
            // drag). −1 = unused. ctrl.min/max are the valid-GPIO span used only to
            // clamp the typed value before sending.
            const pMin = Number(ctrl.min ?? -1);
            const pMax = Number(ctrl.max ?? 52);
            const input = document.createElement("input");
            input.type = "number";
            input.min = pMin;
            input.max = pMax;
            input.value = ctrl.value ?? -1;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                let v = parseInt(input.value, 10);
                if (Number.isNaN(v)) v = -1;
                v = Math.max(pMin, Math.min(pMax, v));
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            appendResetButton(row, moduleName, ctrl, def, () => { input.value = def; });
            break;
        }
        case "int32":
        case "int16": {
            // ctrl.min/ctrl.max are always present (server sends them). An int16 at the type's
            // own limit means "unbounded" and falls back to a +-percentage range, since a slider
            // spanning the full type is useless to drag.
            //
            // An int32 does NOT: its range is what the script declared, and narrowing an
            // unbounded one to -100..200 hid every value outside that window: a control
            // declared 0..1000 sitting at 900 rendered as a slider pinned to its top with the
            // real value unreachable. A bounded int32 keeps its bounds; an unbounded one spans
            // the type, which the number input beside the slider makes usable.
            const isI32 = ctrl.type === "int32";
            const lo = isI32 ? -2147483648 : -32768;
            const hi = isI32 ?  2147483647 :  32767;
            const rawMin = Number(ctrl.min ?? lo);
            const rawMax = Number(ctrl.max ?? hi);
            const min = (!isI32 && rawMin <= lo) ? -100 : rawMin;
            const max = (!isI32 && rawMax >= hi) ?  200 : rawMax;
            const raw = Number(ctrl.value ?? 0);
            const clamped = Math.max(min, Math.min(max, raw));
            const input = document.createElement("input");
            input.type = "range";
            input.min = min;
            input.max = max;
            input.value = clamped;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            const numInput = document.createElement("input");
            numInput.type = "number";
            numInput.className = "control-value-input";
            numInput.min = min;
            numInput.max = max;
            numInput.value = input.value;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                numInput.value = input.value;
                debounceSend(key, 150, () => sendControl(moduleName, ctrl.name, parseInt(input.value)));
            });
            numInput.addEventListener("input", () => {
                dragTs[key] = Date.now();   // stamp so a WS push can't revert what's being typed
                const v = Math.max(min, Math.min(max, parseInt(numInput.value) || 0));
                input.value = v;
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, v));
            });
            row.appendChild(input);
            row.appendChild(numInput);
            appendResetButton(row, moduleName, ctrl, def, () => {
                input.value = def;
                numInput.value = def;
            });
            break;
        }
        case "bool": {
            // Modern on/off switch: an <input type=checkbox> (kept for the
            // existing change/sync paths) wrapped in a <label> that draws a
            // pill-shaped track + sliding thumb via CSS. The checkbox itself
            // is visually hidden but stays the source of truth.
            const sw = document.createElement("label");
            sw.className = "switch";
            const input = document.createElement("input");
            input.type = "checkbox";
            input.checked = !!ctrl.value;
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("change", () => {
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, input.checked);
            });
            const track = document.createElement("span");
            track.className = "switch-track";
            sw.appendChild(input);
            sw.appendChild(track);
            row.appendChild(sw);
            appendResetButton(row, moduleName, ctrl, def, () => { input.checked = !!def; });
            // A surface SWITCH is assignable like a fader or a knob: it drives a control, and
            // switch1 driving Drivers.on is the same kind of binding fader1 has to brightness.
            if (ctrl.switchRow) attachTargetPopup(row, input, ctrl);
            break;
        }
        case "text": {
            const input = document.createElement("input");
            input.type = "text";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            // ctrl.readonly is a UI hint (the server still accepts writes: the
            // flag exists for values pushed by tooling like MoonDeck or the
            // web installer, where editing in the device UI is by-convention
            // disallowed). `readOnly` lets the user select/copy the text but
            // not modify it; `disabled` would also block copy.
            if (ctrl.readonly) {
                input.readOnly = true;
            } else {
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
                });
                // Enter presses the card's `send` button, where the module has one. Generic rather
                // than a MoonTalk rule: any module pairing a text field with a send button gets it,
                // and a module without one is unaffected.
                //
                // The write is FLUSHED first and awaited. Typing is debounced 500 ms, so Enter
                // straight after the last keystroke would otherwise press send while the device
                // still held the previous text, publishing the message a keystroke short.
                input.addEventListener("keydown", async (e) => {
                    if (e.key !== "Enter") return;
                    // allModules() walks children: Talk is a MoonCloud child, so a flat lookup on
                    // state.modules never finds it.
                    const mod = allModules().find(m => m.name === moduleName);
                    const send = (mod?.controls || [])
                        .find(c => c.type === "button" && c.name === "send");
                    if (!send) return;
                    e.preventDefault();
                    clearTimeout(dragTimers[key]);
                    await sendControl(moduleName, ctrl.name, input.value);
                    await sendControl(moduleName, "send", 1);
                });
            }
            row.appendChild(input);
            break;
        }
        case "textarea": {
            // Multi-line text (e.g. a script source). A resizable <textarea>; the
            // value syncs and debounces exactly like a "text" control.
            const input = document.createElement("textarea");
            input.className = "control-textarea";
            input.value = ctrl.value ?? "";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.rows = 2;            // default 2 lines; CSS height + resize grip control size
            input.spellcheck = false;
            // Restore a previously dragged height (view-state, see textareaSizes). A stored px height
            // overrides the 2-row default; an untouched textarea has no entry and keeps the default.
            const savedH = textareaSizes[key];
            if (typeof savedH === "number" && savedH > 0) input.style.height = savedH + "px";
            // Persist the height whenever the user drags the resize grip. A <textarea> has no native resize
            // event, so a ResizeObserver is the standard way to observe it; store the pixel height keyed by
            // "<module>:<control>". Use the observed height (not the inline style, which the initial observe
            // fire and a layout-driven change wouldn't set), track the last saved value, and write only on a
            // real change: so the initial fire and idle re-observations don't thrash localStorage. rAF-
            // coalesced so a drag saves once per frame at most.
            let taRaf = 0, taPrevH = Math.round(savedH > 0 ? savedH : 0);
            const taObserver = new ResizeObserver((entries) => {
                const h = Math.round(entries[0].contentRect.height);
                if (taRaf || h <= 0 || h === taPrevH) return;
                taRaf = requestAnimationFrame(() => { taRaf = 0; taPrevH = h; saveTextareaSize(key, h); });
            });
            taObserver.observe(input);
            if (ctrl.readonly) {
                input.readOnly = true;
            } else {
                input.addEventListener("input", () => {
                    dragTs[key] = Date.now();
                    debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
                });
            }
            row.appendChild(input);
            break;
        }
        case "filepath": return buildFilePathControl(row, label, key, moduleName, ctrl);
        case "password": {
            // ctrl.value arrives XOR-obfuscated + base64-encoded (see
            // HttpServerModule PASSWORD_XOR_KEY). Decode it so the input holds
            // the real stored password: masked by the password input, revealed
            // by hold-to-peek. The obfuscation is trivially reversible by design.
            const input = document.createElement("input");
            input.type = "password";
            input.value = decodePassword(ctrl.value);
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            // Hold-to-peek button: reveals the stored password.
            const peek = document.createElement("button");
            peek.className = "peek-btn";
            peek.type = "button";
            peek.textContent = "👁";
            peek.title = "Hold to reveal";
            const show = () => { input.type = "text"; };
            const hide = () => { input.type = "password"; };
            peek.addEventListener("mousedown", show);
            peek.addEventListener("mouseup", hide);
            peek.addEventListener("mouseleave", hide);
            peek.addEventListener("touchstart", (e) => { e.preventDefault(); show(); });
            peek.addEventListener("touchend", hide);
            peek.addEventListener("touchcancel", hide);   // a cancelled touch fires no touchend
            row.appendChild(peek);
            break;
        }
        case "select": {
            const sel = document.createElement("select");
            sel.dataset.mid = moduleName;
            sel.dataset.key = ctrl.name;
            (ctrl.options || []).forEach((opt, i) => {
                const o = document.createElement("option");
                o.value = i;
                o.textContent = opt;
                if (i === selectIndex(ctrl)) o.selected = true;
                sel.appendChild(o);
            });
            // Protect the dropdown while the user has it open. A native <select>
            // popup stays open for several frames (seconds, if deliberating) while
            // a continuously-refreshed module keeps pushing state over the WS; an
            // unguarded `sel.value = ctrl.value` patch during that window snaps the
            // menu back to the old option and visibly closes it: the user never
            // gets to pick. We mark the select "open" on pointerdown (fires BEFORE
            // the popup opens, unlike focus, which some browsers delay or skip) and
            // clear it on change/blur; updateModuleControls skips any select marked
            // open. pointerdown also stamps the dragTs cooldown as a belt-and-braces
            // fallback for the post-close frames.
            sel.dataset.open = "false";
            const markOpen = () => { sel.dataset.open = "true"; dragTs[key] = Date.now(); };
            sel.addEventListener("pointerdown", markOpen);
            sel.addEventListener("focus", markOpen);
            sel.addEventListener("blur", () => { sel.dataset.open = "false"; });
            sel.addEventListener("change", () => {
                sel.dataset.open = "false";
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, parseInt(sel.value));
                // No refetch/re-render here: blendMode/opacity-style selects don't
                // change the control SET, and a control that does (a hidden-flag
                // flip) is reconciled in place by syncVisibleControls on the next
                // WS push: so the card (and its expanded state) is preserved.
                // A full refetchState() rebuilt the DOM and collapsed the card.
            });
            row.appendChild(sel);
            appendResetButton(row, moduleName, ctrl, def, () => { sel.value = def; });
            break;
        }
        case "palette": return buildPaletteControl(row, key, def, moduleName, ctrl);
        case "display": {
            // Read-only string. Updates via WS push. A value that IS a url renders as a LINK:
            // the only reason to show one is to open or copy it. Device-relative ("/hls/...") on
            // purpose: the anchor resolves it against the page's own origin, so the link works
            // and copies correctly whatever address the device was reached on, and cannot go
            // stale when its IP changes.
            if (isUrlValue(ctrl.value)) {
                const a = document.createElement("a");
                a.className = "display control-url";
                a.dataset.mid = moduleName;
                a.dataset.key = ctrl.name;
                a.target = "_blank";
                a.rel = "noopener";
                setUrlDisplay(a, ctrl.value);
                row.appendChild(a);
                break;
            }
            // The surface's display strip: sixteen-segment cells across the full card width,
            // matching the seven-segment readouts under the encoders and faders.
            if (ctrl.displayStrip) {
                const strip = buildSeg16Strip(SEG16_CHARS, /*stretch=*/true);
                strip.dataset.mid = moduleName;
                strip.dataset.key = ctrl.name;
                strip._setText(ctrl.value ?? "");
                row.appendChild(strip);
                break;
            }
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.textContent = ctrl.value ?? "";
            row.appendChild(span);
            break;
        }
        case "display-int": {
            // Read-only signed int with a unit suffix (e.g. "-58 dBm").
            // ctrl.unit is the suffix the device chose at addReadOnlyInt time.
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.dataset.kind = "display-int";
            span.dataset.unit = ctrl.unit ?? "";
            span.textContent = fmtDisplayInt(ctrl);
            row.appendChild(span);
            break;
        }
        case "ipv4": {
            // Editable dotted-quad. Wire format is the same string the user
            // types: the device parses + validates server-side and rejects
            // malformed values with 400. Inline validation on the client is
            // a future enhancement; today an invalid value goes to the
            // server and the response surfaces the rejection.
            //
            // Same dragTs + debounceSend pattern as text / password so the
            // ipv4 input participates in stale-WS-push protection: while
            // the user is typing, dragTs[key] gets bumped, and an arriving
            // WS push within the cooldown window won't revert mid-edit
            // (see updateValues + the dragTs check ~line 1260).
            const input = document.createElement("input");
            input.type = "text";
            input.className = "ipv4-input";
            input.dataset.mid = moduleName;
            input.dataset.key = ctrl.name;
            input.value = ctrl.value ?? "";
            input.placeholder = "0.0.0.0";
            input.maxLength = 15;  // "255.255.255.255" = 15
            input.addEventListener("input", () => {
                dragTs[key] = Date.now();
                debounceSend(key, 500, () => sendControl(moduleName, ctrl.name, input.value));
            });
            row.appendChild(input);
            break;
        }
        case "time": {
            // Read-only seconds, rendered as "Xd Yh Zm Ws"
            const span = document.createElement("span");
            span.className = "display";
            span.dataset.mid = moduleName;
            span.dataset.key = ctrl.name;
            span.dataset.kind = "time";
            span.textContent = fmtTime(ctrl.value ?? 0);
            row.appendChild(span);
            break;
        }
        case "progress": {
            const bar = document.createElement("progress");
            bar.max = ctrl.total ?? 100;
            bar.value = ctrl.value ?? 0;
            bar.dataset.mid = moduleName;
            bar.dataset.key = ctrl.name;
            row.appendChild(bar);
            const lbl = document.createElement("span");
            lbl.className = "control-value";
            lbl.textContent = fmtProgressLabel(ctrl);
            lbl.dataset.mid = moduleName;
            lbl.dataset.key = ctrl.name + ".label";
            row.appendChild(lbl);
            break;
        }
        case "button": {
            const btn = document.createElement("button");
            btn.className = "action-btn";
            btn.textContent = ctrl.label || ctrl.name;
            // Flush any pending debounced text write first, then act. Typing is debounced 500 ms,
            // so a click inside that window sent the button while the device still held the text as
            // it stood one keystroke ago: the Enter path already does this, and the two must agree.
            btn.addEventListener("click", async () => {
                await flushPendingControlWrites(moduleName);
                sendControl(moduleName, ctrl.name, 1);
            });
            row.appendChild(btn);
            break;
        }
        case "list": {
            // A generic read-only list (ControlType::List). value = array of row
            // summary objects; ctrl.detail = parallel array of detail objects (same
            // order). Render one clickable row per summary; clicking toggles a detail
            // panel below it. Self rows (summary.self === true) get a marker. Fully
            // generic: the engine decides the fields; the UI just renders objects.
            row.classList.add("control-list-row");
            const rows = Array.isArray(ctrl.value) ? ctrl.value : [];
            const details = Array.isArray(ctrl.detail) ? ctrl.detail : [];
            const list = document.createElement("div");
            list.className = "list-control";
            list.dataset.mid = moduleName;
            list.dataset.key = ctrl.name;
            // When ctrl.editable is set, the list gains add/delete/reorder + inline field
            // editing (the editable-List primitive); otherwise it stays read-only (identical
            // to before). The flag flows through to both render paths via the opts object.
            if (ctrl.editable) list.dataset.editable = "true";
            buildListEntries(list, rows, details, new Set(),   // initial: nothing expanded
                {editable: ctrl.editable, pads: ctrl.pads, gridCols: ctrl.gridCols, gridRows: ctrl.gridRows,
                 moduleName: moduleName, ctrlName: ctrl.name, optionSets: ctrl.optionSets || {}});
            row.appendChild(list);
            break;
        }
        default:
            // Unknown control type: skip silently. New types may be added engine-side
            // without breaking the UI; they just don't render until handled here.
            return null;
    }

    return row;
}

// Rebuild a List control's entries inside `container` from `rows` (summary objects)
// and `details` (parallel detail objects). `openSet` is a Set<string> of summary
// texts whose detail panels start expanded: pass `new Set()` for a fresh build, or
// the currently-open set on a live re-render so an expanded row stays open. Shared by
// createControl (initial) and updateModuleControls (WS live patch) so the two can't drift.
//
// `opts` optionally carries {editable, moduleName, ctrlName}. When editable is true the
// list gains row add/delete/reorder + inline field editing (the editable-List primitive);
// each row must then carry a numeric `id`, and each detail an array `fields[]` of field
// descriptors. When editable is falsy the render is exactly the read-only path (unchanged).
// Stable key for the open-detail set. An editable row carries a durable `id` (survives
// field edits: renaming a preset or changing its channel count doesn't change the id), so
// key on it: otherwise changing a field that shows in the summary label would fold the
// open row shut (the label-text key no longer matches after the rebuild). A read-only row
// (e.g. a discovered device) has no `id`; fall back to the summary label, which is stable
// enough there (identity by display text, the previous behavior, unchanged for devices).
function listRowKey(item, labelText) {
    return (item && item.id != null) ? "#" + item.id : labelText;
}

// A grid of uniform pads, one per row: the presentation a `pads` list asks for. Rows are unchanged
//: this is the same data the stacked list renders, laid out for triggering rather than reading.
// Uniform size on purpose: sizing each pad to its label (as MoonLight does) makes a ragged grid that
// is harder to scan and to hit; a fixed pad with a truncated label is what a MIDI deck looks like.
// Long-press as the touch equivalent of a right-click: a touch device has no second button, so the
// configure gesture has to come from somewhere. 500ms, cancelled by movement so it never fires
// during a drag.
function attachLongPress(el, fn) {
    let timer = 0;
    const cancel = () => { if (timer) { clearTimeout(timer); timer = 0; } };
    el.addEventListener("touchstart", () => {
        cancel();
        timer = setTimeout(() => { timer = 0; fn(); }, 500);
    }, {passive: true});
    el.addEventListener("touchmove", cancel, {passive: true});
    el.addEventListener("touchend", cancel);
    el.addEventListener("touchcancel", cancel);
}

// A small popup anchored to a surface control. Floating rather than appended to the anchor: a panel
// inside the grid would reflow the pads underneath it, and a surface must not move while you are
// working on it. Dismisses on click-away, Escape, or opening another.
function openSurfacePopup(anchorEl, title, build) {
    document.querySelectorAll(".surface-popup").forEach(p => p.remove());
    const pop = document.createElement("div");
    pop.className = "surface-popup";

    const head = document.createElement("div");
    head.className = "surface-popup-title";
    head.textContent = title;
    pop.appendChild(head);

    const body = document.createElement("div");
    body.className = "surface-popup-body";
    pop.appendChild(body);
    // One teardown for every exit path (click-away, Escape, or a button calling close()): removing
    // the popup without detaching the document listeners leaked a pair per open.
    let away = null, esc = null;
    const close = () => {
        pop.remove();
        if (away) document.removeEventListener("mousedown", away);
        if (esc) document.removeEventListener("keydown", esc);
    };
    build(body, close);

    document.body.appendChild(pop);
    // Anchor below-right of the control, then pull back inside the viewport if that would overflow.
    const r = anchorEl.getBoundingClientRect();
    pop.style.left = `${Math.min(r.left, window.innerWidth - pop.offsetWidth - 8)}px`;
    pop.style.top = `${Math.min(r.bottom + 4, window.innerHeight - pop.offsetHeight - 8)}px`;

    // Defer so the click that opened it does not immediately close it.
    setTimeout(() => {
        away = (e) => { if (!pop.contains(e.target)) close(); };
        esc = (e) => { if (e.key === "Escape") close(); };
        document.addEventListener("mousedown", away);
        document.addEventListener("keydown", esc);
    }, 0);
    return pop;
}

// The edit form behind a right-click on a pad: rename, choose what it captures, save over it, or
// delete. These are the SAME operations the card's bottom controls performed, moved onto the thing
// they act on: a form at the bottom of the card cannot say which pad it means.
// What a preset captures: exactly ONE of the four top-level subtrees. A radio group rather than four
// checkboxes, so "a look" and "a geometry" are the only things expressible: the combinations that
// used to be possible were the hard part to explain and the hard part to display.
function buildCaptureToggles(body, moduleName) {
    const mod = findModule(moduleName);
    const ctrl = mod && (mod.controls || []).find(c => c.name === "captures");
    if (!ctrl) return;
    const names = Array.isArray(ctrl.options) && ctrl.options.length
        ? ctrl.options : ["Layouts", "Effects", "Drivers", "Services"];
    const wrap = document.createElement("div");
    wrap.className = "surface-popup-captures";
    names.forEach((n, i) => {
        const lab = document.createElement("label");
        lab.className = "surface-popup-capture";
        const rb = document.createElement("input");
        rb.type = "radio";
        rb.name = `capture-${moduleName}`;
        rb.checked = Number(ctrl.value) === i;
        rb.addEventListener("change", () => { if (rb.checked) sendControl(moduleName, "captures", i); });
        lab.append(rb, document.createTextNode(n));
        wrap.appendChild(lab);
    });
    const cap = document.createElement("div");
    cap.className = "surface-popup-caption";
    cap.textContent = "captures";
    body.append(cap, wrap);
}

function openPadEditor(anchorEl, moduleName, ctrlName, item, slot) {
    const filled = item != null;
    openSurfacePopup(anchorEl, filled ? `pad: ${item.name}` : `pad ${slot + 1} (empty)`, (body, close) => {
        if (filled) {
            const nameRow = document.createElement("div");
            nameRow.className = "surface-popup-row";
            const nameLbl = document.createElement("span");
            nameLbl.textContent = "name";
            const nameIn = document.createElement("input");
            nameIn.type = "text";
            nameIn.className = "list-field-input";
            nameIn.value = item.name || "";
            nameIn.addEventListener("change", async () => {
                await listSetField(moduleName, ctrlName, item.id, "name", nameIn.value);
                refetchState();
            });
            nameRow.append(nameLbl, nameIn);
            body.appendChild(nameRow);

            // What this preset carries, read from the file rather than the form.
            if (item.captures) {
                const cap = document.createElement("div");
                cap.className = "surface-popup-row";
                cap.textContent = `captures ${item.captures}`;
                body.appendChild(cap);
            }
            // Re-saving over the pad: the toggles decide what the NEW contents carry.
            buildCaptureToggles(body, moduleName);
            const over = document.createElement("button");
            over.className = "surface-popup-primary";
            over.textContent = "save current state over it";
            over.addEventListener("click", async () => {
                await sendControl(moduleName, "name", item.name);
                await sendControl(moduleName, "slot", slot);
                await sendControl(moduleName, "save", 1);
                close();
                refetchState();
            });
            body.appendChild(over);

            const del = document.createElement("button");
            del.className = "surface-popup-danger";
            del.textContent = "delete preset";
            del.addEventListener("click", async () => {
                await listDeleteRow(moduleName, ctrlName, item.id);
                close();
                refetchState();
            });
            body.appendChild(del);
        } else {
            // An empty pad's action is to fill it: name it and save the current state here.
            const nameRow = document.createElement("div");
            nameRow.className = "surface-popup-row";
            const nameLbl = document.createElement("span");
            nameLbl.textContent = "name";
            const nameIn = document.createElement("input");
            nameIn.type = "text";
            nameIn.className = "list-field-input";
            nameIn.placeholder = "new preset";
            nameRow.append(nameLbl, nameIn);
            body.appendChild(nameRow);
            buildCaptureToggles(body, moduleName);

            const saveBtn = document.createElement("button");
            saveBtn.className = "surface-popup-primary";
            saveBtn.textContent = "save current state here";
            saveBtn.addEventListener("click", async () => {
                if (!nameIn.value.trim()) { nameIn.focus(); return; }
                // The module's own `name` + `slot` + `save` controls do the work; the popup is only
                // the form that fills them, so the save path stays the one the tests cover.
                await sendControl(moduleName, "name", nameIn.value.trim());
                await sendControl(moduleName, "slot", slot);
                await sendControl(moduleName, "save", 1);
                close();
                refetchState();
            });
            body.appendChild(saveBtn);
            setTimeout(() => nameIn.focus(), 0);
        }
    });
}

// A rotary knob driving a range input. The input stays in the DOM (hidden) so every other path -
// the WS live patch, defaults, keyboard: keeps working against it unchanged; this only draws the
// value and turns drags into value changes.
/// "What does this strip drive?": the same popup for an encoder and a fader, since the question and
/// the answer are identical for both. Right-click on a pointer, long-press on touch.
// Every control in the live tree that a surface control can drive, as {module, control} pairs.
//
// Generated from the state the device already sends, so the picker can only ever offer something
// that exists: a typed "Drivers.palette" is invisible when misspelled until the fader silently does
// nothing. Text, file paths, passwords and buttons are left out, because a fader cannot meaningfully
// move a filename and offering it produces a control that looks assigned and is not.
const ASSIGNABLE_TYPES = new Set(["uint8", "uint16", "int16", "int32", "bool", "select", "palette", "pin"]);
function assignableTargets() {
    const out = [];
    const walk = (mods) => {
        for (const m of mods || []) {
            for (const c of m.controls || []) {
                // Not the surface's OWN controls: a fader driving another fader is a loop, and the
                // hidden "...Target" strings are the assignments themselves.
                if (m.type === "ControlModule") continue;
                if (ASSIGNABLE_TYPES.has(c.type)) out.push({module: m.name, control: c.name});
            }
            walk(m.children);
        }
    };
    walk(state && state.modules);
    return out;
}

// ASSIGN MODE. While it is on, a tap on any surface control opens its target picker instead of
// moving it. One flag rather than per-control state, because the mode is a property of the whole
// surface: a desk is either being played or being wired.
let assignMode = false;
function setAssignMode(on) {
    assignMode = on;
    document.body.classList.toggle("assign-mode", on);
    document.querySelectorAll(".assign-toggle").forEach(b => {
        b.classList.toggle("active", on);
        // The same two glyphs the builder uses: a word here and an emoji there made the button
        // change shape as well as state.
        b.textContent = on ? "✓" : "🔗";
        b.title = on ? "Done assigning" : "Choose what each fader, knob and switch drives";
    });
    if (!on) document.querySelectorAll(".surface-popup").forEach(p => p.remove());
}

function attachTargetPopup(row, input, ctrl) {
    const show = () => openSurfacePopup(input, ctrl.name, (body, close) => {
        const pairs = assignableTargets();
        const [curMod, curCtl] = (ctrl.target || "").split(".");

        const mkRow = (label, el) => {
            const r = document.createElement("div");
            r.className = "surface-popup-row";
            const s = document.createElement("span");
            s.textContent = label;
            r.append(s, el);
            body.appendChild(r);
            return r;
        };

        const modSel = document.createElement("select");
        const ctlSel = document.createElement("select");
        const mods = [...new Set(pairs.map(p => p.module))];

        const fillControls = () => {
            ctlSel.replaceChildren();
            for (const p of pairs.filter(p => p.module === modSel.value)) {
                const o = document.createElement("option");
                o.value = p.control; o.textContent = p.control;
                if (p.control === curCtl) o.selected = true;
                ctlSel.appendChild(o);
            }
        };
        for (const m of mods) {
            const o = document.createElement("option");
            o.value = m; o.textContent = m;
            if (m === curMod) o.selected = true;
            modSel.appendChild(o);
        }
        fillControls();
        modSel.addEventListener("change", fillControls);

        mkRow("module", modSel);
        mkRow("control", ctlSel);

        const buttons = document.createElement("div");
        buttons.className = "surface-popup-row";
        const assign = document.createElement("button");
        assign.textContent = "assign";
        const applyTarget = (value) => {
            sendControl("Control", ctrl.name + "Target", value);
            // Reflect it NOW. `target` is metadata beside the control, not its value, so
            // sendControl's eager state update does not touch it and the card showed the old
            // binding until the next 1 Hz push: a full second of looking like nothing happened.
            const mod = allModules().find(m => m.name === "Control");
            const c = mod && mod.controls.find(x => x.name === ctrl.name);
            if (c) c.target = value;
            ctrl.target = value;
            renderCards();
            close();
        };
        assign.addEventListener("click", () => {
            if (!modSel.value || !ctlSel.value) return;
            applyTarget(`${modSel.value}.${ctlSel.value}`);
        });
        const clear = document.createElement("button");
        clear.textContent = "clear";
        // Unassigning has to be as easy as assigning, or a mistake is only fixable through the API.
        clear.addEventListener("click", () => applyTarget(""));
        buttons.append(assign, clear);
        body.appendChild(buttons);

        const now = document.createElement("div");
        now.className = "surface-popup-row";
        now.textContent = ctrl.target ? `now drives ${ctrl.target}` : "unassigned";
        body.appendChild(now);
    });
    // ONE mechanism: assign mode, then a plain tap. That is what Ableton, Bitwig, Reaper, TouchOSC
    // and Open Stage Control all do, and it is the only shape that works identically on a mouse and
    // a touch screen. The alternatives each fail somewhere: right-click has no touch equivalent,
    // long-press has no mouse equivalent, and double-click COLLIDES with the control itself (a
    // double-click on a fader also moves it, so the assignment and the value fight over one
    // gesture). A mode has no such conflict, because a tap means only one thing while it is on.
    row.addEventListener("click", (e) => {
        if (!assignMode) return;                  // off: a click is an ordinary control interaction
        e.preventDefault();
        e.stopPropagation();
        show();
    }, /*capture=*/true);
    // A fader is a range input, which changes on pointerdown before any click fires: captured and
    // swallowed here, or the tap that opens the picker also moves the fader it is opening.
    row.addEventListener("pointerdown", (e) => {
        if (!assignMode) return;
        e.preventDefault();
        e.stopPropagation();
    }, /*capture=*/true);
    // The same door for the keyboard. Without this, assign mode was reachable only by pointer: a
    // keyboard user landing on a fader and pressing Enter or Space moved the control instead of
    // opening its picker, so the assignment could not be made at all. Captured like the two above,
    // for the same reason: a range input acts on the key before any click would fire.
    row.addEventListener("keydown", (e) => {
        if (!assignMode) return;
        // EVERY key is swallowed while assigning, not just the two that open the picker: an arrow
        // key on a focused fader still moves it, so a user tabbing through the surface in assign
        // mode would change the values they are trying to re-target. In this mode the row is a
        // button, and a button does not respond to arrows.
        e.preventDefault();
        e.stopPropagation();
        if (e.key === "Enter" || e.key === " ") show();
    }, /*capture=*/true);
    row.title = ctrl.target ? `drives ${ctrl.target}` : "unassigned";

    // The LABEL says what it drives, so an assigned control is recognizable without opening
    // anything: "fad1" is a position, "brightness" is what a user is looking for. The CONTROL half
    // only, because the module is usually obvious from the control (a palette is Drivers') and a
    // surface column is 26px wide. CSS ellipsis caps what does not fit.
    const label = row.querySelector(".control-label");
    if (label && ctrl.target) {
        const dot = ctrl.target.indexOf(".");
        label.textContent = dot >= 0 ? ctrl.target.slice(dot + 1) : ctrl.target;
        label.title = `${displayName(ctrl.name)} drives ${ctrl.target}`;
    }
}

function buildKnob(input, ctrl) {
    // Read the bounds LIVE rather than snapshotting them: the caller assigns input.min/max/value
    // after this returns, so a snapshot here is NaN and the dial draws against nothing.
    const bounds = () => {
        const lo = Number(input.min), hi = Number(input.max);
        return [Number.isFinite(lo) ? lo : 0, Number.isFinite(hi) ? hi : 255];
    };
    const wrap = document.createElement("div");
    wrap.className = "knob";
    const NS = "http://www.w3.org/2000/svg";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 40 40");
    // The arc runs from 7 o'clock to 5 o'clock: the 270° sweep a real encoder cap shows, with the
    // gap at the bottom so the pointer never sits in an ambiguous place.
    const track = document.createElementNS(NS, "circle");
    track.setAttribute("cx", "20"); track.setAttribute("cy", "20"); track.setAttribute("r", "15");
    track.setAttribute("class", "knob-track");
    const fill = document.createElementNS(NS, "circle");
    fill.setAttribute("cx", "20"); fill.setAttribute("cy", "20"); fill.setAttribute("r", "15");
    fill.setAttribute("class", "knob-fill");
    const pointer = document.createElementNS(NS, "line");
    pointer.setAttribute("class", "knob-pointer");
    svg.append(track, fill, pointer);
    wrap.appendChild(svg);

    const CIRC = 2 * Math.PI * 15;
    const SWEEP = 0.75;                       // 270° of the circle
    // rotate() as an ATTRIBUTE, not CSS: a CSS transform on an SVG geometry element resolves against
    // a different origin and drew a quarter arc instead of the intended 270°.
    track.setAttribute("transform", "rotate(135 20 20)");
    fill.setAttribute("transform", "rotate(135 20 20)");
    // An ENDLESS encoder's track is a full circle: the knob turns forever, so a track with a gap at
    // the bottom draws end stops it does not have. Only the track changes; the fill and the pointer
    // still sweep the same 270° against the value, exactly as a fader's do.
    const endlessTrack = !!(ctrl && ctrl.encoder);
    track.setAttribute("stroke-dasharray",
                       endlessTrack ? `${CIRC} ${CIRC}` : `${CIRC * SWEEP} ${CIRC}`);
    const draw = () => {
        const [min, max] = bounds();
        const v = Number(input.value);
        const frac = max > min ? Math.min(1, Math.max(0, (v - min) / (max - min))) : 0;
        fill.style.strokeDasharray = `${CIRC * SWEEP * frac} ${CIRC}`;
        // 135° is the 7 o'clock start; sweep clockwise from there.
        const ang = (135 + frac * 270) * Math.PI / 180;
        pointer.setAttribute("x1", String(20 + 6 * Math.cos(ang)));
        pointer.setAttribute("y1", String(20 + 6 * Math.sin(ang)));
        pointer.setAttribute("x2", String(20 + 13 * Math.cos(ang)));
        pointer.setAttribute("y2", String(20 + 13 * Math.sin(ang)));
    };
    draw();
    // Redraw when the value changes from anywhere: a drag here, or a WS push patching the input.
    input.addEventListener("input", draw);
    input.addEventListener("change", draw);
    wrap._redraw = draw;

    // Vertical drag turns the knob: up increases. 150px of travel covers the full range, which is
    // fine for a coarse control and keeps the gesture inside a card.
    // The gesture needs to be discoverable: without a hint the knob looks like a picture, and the
    // range input underneath makes the browser offer a resize cursor that suggests the wrong action.
    wrap.title = "drag up/down or scroll to turn";
    wrap.addEventListener("pointerdown", (e) => {
        // In assign mode a tap picks a target, so the knob must not also turn: without this the
        // value moved under the finger on the way to opening the picker.
        if (assignMode) return;
        e.preventDefault();
        wrap.setPointerCapture(e.pointerId);
        wrap.classList.add("knob-turning");
        const startY = e.clientY, startV = Number(input.value);
        const [min, max] = bounds();
        const move = (ev) => {
            const delta = (startY - ev.clientY) / 150 * (max - min);
            const next = Math.round(Math.min(max, Math.max(min, startV + delta)));
            if (next === Number(input.value)) return;
            input.value = String(next);
            input.dispatchEvent(new Event("input", {bubbles: true}));
        };
        // Every termination path runs this once: pointerup, but also pointercancel and
        // lostpointercapture, which fire when the browser takes the gesture over (scroll, a system
        // gesture). Without them the move listener stayed attached and the knob kept turning.
        let ended = false;
        const up = (ev) => {
            if (ended) return;
            ended = true;
            wrap.classList.remove("knob-turning");
            if (ev && ev.pointerId != null && wrap.hasPointerCapture(ev.pointerId))
                wrap.releasePointerCapture(ev.pointerId);
            wrap.removeEventListener("pointermove", move);
            wrap.removeEventListener("pointerup", up);
            wrap.removeEventListener("pointercancel", up);
            wrap.removeEventListener("lostpointercapture", up);
            input.dispatchEvent(new Event("change", {bubbles: true}));
        };
        wrap.addEventListener("pointermove", move);
        wrap.addEventListener("pointerup", up);
        wrap.addEventListener("pointercancel", up);
        wrap.addEventListener("lostpointercapture", up);
    });
    // Scroll to turn: the gesture people try first on anything round, and the one that works without
    // knowing the drag exists. `passive:false` so the page does not scroll underneath it.
    wrap.addEventListener("wheel", (e) => {
        if (assignMode) return;
        e.preventDefault();
        const [min, max] = bounds();
        const step = Math.max(1, Math.round((max - min) / 100));
        const next = Math.round(Math.min(max, Math.max(min,
            Number(input.value) + (e.deltaY < 0 ? step : -step))));
        if (next === Number(input.value)) return;
        input.value = String(next);
        input.dispatchEvent(new Event("input", {bubbles: true}));
        input.dispatchEvent(new Event("change", {bubbles: true}));
    }, {passive: false});
    return wrap;
}

// A SIXTEEN-SEGMENT readout: the ONE segmented display in the UI, used both for the numeric
// readouts under the encoders and faders and for the surface's full-width display strip.
//
// It replaced a separate seven-segment renderer, which could only draw digits: two renderers meant
// two geometries to keep visually matched (they drifted, and matching them again took several
// passes), and it left the numeric readouts unable to ever show a word. Sixteen segments (the seven
// segment bars with top, bottom and middle split in two, plus two verticals and four diagonals) is
// what an alphanumeric LED module uses, and it renders M, W, N and K legibly where fourteen cannot.
//
// Segment names follow the usual convention:
//   a1 a2   top left / right      f  h  j  k  b   upper verticals + diagonals
//   g1 g2   middle left / right   e  n  m  l  c   lower verticals + diagonals
//   d1 d2   bottom left / right
// Space-separated so a segment name is a whole token: "a1" and "a" are different segments, and
// substring matching would light both.
const SEG16 = Object.fromEntries(Object.entries({
    // Plain, not barred: a sixteen-segment module usually bars its zero to tell it from O, but these
    // readouts are numeric and the strip shows words, so context settles it and a clean rectangle is
    // what the readouts have always shown.
    "0": "a1 a2 b c d1 d2 e f",       "1": "b c",
    "2": "a1 a2 b g1 g2 e d1 d2",    "3": "a1 a2 b c d1 d2 g1 g2",
    "4": "f b g1 g2 c",              "5": "a1 a2 f g1 g2 c d1 d2",
    "6": "a1 a2 f g1 g2 e c d1 d2",  "7": "a1 a2 b c",
    "8": "a1 a2 b c d1 d2 e f g1 g2","9": "a1 a2 b c d1 d2 f g1 g2",
    "A": "a1 a2 b c e f g1 g2",      "B": "a1 a2 b c d1 d2 g2 j m",
    "C": "a1 a2 f e d1 d2",          "D": "a1 a2 b c d1 d2 j m",
    "E": "a1 a2 f e d1 d2 g1 g2",    "F": "a1 a2 f e g1 g2",
    "G": "a1 a2 f e d1 d2 c g2",     "H": "b c e f g1 g2",
    "I": "a1 a2 d1 d2 j m",          "J": "b c d1 d2 e",
    "K": "e f g1 k l",               "L": "f e d1 d2",
    "M": "b c e f h k",              "N": "b c e f h l",
    "O": "a1 a2 b c d1 d2 e f",      "P": "a1 a2 b e f g1 g2",
    "Q": "a1 a2 b c d1 d2 e f l",    "R": "a1 a2 b e f g1 g2 l",
    "S": "a1 a2 f g1 g2 c d1 d2",    "T": "a1 a2 j m",
    "U": "b c d1 d2 e f",            "V": "e f k n",
    "W": "b c e f n l",              "X": "h k n l",
    "Y": "h k m",                    "Z": "a1 a2 k n d1 d2",
    "-": "g1 g2", "+": "g1 g2 j m", ".": "d1", ":": "j m", "/": "k n", " ": "",
}).map(([ch, segs]) => [ch, new Set(segs.split(" ").filter(Boolean))]));

// One character cell on the SEVEN-SEGMENT's own grid: 14 units wide, 20 tall, 2-unit strokes. The
// readouts render 14 units at 14px (three cells in 42px), so the strip drawn on the identical grid
// reads as the same instrument. The strip is then stretched horizontally to span the eight surface
// columns, which is a small widening of the cells and leaves the stroke height untouched.
function seg16Cell(NS, ox) {
    const bar = (x, y, w, h) => {
        const r = document.createElementNS(NS, "rect");
        r.setAttribute("x", String(x)); r.setAttribute("y", String(y));
        r.setAttribute("width", String(w)); r.setAttribute("height", String(h));
        r.setAttribute("rx", "0.75");
        return r;
    };
    // A diagonal is a 2-unit line with a round cap, matching the bars' weight and their rx rounding.
    const diag = (x1, y1, x2, y2) => {
        const l = document.createElementNS(NS, "line");
        l.setAttribute("x1", String(x1)); l.setAttribute("y1", String(y1));
        l.setAttribute("x2", String(x2)); l.setAttribute("y2", String(y2));
        l.setAttribute("stroke-width", "1.5"); l.setAttribute("stroke-linecap", "round");
        return l;
    };
    // The seven-segment's own coordinates and cell size, but a 1.5-unit stroke against its 2.
    //
    // DELIBERATELY thinner, not a mismatch: a sixteen-segment glyph lights up to sixteen segments in
    // the cell where a digit lights seven, and the two center verticals plus four diagonals crowd
    // the middle. At an identical stroke width the strip reads noticeably bolder than the numbers
    // below it, so matching the WEIGHT means drawing thinner than them.
    const S = 1.5;
    return {
        a1: bar(ox + 2, 1, 3, S),        a2: bar(ox + 6, 1, 3, S),
        d1: bar(ox + 2, 17.5, 3, S),     d2: bar(ox + 6, 17.5, 3, S),
        g1: bar(ox + 2, 9.25, 3, S),     g2: bar(ox + 6, 9.25, 3, S),
        f:  bar(ox + 0.25, 2, S, 7),     b:  bar(ox + 9.25, 2, S, 7),
        e:  bar(ox + 0.25, 11, S, 7),    c:  bar(ox + 9.25, 11, S, 7),
        j:  bar(ox + 4.75, 2, S, 7),     m:  bar(ox + 4.75, 11, S, 7),
        h:  diag(ox + 2.7, 3.5, ox + 4.3, 7.9),   k: diag(ox + 8.3, 3.5, ox + 6.7, 7.9),
        n:  diag(ox + 2.7, 16.9, ox + 4.3, 12.4), l: diag(ox + 8.3, 16.9, ox + 6.7, 12.4),
    };
}

const EMPTY_SEGS = new Set();

// Cells in the display strip: 28 at the seven-segment's 14-unit pitch, which is exactly the width
// of the eight surface columns at one unit per pixel. Long enough for any built-in palette name.
const SEG16_CHARS = 28;

// The numeric readout under an encoder or fader: three cells bound to a range input, right-aligned
// and blank-padded the way a three-digit module displays ("  7", " 42", "255"). Text-capable like
// every cell here, so a readout that wants to show a word later needs no new renderer.
function buildSegReadout(input) {
    const wrap = buildSeg16Strip(3);
    wrap.classList.add("seg-readout");
    const draw = () => wrap._setText(
        String(Math.round(Number(input.value))).padStart(3, " ").slice(-3));
    draw();
    input.addEventListener("input", draw);
    input.addEventListener("change", draw);
    // Same hook the knob exposes, so redrawRangeDecorations refreshes both without knowing what
    // either one is.
    wrap._redraw = draw;
    return wrap;
}

// The display strip: `chars` cells of sixteen segments, driven by a string rather than a range
// input. Returns the wrapper with a `_setText` hook the update path calls.
function buildSeg16Strip(chars, stretch = false) {
    const NS = "http://www.w3.org/2000/svg";
    const wrap = document.createElement("div");
    wrap.className = "seg16";
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", `0 0 ${chars * 14} 20`);
    // `stretch` fills the box in BOTH axes, which the display strip needs: it is set to the width of
    // the eight surface columns, and preserving the ratio of a 28-cell box would shrink it to fit
    // its 20px height and stop it reaching the eighth column. The three-cell readouts must NOT get
    // this: their box is already the natural 42x20, and stretching blew the glyphs up to a solid
    // bar.
    if (stretch) svg.setAttribute("preserveAspectRatio", "none");
    const cells = [];
    for (let i = 0; i < chars; i++) {
        const map = seg16Cell(NS, i * 14 + 1);
        for (const el of Object.values(map)) {
            el.setAttribute("class", "seg16-off");
            svg.appendChild(el);
        }
        cells.push(map);
    }
    wrap.appendChild(svg);
    wrap._setText = (text) => {
        // Uppercased and left-aligned: the map is uppercase-only, and a name reads from the left the
        // way a scribble strip does. An unmappable character shows as blank rather than as garbage.
        const s = String(text ?? "").toUpperCase();
        cells.forEach((map, i) => {
            const on = SEG16[s[i]] ?? EMPTY_SEGS;
            for (const [name, el] of Object.entries(map))
                el.setAttribute("class", on.has(name) ? "seg16-on" : "seg16-off");
        });
    };
    return wrap;
}

function buildListPads(container, rows, opts) {
    const moduleName = opts && opts.moduleName;
    const ctrlName = opts && opts.ctrlName;
    const editable = !!(opts && opts.editable);
    const cols = (opts && opts.gridCols) | 0;
    const gridRows = (opts && opts.gridRows) | 0;
    const fixed = cols > 0 && gridRows > 0;
    container.replaceChildren();
    const grid = document.createElement("div");
    grid.className = "list-pads" + (fixed ? " list-pads-fixed" : "");
    // Columns track --surface-col, the ONE width the knob strips and fader strips also use, so the
    // three banks line up as columns of a single surface. It is a clamp() against the card, so the
    // alignment survives the right pane being dragged narrower or wider.
    if (fixed) grid.style.gridTemplateColumns = `repeat(${cols}, var(--surface-col))`;
    container.appendChild(grid);
    if (!fixed && rows.length === 0) {
        const empty = document.createElement("div");
        empty.className = "list-empty";
        empty.textContent = "(none)";
        container.appendChild(empty);
        return;
    }
    // On a FIXED surface every cell is rendered, occupied or not: an empty cell is a position you
    // can drop a pad onto, which is what makes the grid a surface rather than a wrapped list.
    const cells = fixed ? new Array(cols * gridRows).fill(null) : rows.slice();
    if (fixed) for (const r of rows) {
        const s = (r && r.slot) | 0;
        if (s >= 0 && s < cells.length) cells[s] = r;
    }
    cells.forEach((item, i) => {
        if (fixed && item == null) {
            // An empty cell: a drop target and nothing else. It carries the slot index so a drop
            // knows where it landed.
            // A button, not a div: an empty cell is an ACTION (create a preset here), so it must be
            // reachable and activatable from the keyboard like every other pad.
            const hole = document.createElement("button");
            hole.type = "button";
            hole.className = "list-pad list-pad-empty";
            hole.title = `pad ${i + 1} (empty): click to save the current state here`;
            hole.addEventListener("click", () => openPadEditor(hole, moduleName, ctrlName, null, i));
            hole.addEventListener("dragover", (e) => {
                e.preventDefault();
                e.dataTransfer.dropEffect = "move";
                hole.classList.add("list-drop-target");
            });
            hole.addEventListener("dragleave", () => hole.classList.remove("list-drop-target"));
            hole.addEventListener("contextmenu", (e) => {
                e.preventDefault();
                openPadEditor(hole, moduleName, ctrlName, null, i);
            });
            attachLongPress(hole, () => openPadEditor(hole, moduleName, ctrlName, null, i));
            hole.addEventListener("drop", (e) => {
                e.preventDefault();
                hole.classList.remove("list-drop-target");
                const draggedId = parseInt(e.dataTransfer.getData("text/plain"));
                if (!Number.isNaN(draggedId)) listMoveRow(moduleName, ctrlName, draggedId, i);
            });
            grid.appendChild(hole);
            return;
        }
        const pad = document.createElement("button");
        pad.type = "button";
        pad.className = "list-pad" + (item && item.active ? " list-pad-active" : "");
        // A pad is tinted by the roles it carries, so what a preset covers reads as color as well as
        // emoji. When it is lit, the tint comes from the roles it still HOLDS (activeRoles): a mixed
        // preset whose layer was superseded but whose layout is still on stays lit in the layout hue.
        const padRoles = Array.isArray(item && item.roles) ? item.roles : [];
        const litRoles = Array.isArray(item && item.activeRoles) ? item.activeRoles : [];
        const hue = roleHue(litRoles.length ? litRoles : padRoles);
        if (hue) pad.style.setProperty("--pad-hue", hue);
        const label = String((item && item.name) ?? "");
        // Role emoji through the SAME table the module cards use, so 🥞 means the same thing on a pad
        // as on a card. A row that carries no roles simply gets no emoji.
        const roles = Array.isArray(item && item.roles) ? item.roles : [];
        const emoji = roles.map(r => ROLE_EMOJI[r] || "").join("");
        if (emoji) {
            const em = document.createElement("span");
            em.className = "list-pad-emoji";
            em.textContent = emoji;
            pad.appendChild(em);
        }
        const nameEl = document.createElement("span");
        nameEl.className = "list-pad-name";
        nameEl.textContent = label;
        pad.appendChild(nameEl);
        pad.title = emoji ? `${label} (${roles.join(", ")})` : label;   // the full name when truncated
        pad.addEventListener("click", async () => {
            if (item == null || item.id == null) return;
            pad.disabled = true;
            // An ACTION field: the arrival is the whole message, so the value is unused. (The tests
            // call setListRowField directly and pass the request BODY, which is why they read "{}".)
            await listSetField(moduleName, ctrlName, item.id, "activate", "");
            refetchState();
            pad.disabled = false;
        });
        // Right-click edits the pad rather than triggering it: the controls belong on the thing they
        // configure, not in a form at the bottom of the card that cannot say which pad it means.
        pad.addEventListener("contextmenu", (e) => {
            e.preventDefault();
            openPadEditor(pad, moduleName, ctrlName, item, (item && item.slot) | 0);
        });
        // Touch has no right button, so a long press opens the same editor. Cancelled by movement so
        // a drag to reorder is not mistaken for a press-and-hold.
        attachLongPress(pad, () => openPadEditor(pad, moduleName, ctrlName, item, (item && item.slot) | 0));
        // Drag to reorder, so a grid can be arranged to match a physical control surface. Same
        // mechanism the list rows use (listMoveRow by stable id), and the same reason the ids are
        // stable: a reorder must not change what a pad refers to.
        if (editable && item && item.id != null) {
            pad.draggable = true;
            pad.addEventListener("dragstart", (e) => {
                e.dataTransfer.effectAllowed = "move";
                e.dataTransfer.setData("text/plain", String(item.id));
                pad.classList.add("list-dragging");
            });
            pad.addEventListener("dragend", () => {
                pad.classList.remove("list-dragging");
                grid.querySelectorAll(".list-drop-target").forEach(el => el.classList.remove("list-drop-target"));
            });
            pad.addEventListener("dragover", (e) => {
                e.preventDefault();
                e.dataTransfer.dropEffect = "move";
                pad.classList.add("list-drop-target");
            });
            pad.addEventListener("dragleave", () => pad.classList.remove("list-drop-target"));
            pad.addEventListener("drop", (e) => {
                e.preventDefault();
                pad.classList.remove("list-drop-target");
                const draggedId = parseInt(e.dataTransfer.getData("text/plain"));
                if (!Number.isNaN(draggedId) && draggedId !== item.id) {
                    // `i` is the SLOT on a fixed grid (the cell), or the row index on a flowing one.
                    listMoveRow(moduleName, ctrlName, draggedId, i);
                }
            });
        }
        grid.appendChild(pad);
    });
}

function buildListEntries(container, rows, details, openSet, opts) {
    if (opts && opts.pads) { buildListPads(container, rows, opts); return; }
    const editable = !!(opts && opts.editable);
    const moduleName = opts && opts.moduleName;
    const ctrlName = opts && opts.ctrlName;
    const optionSets = (opts && opts.optionSets) || {};   // shared option arrays, keyed by name (optionsRef)
    container.replaceChildren();
    if (rows.length === 0 && !editable) {
        const empty = document.createElement("div");
        empty.className = "list-empty";
        empty.textContent = "(none)";
        container.appendChild(empty);
        return;
    }
    // Rows live in their own scroll box so a long list (e.g. the seeded fixture presets) caps its
    // height (~5 rows, see .list-scroll in the CSS) and scrolls, instead of eating the whole card.
    // The "+ Add" button stays OUTSIDE this box, pinned below the scroll area: the standard
    // scrollable-list-with-fixed-footer pattern.
    const scroll = document.createElement("div");
    scroll.className = "list-scroll";
    container.appendChild(scroll);
    rows.forEach((item, i) => {
        const entry = document.createElement("div");
        // Row classes: the `self` marker + a generic `severity` marker (rowSeverityClass): both are
        // field-name conventions the engine opts into, no per-module UI code.
        const sevClass = rowSeverityClass(item);
        entry.className = "list-entry" + (item && item.self ? " list-self" : "") +
                          (sevClass ? " " + sevClass : "");
        const summary = document.createElement("div");
        summary.className = "list-summary";
        summary.tabIndex = 0;
        summary.setAttribute("role", "button");
        // Freshness dot (always-visible age at a glance) when the row carries a `*Sec`
        // duration; colored by ageBucketClass. Generic: no device knowledge here.
        // The age fields (`ageSec`/`cached`) live in the DETAIL object, not the summary,
        // so read the detail for the dot (it also carries `self`); fall back to the
        // summary item when a list has no separate detail.
        const ageClass = rowAgeClass(details[i] ?? item);
        if (ageClass) {
            const dot = document.createElement("span");
            dot.className = "age-dot " + ageClass;
            dot.setAttribute("aria-hidden", "true");
            summary.appendChild(dot);
        }
        const label = document.createElement("span");
        label.className = "list-summary-label";
        label.textContent = listSummaryText(item);
        summary.appendChild(label);
        const detailPanel = document.createElement("div");
        detailPanel.className = "list-detail";
        // Keyed via listRowKey (stable `id` for editable rows, else the label text: see the
        // helper) so an expanded row stays expanded across a live re-render, even when the field
        // that changed is shown in the summary label (e.g. a preset's channel count).
        detailPanel.hidden = !openSet.has(listRowKey(item, label.textContent));
        summary.setAttribute("aria-expanded", String(!detailPanel.hidden));
        const locked = !!(item && item.locked);
        if (editable) {
            // Right-side row actions: delete (✕) then a `⠿` drag-to-reorder handle, in that order -
            // handles on the RIGHT, after delete, keep every row's controls aligned in one column
            // (the common data-grid layout). A `locked` (seeded) row isn't draggable and has no
            // delete: the server refuses both, so the UI shows neither, and locked rows have an
            // empty actions area that still aligns. The buttons stop click propagation so they don't
            // toggle the detail panel. entry carries the row id so a drop knows what moved where.
            entry.dataset.rowId = item.id;
            entry.dataset.rowIndex = i;
            const actions = document.createElement("span");
            actions.className = "list-actions";
            if (!locked) {
                const del = document.createElement("button");
                del.type = "button";
                del.className = "list-action-btn";
                del.textContent = "✕";
                del.title = "Delete";
                del.addEventListener("click", (e) => { e.stopPropagation(); listDeleteRow(moduleName, ctrlName, item.id); });
                actions.appendChild(del);

                const handle = document.createElement("span");
                handle.className = "list-drag-handle";
                handle.textContent = "⠿";                 // a braille-dots grab affordance (the ::: idiom)
                handle.title = "Drag to reorder";
                handle.setAttribute("aria-hidden", "true");
                handle.addEventListener("click", (e) => e.stopPropagation());
                // The entry is draggable, gated by the SAME mechanism the module-card drag uses
                // (attachDragHandlers): a capture-phase MOUSEDOWN handler sets `draggable` based on where
                // the grab landed. HTML5 drag initiates from mousedown, so the flag must be set there -
                // a pointerdown handler runs too late (the browser has already read draggable at
                // mousedown) and dragstart never fires. Here the row drags ONLY when the grab is on the
                // handle (a click elsewhere expands the row); the touchstart mirror arms mobile drag.
                const dragGate = (e) => { entry.draggable = !!e.target.closest(".list-drag-handle"); };
                entry.addEventListener("mousedown", dragGate, true);
                entry.addEventListener("touchstart", dragGate, {capture: true, passive: true});
                entry.addEventListener("dragstart", (e) => {
                    e.dataTransfer.effectAllowed = "move";
                    e.dataTransfer.setData("text/plain", String(item.id));
                    entry.classList.add("list-dragging");
                });
                entry.addEventListener("dragend", () => {
                    entry.draggable = false;
                    entry.classList.remove("list-dragging");
                    container.querySelectorAll(".list-drop-target").forEach(el => el.classList.remove("list-drop-target"));
                });
                actions.appendChild(handle);
            }
            summary.appendChild(actions);
            // Every editable row is a drop TARGET (drop a dragged row onto it to move there). A drop
            // onto a locked row is clamped server-side to the first custom slot, so it's harmless.
            entry.addEventListener("dragover", (e) => { e.preventDefault(); e.dataTransfer.dropEffect = "move"; entry.classList.add("list-drop-target"); });
            entry.addEventListener("dragleave", () => entry.classList.remove("list-drop-target"));
            entry.addEventListener("drop", (e) => {
                e.preventDefault();
                entry.classList.remove("list-drop-target");
                const draggedId = parseInt(e.dataTransfer.getData("text/plain"));
                if (!Number.isNaN(draggedId) && draggedId !== item.id) {
                    listMoveRow(moduleName, ctrlName, draggedId, i);   // move the dragged row to THIS row's index
                }
            });
            // Editable detail: render each field descriptor as an inline control, unless
            // the row is locked (then fall back to the read-only key/value render).
            if (locked) fillListDetail(detailPanel, details[i] ?? item);
            else fillEditableListDetail(detailPanel, details[i] ?? item, moduleName, ctrlName, item.id, optionSets);
        } else {
            fillListDetail(detailPanel, details[i] ?? item);
        }
        const toggle = () => {
            detailPanel.hidden = !detailPanel.hidden;
            summary.setAttribute("aria-expanded", String(!detailPanel.hidden));
        };
        summary.addEventListener("click", toggle);
        summary.addEventListener("keydown", (e) => {
            if (e.key === "Enter" || e.key === " ") { e.preventDefault(); toggle(); }
        });
        entry.append(summary, detailPanel);
        scroll.appendChild(entry);
    });
    if (editable) {
        // "+ Add" appends a new row (server assigns the id, then state refreshes).
        const addBtn = document.createElement("button");
        addBtn.type = "button";
        addBtn.className = "list-add-btn";
        addBtn.textContent = "+ Add";
        addBtn.addEventListener("click", () => listAddRow(moduleName, ctrlName));
        container.appendChild(addBtn);
    }
}

// Render an editable row's detail: each descriptor in `detail.fields[]` becomes an inline
// control (text→<input>, uint8→<input type=number>, select→<select>). On change we call
// listSetField(...) and let the normal refresh reflect the persisted state: no eager local
// local mutation. A field with `readonly:true` renders as a plain read-only value (same look
// as fillListDetail). Fields carry a dragTs cooldown so a WS state push mid-edit can't revert
// what's being typed/picked, mirroring the select/text guards in createControl.
function fillEditableListDetail(panel, detail, moduleName, ctrlName, id, optionSets) {
    panel.replaceChildren();
    optionSets = optionSets || {};
    const fields = detail && Array.isArray(detail.fields) ? detail.fields : [];
    for (const f of fields) {
        const r = document.createElement("div");
        r.className = "list-detail-row";
        const kEl = document.createElement("span");
        kEl.className = "list-detail-key";
        kEl.textContent = f.name;
        const vEl = document.createElement("span");
        vEl.className = "list-detail-val";
        // Per-field cooldown key: unique per module/control/row/field so edits don't collide.
        const dragKey = `list:${moduleName}:${ctrlName}:${id}:${f.name}`;
        if (f.readonly) {
            vEl.textContent = String(f.value ?? "");
            vEl.classList.add("list-detail-muted");
        } else if (f.type === "button") {
            // A row ACTION rather than a value: the click PATCHes the field like any edit, and the
            // source reads the arrival as "do this to this row" (ControlModule's preset `apply`).
            // Generic on purpose: a row button is a primitive the list has lacked, not a
            // preset-specific affordance.
            //
            // `refetch` is opt-IN because a full refetch rebuilds every card, which collapses the
            // expanded row the button lives in. That is right for an action that reshapes the tree
            // (applying a preset) and wrong for one that arms a mode the user is about to use: the
            // infrared learn button closed its own row and left nowhere to watch the result. Without
            // it the WS push reconciles the row in place, which is what a field edit already relies
            // on.
            const btn = document.createElement("button");
            btn.className = "list-field-btn";
            btn.textContent = f.label || f.name;
            btn.addEventListener("click", async () => {
                btn.disabled = true;
                await listSetField(moduleName, ctrlName, id, f.name, "");
                if (f.refetch) refetchState();
                btn.disabled = false;
            });
            vEl.appendChild(btn);
        } else if (f.type === "select") {
            const sel = document.createElement("select");
            sel.className = "list-field-input";
            sel.dataset.dragkey = dragKey;
            // Options come from the field's inline `options`, or (the common case for a repeated select
            // like the channel-role pickers) from the list's shared `optionSets` via `optionsRef`: so
            // the option array is sent once per list, not re-inlined in every row (see writeListOptionSets).
            const fieldOptions = f.options || (f.optionsRef ? optionSets[f.optionsRef] : null) || [];
            fieldOptions.forEach((opt, idx) => {
                const o = document.createElement("option");
                o.value = idx;
                o.textContent = opt;
                if (idx === f.value) o.selected = true;
                sel.appendChild(o);
            });
            const mark = () => { dragTs[dragKey] = Date.now(); };
            sel.addEventListener("pointerdown", mark);
            sel.addEventListener("focus", mark);
            sel.addEventListener("change", () => {
                dragTs[dragKey] = Date.now();
                listSetField(moduleName, ctrlName, id, f.name, parseInt(sel.value));
            });
            vEl.appendChild(sel);
        } else if (f.type === "uint8") {
            const inp = document.createElement("input");
            inp.type = "number";
            inp.className = "list-field-input";
            inp.dataset.dragkey = dragKey;
            if (f.min !== undefined) inp.min = f.min;
            if (f.max !== undefined) inp.max = f.max;
            inp.value = f.value ?? 0;
            inp.addEventListener("input", () => { dragTs[dragKey] = Date.now(); });
            inp.addEventListener("change", () => {
                dragTs[dragKey] = Date.now();
                // Guard against empty/invalid entry (parseInt → NaN) and clamp to the control's
                // range: the HTML min/max attributes don't enforce a hand-typed value, so a stray
                // "" or out-of-range number would otherwise reach the device as NaN / an overflow.
                let v = parseInt(inp.value, 10);
                if (!Number.isFinite(v)) v = f.min ?? 0;
                if (f.min !== undefined) v = Math.max(v, f.min);
                if (f.max !== undefined) v = Math.min(v, f.max);
                inp.value = v;   // reflect the clamped value back into the field
                listSetField(moduleName, ctrlName, id, f.name, v);
            });
            vEl.appendChild(inp);
        } else {   // "text" and any unknown type render as a text input
            const inp = document.createElement("input");
            inp.type = "text";
            inp.className = "list-field-input";
            inp.dataset.dragkey = dragKey;
            inp.value = f.value ?? "";
            inp.addEventListener("input", () => { dragTs[dragKey] = Date.now(); });
            inp.addEventListener("change", () => {
                dragTs[dragKey] = Date.now();
                listSetField(moduleName, ctrlName, id, f.name, inp.value);
            });
            vEl.appendChild(inp);
        }
        r.append(kEl, vEl);
        panel.appendChild(r);
    }
}

// Join a list row's scalar fields into a one-line summary (skips marker fields: `self`, `severity`
//: that render as row styling rather than text, and any nested objects). Generic: the engine names
// the fields.
function listSummaryText(item) {
    if (!item || typeof item !== "object") return String(item ?? "");
    return Object.entries(item)
        .filter(([k, v]) => k !== "self" && k !== "severity" && typeof v !== "object")
        .map(([, v]) => v)
        .join("  ·  ");
}

// Render a list row's detail object as read-only key/value rows. Scalars print as-is;
// an array of scalars (e.g. a device's `speaks:["http"]` or `via:["mdns","scan"]`)
// renders as small chips so multi-valued fields like the discovery source are visible
// at a glance. Nested objects are still skipped (no use case yet). Generic: the engine
// names the fields, so a new array field shows up with no UI change here.
function fillListDetail(panel, detail) {
    panel.replaceChildren();
    if (!detail || typeof detail !== "object") return;
    for (const [k, v] of Object.entries(detail)) {
        const isScalarArray = Array.isArray(v) && v.every(e => typeof e !== "object");
        if (typeof v === "object" && !isScalarArray) continue;
        // `cached` and `ageSec` both render as "last seen"; a MoonLight device emits
        // exactly one (mutually exclusive in DevicesModule), but skip ageSec when a
        // `cached` key is also present so any other source can't produce two conflicting
        // "last seen" rows. Match the cached branch's render condition, which fires on
        // key EXISTENCE (`k === "cached"`), not truthiness: so gate on the key being
        // present, not on its value. (Robust-to-any-input, generic.)
        if (k === "ageSec" && "cached" in detail) continue;
        const r = document.createElement("div");
        r.className = "list-detail-row";
        const kEl = document.createElement("span");
        // A `*Sec` field is a duration in seconds (e.g. a device's `ageSec`): show it
        // under a plainer label ("last seen") and as a relative time, not a bare count.
        // `cached` is the sibling: a restored device not yet re-seen live → "last seen:
        // cached" rather than a fake recent time.
        const isDuration = k.endsWith("Sec");
        kEl.className = "list-detail-key";
        kEl.textContent = (k === "ageSec" || k === "cached") ? "last seen" : k;
        const vEl = document.createElement("span");
        vEl.className = "list-detail-val";
        if (k === "cached") {
            vEl.textContent = "cached";
            vEl.classList.add("list-detail-muted");
        } else if (isScalarArray) {
            for (const e of v) {
                const chip = document.createElement("span");
                chip.className = "list-detail-chip";
                chip.textContent = String(e);
                vEl.appendChild(chip);
            }
        } else if (isDuration) {
            vEl.textContent = relativeAge(Number(v));
            const ageClass = ageBucketClass(Number(v));   // tint to match the summary dot
            if (ageClass) vEl.classList.add(ageClass);
        } else if (typeof v === "string" && /^https?:\/\//.test(v)) {
            // A value that is an http(s) URL (e.g. a device's `url`) renders as a link that
            // opens in a new tab: generic, any ListSource detail can surface one. rel
            // guards the opened page from reaching back via window.opener.
            const a = document.createElement("a");
            a.href = v;
            a.textContent = v;
            a.target = "_blank";
            a.rel = "noopener noreferrer";
            a.className = "list-detail-link";
            vEl.appendChild(a);
        } else {
            vEl.textContent = String(v);
        }
        r.append(kEl, vEl);
        panel.appendChild(r);
    }
}

// Freshness bucket for a duration-in-seconds (a `*Sec` field): green < 1 min, orange
// < 1 hour, red beyond. Generic: any duration field a ListSource emits gets the same
// scale; nothing device-specific here. (DevicesModule's ageSec is the first user: a
// device unseen > 24h is aged out of the list entirely, so "red" spans 1h-24h.)
function ageBucketClass(sec) {
    if (!Number.isFinite(sec) || sec < 0) return "";
    if (sec < 60) return "age-fresh";
    if (sec < 3600) return "age-recent";
    return "age-stale";
}

// Find a row's freshness CSS class from its first `*Sec` scalar field, or "" if it has
// none. `self` is always "now" → fresh; a `cached` row (restored, not re-seen) is
// unknown-age → no dot (the detail says "cached"). Generic over field names.
function rowAgeClass(item) {
    if (!item || typeof item !== "object") return "";
    if (item.self) return "age-fresh";
    if (item.cached) return "";
    for (const [k, v] of Object.entries(item)) {
        if (k.endsWith("Sec") && typeof v === "number") return ageBucketClass(v);
    }
    return "";
}

// Severity CSS class from a row's `severity` field ("error"/"warn"), or "" if none/absent. Generic
// over the field name, exactly like rowAgeClass over `*Sec`: any ListSource that emits a `severity`
// string gets the same visual (a colored row marker). PinsModule is the first user: it flags a GPIO
// claim on a reserved/strap/input-only pin: but nothing here is pins-specific; a task in a bad state
// or a device with an error could emit `severity` and light up the same way.
function rowSeverityClass(item) {
    if (!item || typeof item !== "object") return "";
    if (item.severity === "error") return "list-severity-error";
    if (item.severity === "warn") return "list-severity-warn";
    return "";
}

// Render a seconds-ago count as a short relative time ("just now", "2m ago", "3h ago",
// "5d ago"). Snapshot at state-push time: it refreshes when the list re-renders, not
// per second. Mirrors the device-side ageSec (now - lastSeenMs); kept simple on purpose.
function relativeAge(sec) {
    if (!Number.isFinite(sec) || sec < 0) return "-";
    if (sec < 10) return "just now";
    if (sec < 60) return `${sec}s ago`;
    if (sec < 3600) return `${Math.floor(sec / 60)}m ago`;
    if (sec < 86400) return `${Math.floor(sec / 3600)}h ago`;
    return `${Math.floor(sec / 86400)}d ago`;
}

/// Send any text control of this module whose debounced write is still pending, and wait for it.
///
/// Typing is debounced, so anything that ACTS on the typed value (Enter, a send button) has to land
/// the text first or it acts on what the device held a keystroke ago.
async function flushPendingControlWrites(moduleName) {
    const mod = allModules().find(m => m.name === moduleName);
    for (const ctrl of mod?.controls || []) {
        const key = moduleName + ":" + ctrl.name;
        if (!dragTimers[key]) continue;
        clearTimeout(dragTimers[key]);
        delete dragTimers[key];
        // queryByName, not a bare querySelector: a CSS attribute match is case-insensitive, so
        // `data-mid="talk"` would resolve to a module named `Talk`. See queryByName for the bench
        // case that found it.
        const input = queryByName(
            `input[data-key="${cssEscape(ctrl.name)}"]`, "data-mid", moduleName);
        if (input) await sendControl(moduleName, ctrl.name, input.value);
    }
}

function appendResetButton(row, moduleName, ctrl, def, applyVisually) {
    if (def === undefined || def === null) return;  // type not loaded yet or no default
    // A SURFACE control has no default worth restoring: its value belongs to whatever it drives, so
    // "reset" would drive that target to zero, which is a change rather than a reset. The row is
    // also 26px wide, and the button was taking space from the thing being operated.
    //
    // A CONSENT control is the same shape for a different reason: off is where it starts, so the
    // button's only possible action is to withdraw consent, which is a decision rather than a
    // reset. The checkbox already expresses both answers.
    if (ctrl.fader || ctrl.encoder || ctrl.switchRow || ctrl.name === "consent") return;
    const btn = document.createElement("button");
    btn.className = "reset-btn";
    btn.type = "button";
    btn.textContent = "↺";
    btn.title = `Reset to default (${def})`;
    btn.dataset.mid = moduleName;
    btn.dataset.key = ctrl.name + ".reset";
    btn.dataset.def = String(def);
    const eq = controlValuesEqual(ctrl, def);
    btn.classList.toggle("active", !eq);
    btn.addEventListener("click", () => {
        const key = moduleName + ":" + ctrl.name;
        clearTimeout(dragTimers[key]);   // a pending debounced edit must not overwrite the reset
        dragTs[key] = Date.now();        // and a stale WS patch must not revert it
        applyVisually();
        sendControl(moduleName, ctrl.name, def);
    });
    row.appendChild(btn);
}

function debounceSend(key, ms, fn) {
    clearTimeout(dragTimers[key]);
    dragTimers[key] = setTimeout(fn, ms);
}

// Password controls arrive XOR-obfuscated + base64-encoded (see
// HttpServerModule PASSWORD_XOR_KEY). This reverses it. The XOR key is a fixed
// shared constant, not a secret: this is obfuscation so the password is not
// plainly readable in a raw /api/state response, not real encryption.
const PW_XOR_KEY = 0x5A;
function decodePassword(encoded) {
    if (!encoded) return "";
    try {
        const bytes = atob(encoded);
        let out = "";
        for (let i = 0; i < bytes.length; i++) {
            out += String.fromCharCode(bytes.charCodeAt(i) ^ PW_XOR_KEY);
        }
        return out;
    } catch {
        return "";
    }
}

function fmtTime(sec) {
    sec = Math.max(0, Math.floor(Number(sec) || 0));
    const d = Math.floor(sec / 86400); sec -= d * 86400;
    const h = Math.floor(sec / 3600);  sec -= h * 3600;
    const m = Math.floor(sec / 60);    sec -= m * 60;
    const parts = [];
    if (d) parts.push(d + "d");
    if (d || h) parts.push(h + "h");
    if (d || h || m) parts.push(m + "m");
    parts.push(sec + "s");
    return parts.join(" ");
}

function fmtProgressLabel(ctrl) {
    const v = Number(ctrl.value) || 0;
    const t = Number(ctrl.total) || 0;
    // bytes === false → a plain count (e.g. a scan position "37 / 254"); otherwise
    // KB (the heap / flash / filesystem gauges, the original use).
    if (ctrl.bytes === false) return v + " / " + t;
    return Math.round(v / 1024) + "KB / " + Math.round(t / 1024) + "KB";
}

// "<value> <unit>": treats null / undefined / 0 as "unavailable" so the
// UI doesn't render bogus "0 dBm" when the device is in a state where the
// metric isn't meaningful. The device's updateMetrics() writes 0 to rssi /
// txPower in non-WiFi states; the control is hidden in those states, but
// if anyone toggles hidden off (DevTools, future code path) the unit-with-
// zero rendering would still mislead. Real metric values are never zero
// in practice: RSSI is negative, TX power is 0..127 dBm (zero only on
// driver-uninitialized reads).
// Is a display control's value a url? One predicate, because BOTH render paths (renderCards and
// updateModuleControls) have to agree on which values become links: a rule applied in only one
// of them shows a link that the next state push replaces with plain text.
function isUrlValue(v) {
    return typeof v === "string" &&
           (v.startsWith("/") || /^https?:\/\//.test(v) || v.startsWith("rtsp://"));
}

// Point a link at `value` and show it ABSOLUTE. The stored value is device-relative, but a user
// reading the card wants the address they could paste into a player, and `a.href` resolves that
// against the current origin for us.
// A non-http scheme resolves against nothing, so `rtsp://:554/` names its port and leaves the host
// to us: the device has several addresses and cannot know which one this reader used, while the
// page was loaded from exactly that one.
function setUrlDisplay(a, value) {
    let v = value ?? "";
    if (v.startsWith("rtsp://:")) v = "rtsp://" + location.hostname + v.slice("rtsp://".length);
    if (a.getAttribute("href") === v) return;   // unchanged: leave the DOM alone
    a.setAttribute("href", v);
    a.textContent = v;   // an unknown scheme leaves a.href untouched, so show what we built
}

function fmtDisplayInt(ctrl) {
    const v = ctrl.value;
    const u = ctrl.unit || "";
    if (v === null || v === undefined || v === 0) return "";
    return u ? `${v} ${u}` : String(v);
}

// ---------------------------------------------------------------------------
// 5. State patching (no-rebuild contract)
// ---------------------------------------------------------------------------

// Apply a diff-on-the-wire patch onto the in-memory `state`. Each entry is {path,value} where path is
// "<module>/<control>" for a control value, or "<module>/@<field>" for a live module-header field
// (tickTimeUs / dynamicBytes: shown per card). Module names are unique tree-wide (the device dedups
// them), so the first path segment identifies the module. Mutates `state` in place; the caller then
// calls updateValues() to refresh the DOM without a rebuild. An unknown path is skipped (a resync will
// reconcile). List controls: the value is the full summary array (the device sends a changed list's
// whole value: see the plan), so we replace it wholesale, which updateModuleControls/renderCards read.
function applyStatePatch(patch) {
    const byName = new Map(allModules().map(m => [m.name, m]));
    for (const entry of patch) {
        if (!entry || typeof entry.path !== "string") continue;
        const slash = entry.path.indexOf("/");
        if (slash < 0) continue;
        const modName = entry.path.slice(0, slash);
        const leaf = entry.path.slice(slash + 1);
        const mod = byName.get(modName);
        if (!mod) continue;
        if (leaf[0] === "@") {
            mod[leaf.slice(1)] = entry.value;   // header field, e.g. @tickTimeUs -> mod.tickTimeUs
        } else if (Array.isArray(mod.controls)) {
            const c = mod.controls.find(c => c.name === leaf);
            if (c) c.value = entry.value;
        }
    }
}

/// Refresh the decorations drawn ON TOP of a range input (the knob dial, the seven-segment readout).
///
/// A WebSocket patch assigns `input.value` directly, which fires NEITHER `input` NOR `change`: those
/// only fire for user gestures. Anything listening for them therefore keeps showing the value it was
/// built with, which is why the readouts read 880/50 while the knobs pointed elsewhere. Call this
/// wherever a range input's value is set programmatically.
function redrawRangeDecorations(input) {
    if (!input) return;
    for (const el of input.parentElement ? input.parentElement.children : []) {
        if (typeof el._redraw === "function") el._redraw();
    }
}

// Write text only when it actually changed. Assigning textContent REPLACES the text node, which
// collapses any selection inside it, so an unconditional write once a second makes a value
// impossible to select and copy: the highlight dies under the cursor. It is also wasted DOM work,
// since most of these strings are identical from one second to the next.
// A control's DISPLAY label. The name itself stays the industry-standard word, because it is the
// API key, the persisted key and the OSC address (`/mm/fader/1`), and a surface built against a
// standard name is portable where an invented abbreviation is not. But eight of them side by side
// in a strip have no room for it, so the shortening lives here, in the one place a label is drawn.
const kShortLabels = {fader: "fad", encoder: "enc", switch: "sw"};
function displayName(name) {
    const m = /^([a-z]+)(\d+)$/.exec(name);
    return m && kShortLabels[m[1]] ? kShortLabels[m[1]] + m[2] : name;
}

function setText(el, text) {
    if (el && el.textContent !== text) el.textContent = text;
}

// The mode the cards were BUILT at. `uiMode()` decides which controls and which stats line a card
// gets, so a change to it reshapes the DOM rather than a value in it: the one case the no-rebuild
// contract cannot patch. Tracked here because the change can come from another client entirely.
let renderedMode_ = null;

function updateValues() {
    if (!state || !state.modules) return;
    // A mode change adds or removes rows, so patching values cannot express it: rebuild once.
    const mode = uiMode();
    if (renderedMode_ !== null && renderedMode_ !== mode) {
        renderedMode_ = mode;
        renderCards();
        return;
    }
    renderedMode_ = mode;
    // Patch each visible card's controls and stats line; never rebuild the DOM here.
    for (const mod of allModules()) {
        updateTabDot(mod);   // a fault on a BACKGROUND tab must surface without opening it
        updateModuleControls(mod);
        // refresh the stats line for this module if visible
        const statsEl = queryByName(`.card-stats[data-mid="${cssEscape(mod.name)}"]`, "data-mid", mod.name);
        if (statsEl) {
            setText(statsEl, formatStats(mod));
            const t = formatStatsTitle(mod);
            if (statsEl.title !== t) statsEl.title = t;
        }
        // refresh status row: insert it if status appeared after card build
        let statusRow = queryByName(`[data-status-mid="${cssEscape(mod.name)}"]`, "data-status-mid", mod.name);
        if (mod.status) {
            if (!statusRow) {
                // Card exists but had no status at build time: insert now before first control.
                const card = queryByName(`.card[data-module="${cssEscape(mod.name)}"]`, "data-module", mod.name);
                const host = card && (card.querySelector(".card-controls-collapse") || card);
                if (host) {
                    statusRow = document.createElement("div");
                    statusRow.className = "control-row";
                    statusRow.dataset.statusMid = mod.name;
                    const label = document.createElement("span");
                    label.className = "control-label";
                    label.textContent = "status";
                    const val = document.createElement("span");
                    val.className = "status-value";
                    val.dataset.sev = mod.severity || "status";
                    setStatusText(val, mod);
                    statusRow.appendChild(label);
                    statusRow.appendChild(val);
                    // Insert before first .control-row, or append.
                    const firstRow = host.querySelector(".control-row");
                    firstRow ? host.insertBefore(statusRow, firstRow) : host.appendChild(statusRow);
                }
            } else {
                statusRow.style.display = "";
                const val = statusRow.querySelector(".status-value");
                if (val) {
                    setStatusText(val, mod);
                    const sev = mod.severity || "status";
                    if (val.dataset.sev !== sev) val.dataset.sev = sev;
                }
            }
        } else if (statusRow) {
            statusRow.style.display = "none";
        }
        // refresh enabled toggle (now a styled <button>, not an <input>)
        const enabledEl = queryByName(`button.module-enabled[data-mid="${cssEscape(mod.name)}"]`, "data-mid", mod.name);
        if (enabledEl) {
            const ts = dragTs[mod.name + ":enabled"] || 0;
            if (Date.now() - ts > 1000) {
                const on = (mod.enabled === undefined) ? true : !!mod.enabled;
                enabledEl.dataset.checked = on ? "true" : "false";
                setText(enabledEl, "\u23FB");
                enabledEl.classList.toggle("module-enabled--off", !on);
                enabledEl.setAttribute("aria-pressed", on ? "true" : "false");
                const cardEl = queryByName(`.card[data-module="${cssEscape(mod.name)}"]`, "data-module", mod.name);
                if (cardEl) cardEl.classList.toggle("card--disabled", !on);
            }
        }
    }
    updateStatusBar();
}

function allModules() {
    const out = [];
    function walk(modules) {
        for (const m of modules) {
            out.push(m);
            if (m.children) walk(m.children);
        }
    }
    if (state && state.modules) walk(state.modules);
    return out;
}

// Reconcile a card's control rows when its set of VISIBLE controls changed (a
// `hidden` flag flipped at runtime, e.g. NetworkModule's static-IP fields or
// RmtLedDriver's loopbackRxPin). The value-patch path in updateModuleControls
// can't add or remove rows, so this handles that half. Returns true if it
// changed the DOM. No-op (returns false) on the common frame where nothing moved.
//
// Position-stable by design: it inserts each newly-visible row at its correct
// index among the existing control rows and removes rows that became hidden -
// it does NOT tear down and re-append every row (that would land them after the
// card's child-module block / install-picker mount, never converge, and re-fire
// every WS tick: a render loop that wedges the UI).
function syncVisibleControls(mod) {
    const card = queryByName(`.card[data-module="${cssEscape(mod.name)}"]`, "data-module", mod.name);
    if (!card) return false;
    // The controls host is THIS card's own collapse wrapper: must be a DIRECT
    // child (`:scope >`), not any descendant: a container card (e.g. Effects) nests
    // its child cards (Layer) inside .card-children, and a plain
    // `card.querySelector(".card-controls-collapse")` would reach down and match
    // the CHILD's wrapper. That made Effects adopt Layer's control rows as its own,
    // so both cards saw a control-set mismatch every WS frame and rebuilt each
    // other's rows in a loop: tearing down (and closing) any open <select>.
    const host = card.querySelector(":scope > .card-controls-collapse") || card;

    // THE FIRMWARE CARD'S IMAGE TABS, which this patch path would otherwise never touch. The strip
    // is built in createCard, and so are the install routes it implies: `kind` is captured in the
    // handlers' closures. So a stale strip is not merely a wrong highlight, it is a card whose
    // buttons write the OTHER partition than the one it names. Rebuild rather than repaint the
    // highlight: the closures cannot be patched.
    //
    // The UI has two render paths and a rule must live in both (see renderChildTabs' updateTabDot,
    // written for the other tab strip for exactly this reason).
    const strip = card.querySelector(":scope > .tab-strip");
    if (strip) {
        const sel = (mod.controls.find(c => c.name === "image") || {}).value;
        const shown = [...strip.children].findIndex(t => t.classList.contains("tab-active"));
        if (sel !== undefined && shown >= 0 && shown !== sel) { renderCards(); return true; }
    }

    const wantNames = mod.controls.filter(c => controlRendersGenerically(mod, c)).map(c => c.name);
    const haveRows = [...host.querySelectorAll(":scope > .control-row[data-key]")];
    const haveNames = haveRows.map(r => r.dataset.key);
    if (wantNames.length === haveNames.length && wantNames.every((n, i) => n === haveNames[i])) {
        return false;  // unchanged: the common case, so an idle WS push rebuilds nothing
    }
    // The visible-control set genuinely drifted (a hidden flag flipped: e.g. whiteMode appearing when
    // the preset changes). Rebuild to reflect it; latest state wins. We don't defer for an in-progress
    // edit: the drift is a consequence of a real change, and suppressing it would hide that change.

    // Remove rows whose control is no longer visible.
    const wantSet = new Set(wantNames);
    for (const row of haveRows) {
        if (!wantSet.has(row.dataset.key)) row.remove();
    }
    // Insert each visible control's row at its correct position. The anchor is the
    // first existing control row that should come AFTER this one; null → append
    // before the children block (insertBefore(node, null) appends to host's end,
    // but control rows precede .card-children which lives on the card, not here).
    const visibleControls = mod.controls.filter(c => controlRendersGenerically(mod, c));
    for (let i = 0; i < visibleControls.length; i++) {
        const name = visibleControls[i].name;
        if (host.querySelector(`:scope > .control-row[data-key="${cssEscape(name)}"]`)) continue;
        const row = createControl(mod.name, mod.type, visibleControls[i]);
        if (!row) continue;
        // Anchor: the rendered row of the next visible control that already exists.
        let anchor = null;
        for (let j = i + 1; j < visibleControls.length && !anchor; j++) {
            anchor = host.querySelector(`:scope > .control-row[data-key="${cssEscape(visibleControls[j].name)}"]`);
        }
        // No later control row exists yet → keep this row above the card's
        // children block / install-picker mount / footer (which live in the host
        // when host===card), so controls never render below the children.
        if (!anchor) {
            anchor = host.querySelector(":scope > .card-children")
                  || host.querySelector(":scope > .install-picker-host")
                  || host.querySelector(":scope > .card-footer");
        }
        host.insertBefore(row, anchor);
    }
    // Rebuild the bank breaks. This path inserts rows one at a time, so it cannot know where a bank
    // ends while it works; doing it once at the end, from the same predicate createCard uses, is
    // what keeps the two paths agreeing. Stale breaks are dropped first: a rebuild that left them
    // in place would wrap the strips at last render's boundaries.
    for (const br of host.querySelectorAll(":scope > .surface-break")) br.remove();
    let bank = null;
    for (const c of visibleControls) {
        const kind = surfaceBank(c);
        const row = host.querySelector(`:scope > .control-row[data-key="${cssEscape(c.name)}"]`);
        if (!row) continue;
        if (bank && kind !== bank) host.insertBefore(surfaceBreak(), row);
        bank = kind;
    }
    return true;
}

function updateModuleControls(mod) {
    if (!mod.controls) return;

    // Conditional controls: a module can flip a control's `hidden` flag at runtime
    // (e.g. RmtLedDriver reveals loopbackRxPin while the test is on, NetworkModule
    // reveals static-IP fields). The value-patch loop below only updates controls
    // already in the DOM: it can't add or remove one. So first detect whether the
    // set of VISIBLE controls drifted from what's rendered, and if so re-render
    // this card's control rows. Cheap: only fires on the rare frame where a hidden
    // flag actually changed.
    if (syncVisibleControls(mod)) return;  // re-rendered: values are fresh, skip patch

    for (const ctrl of mod.controls) {
        // RAW, not escaped: every selector below escapes it itself, and queryByName compares this
        // against the data-mid ATTRIBUTE, which the DOM returns unescaped. Escaping here made the
        // comparison fail for any module name that needs escaping (a quote or a backslash), so the
        // live patch silently found nothing and that card stopped updating.
        const mid = mod.name;
        const k = cssEscape(ctrl.name);
        const dragKey = mod.name + ":" + ctrl.name;
        const ts = dragTs[dragKey] || 0;
        const userActive = Date.now() - ts < 1000;

        // One guard for every editable control: while the user is mid-edit (a
        // keystroke / drag within the last second, tracked by dragTs), don't let a
        // WS state push overwrite the field with the value it had before the edit
        // landed. The read-only types (display/display-int/time/progress) and the
        // composite `list` aren't in this set: they always reflect the latest push.
        if (userActive && EDITABLE_CONTROL_TYPES.has(ctrl.type)) continue;

        switch (ctrl.type) {
            case "uint8":
            case "uint16":
            case "int16":
            case "int32":
            case "pin": {   // pin is a plain number input (no slider sibling); patches the same way
                const input = queryByName(`input[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                // While the demo sweep animates a control, leave it alone: the sweep restores the
                // real value when it ends, and the next patch after that lands normally.
                if (input && surfaceDemoRunning() &&
                    (input.classList.contains("encoder-input") || input.classList.contains("fader-input"))) break;
                if (input && Number(input.value) !== Number(ctrl.value)) {
                    input.value = ctrl.value ?? 0;
                    const val = input.nextElementSibling;
                    if (val && val.classList.contains("control-value-input")) val.value = ctrl.value ?? 0;
                    redrawRangeDecorations(input);
                }
                break;
            }
            case "bool": {
                const input = queryByName(`input[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (input && input.checked !== !!ctrl.value) input.checked = !!ctrl.value;
                break;
            }
            case "text": {
                const input = queryByName(`input[type="text"][data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "textarea": {
                const input = queryByName(`textarea[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                // Don't clobber the box while the user is typing in it.
                if (input && document.activeElement !== input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "filepath": {
                // A BUTTON, not a select: buildFilePathControl replaced the native select with a
                // painted picker that keeps its value in `_value` behind a `value` property, so the
                // assignment below still works. Matching `select` here found nothing, and a script
                // changed from another client (or by a preset) never repainted this picker.
                const sel = queryByName(`button.fileedit-pick[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                // Don't clobber the choice while it is focused, and don't reload the pane under
                // someone who is typing in it: the value is only pushed back when it really moved.
                if (sel && document.activeElement !== sel && sel.value !== (ctrl.value ?? "")) {
                    sel.value = ctrl.value ?? "";
                }
                break;
            }
            case "password": {
                // The peek button flips the input to type="text", so match either.
                const input = queryByName(`input[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                const decoded = decodePassword(ctrl.value);
                if (input && input.value !== decoded) input.value = decoded;
                break;
            }
            case "select": {
                const sel = queryByName(`select[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                // Never overwrite a select the user currently has OPEN (popup
                // showing) or focused. data-open is set on pointerdown/focus and
                // cleared on change/blur: more reliable than document.activeElement,
                // which is ambiguous while a native popup is up (the popup is a
                // separate OS layer on macOS). The 1s dragTs cooldown is the
                // additional fallback for the frames right after the popup closes.
                if (sel && sel.dataset.open !== "true" && sel !== document.activeElement) {
                    // Re-sync the OPTION list when it changed since render: some selects are
                    // populated asynchronously (e.g. HueDriver learns its rooms/lights ~1-2s after
                    // boot, growing this select from ["All"] to the full list). The value-only patch
                    // below can't reveal new options, so rebuild them in place when they differ.
                    const opts = ctrl.options || [];
                    const cur = Array.from(sel.options).map(o => o.textContent);
                    if (cur.length !== opts.length || opts.some((o, i) => o !== cur[i])) {
                        sel.innerHTML = "";
                        opts.forEach((opt, i) => {
                            const o = document.createElement("option");
                            o.value = i;
                            o.textContent = opt;
                            sel.appendChild(o);
                        });
                    }
                    const want = selectIndex(ctrl);
                    if (Number(sel.value) !== want) sel.value = want;
                }
                break;
            }
            case "palette": {
                // Custom dropdown: patch the trigger (swatch + name) and the selected row, but not
                // while the user has the list open (data-open === "true").
                const wrap = queryByName(`.palette-control[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (wrap && wrap.dataset.open !== "true" && Number(wrap.dataset.value) !== Number(ctrl.value)) {
                    wrap.dataset.value = ctrl.value;
                    const cols = ((ctrl.options || [])[ctrl.value] || {}).colors || "";
                    const grad = paletteGradientCss(cols);
                    const triSwatch = wrap.querySelector(".palette-trigger .palette-swatch");
                    if (triSwatch) triSwatch.style.background = grad;
                    const triName = wrap.querySelector(".palette-trigger .palette-name");
                    if (triName) setText(triName, ((ctrl.options || [])[ctrl.value] || {}).name || String(ctrl.value));
                    wrap.querySelectorAll(".palette-item.selected").forEach(x => x.classList.remove("selected"));
                    const row = wrap.querySelector(`.palette-item[data-idx="${ctrl.value}"]`);
                    if (row) row.classList.add("selected");
                }
                break;
            }
            case "display": {
                // A url-valued display is an <a>, not a <span> (see renderControl), so this path
                // has to know both shapes or the link goes stale on the next push.
                const link = queryByName(`a.control-url[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (link) { setUrlDisplay(link, ctrl.value); break; }
                // The display strip is a segment renderer, not a span: it has to be patched through
                // its own hook or the surface would freeze at whatever it showed on first render.
                const strip = queryByName(`.seg16[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (strip && strip._setText) { strip._setText(ctrl.value ?? ""); break; }
                const span = queryByName(`span.display[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (span) setText(span, String(ctrl.value ?? ""));
                break;
            }
            case "display-int": {
                const span = queryByName(`span.display[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (span) {
                    // Re-cache the unit in case the device changed it (it
                    // shouldn't, but the WS path is the authority).
                    span.dataset.unit = ctrl.unit ?? span.dataset.unit ?? "";
                    setText(span, fmtDisplayInt(ctrl));
                }
                break;
            }
            case "ipv4": {
                // Guarded by the shared userActive check above (same as text).
                const input = queryByName(`input.ipv4-input[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (input && input.value !== (ctrl.value ?? "")) input.value = ctrl.value ?? "";
                break;
            }
            case "time": {
                const span = queryByName(`span.display[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (span) setText(span, fmtTime(ctrl.value ?? 0));
                break;
            }
            case "progress": {
                const bar = queryByName(`progress[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (bar) {
                    bar.value = ctrl.value ?? 0;
                    bar.max = ctrl.total ?? 100;
                }
                const lbl = queryByName(`span.control-value[data-mid="${cssEscape(mid)}"][data-key="${k}.label"]`, "data-mid", mid);
                if (lbl) setText(lbl, fmtProgressLabel(ctrl));
                break;
            }
            case "list": {
                const list = queryByName(`div.list-control[data-mid="${cssEscape(mid)}"][data-key="${k}"]`, "data-mid", mid);
                if (!list) break;
                const rows = Array.isArray(ctrl.value) ? ctrl.value : [];
                const details = Array.isArray(ctrl.detail) ? ctrl.detail : [];
                // ONLY REBUILD IF THE LIST ACTUALLY CHANGED. The device pushes full state ~1/sec, so an
                // unconditional rebuild destroyed + recreated every row (and its inline dropdowns / drag
                // handlers) every second: collapsing an open dropdown and breaking a drag. Compare the
                // incoming rows+details against the last rendered snapshot cached on the element; if
                // identical, do nothing (the common case, and the whole fix). Only a GENUINE change
                // (someone edited the list) rebuilds: latest state wins, no attempt to protect a stale
                // in-progress view (that would suppress the user's own edit from re-rendering). A
                // rebuild on a real change is momentary and a direct consequence of that change.
                const sig = JSON.stringify([rows, details]);
                if (list.dataset.sig === sig) break;   // unchanged: leave the DOM (and any open edit) alone

                // A field the user is STILL EDITING survives the rebuild. Committing one field
                // changes the list, which rebuilds every row from the device's state, and any other
                // field typed but not yet committed went back to what the device still had: every
                // field had to be entered twice, the second time sticking only because the first
                // had committed by then. The row fields already stamp dragTs on each keystroke, so
                // the same one-second cooldown every other control uses applies here too.
                const held = new Map();
                for (const el of list.querySelectorAll("[data-dragkey]")) {
                    const ts = dragTs[el.dataset.dragkey] || 0;
                    if (Date.now() - ts < 1000) held.set(el.dataset.dragkey, el.value);
                }
                // Which field had focus, so typing can continue into the rebuilt one rather than
                // into nothing: the rebuild replaces the node, and focus goes with it.
                const focusedKey = document.activeElement?.dataset?.dragkey;
                list.dataset.sig = sig;
                // Preserve which detail panels are open across the rebuild. Capture the SAME key
                // listRowKey emits: the row's stable `id` (on entry.dataset.rowId for editable
                // rows) when present, else the summary label text. Keying on id means changing a
                // field shown in the label (a preset's channel count) keeps the row expanded.
                const open = new Set(
                    [...list.querySelectorAll(".list-entry")]
                        .filter(e => { const d = e.querySelector(".list-detail"); return d && !d.hidden; })
                        .map(e => e.dataset.rowId != null
                            ? "#" + e.dataset.rowId
                            : e.querySelector(".list-summary-label")?.textContent));
                // Preserve scroll position too: a rebuild recreates .list-scroll (scrollTop 0), which
                // would jump a long list back to the top mid-edit of a row further down.
                const prevScroll = list.querySelector(".list-scroll")?.scrollTop ?? 0;
                buildListEntries(list, rows, details, open,
                    {editable: ctrl.editable, pads: ctrl.pads, gridCols: ctrl.gridCols, gridRows: ctrl.gridRows,
                     moduleName: mod.name, ctrlName: ctrl.name, optionSets: ctrl.optionSets || {}});
                const newScroll = list.querySelector(".list-scroll");
                if (newScroll) newScroll.scrollTop = prevScroll;
                // Put the in-progress values back, and the caret with them where the field is the
                // one being typed in: restoring the text alone would send the cursor to the start.
                for (const el of list.querySelectorAll("[data-dragkey]")) {
                    if (!held.has(el.dataset.dragkey)) continue;
                    const wasFocused = el.dataset.dragkey === focusedKey;
                    el.value = held.get(el.dataset.dragkey);
                    if (wasFocused) el.focus();
                    if (wasFocused && typeof el.setSelectionRange === "function") {
                        const end = el.value.length;
                        try { el.setSelectionRange(end, end); } catch { /* not a text input */ }
                    }
                }
                break;
            }
        }
        // Reset-button state may change as the value drifts in/out of default. A default on the
        // control itself wins over the type-level ones from /api/types; see defaultFor.
        const def = defaultFor(mod.type, ctrl.name, ctrl);
        if (def !== undefined && def !== null) {
            const btn = queryByName(`button.reset-btn[data-mid="${cssEscape(mid)}"][data-key="${k}.reset"]`, "data-mid", mid);
            if (btn) {
                const eq = controlValuesEqual(ctrl, def);
                btn.classList.toggle("active", !eq);
            }
        }
    }
}

// Per-type equality for reset-button highlighting. bool→boolish, ipv4/text→
// string compare, everything else → numeric. Centralized so the rules cannot
// drift between createControl and updateModuleControls.
function controlValuesEqual(ctrl, def) {
    if (ctrl.type === "bool") return !!ctrl.value === !!def;
    if (ctrl.type === "ipv4" || ctrl.type === "text" || ctrl.type === "textarea" ||
        ctrl.type === "filepath" || ctrl.type === "password") {
        return String(ctrl.value ?? "") === String(def ?? "");
    }
    return Number(ctrl.value) === Number(def);
}

function cssEscape(s) {
    // Minimal CSS attribute selector escape. Module/control names are alphanumeric
    // in practice, so this is defensive.
    return String(s).replace(/(["\\])/g, "\\$1");
}

// Find the one element whose `attr` EQUALS `value`, case included.
//
// CSS attribute selectors match values case-INSENSITIVELY in an HTML document, while the firmware
// compares module names with strcmp. So `lines` (a MoonLive effect) and `Lines` (a LinesEffect) are
// two different modules that every `[data-module="..."]` lookup confuses: querySelector returns
// whichever sits first in the DOM, and on the bench (2026-09-08) a Layer holding both rendered no
// effect cards at all. Selectors Level 4 has a case-sensitivity flag for exactly this
// (`[data-module="x" s]`) and Chrome does not support it: it throws SyntaxError, which takes out
// every card on the page. So the match is narrowed here instead, which works everywhere.
function queryByName(selector, attr, value) {
    const want = String(value);
    for (const el of document.querySelectorAll(selector)) {
        if (el.getAttribute(attr) === want) return el;
    }
    return null;
}

// ---------------------------------------------------------------------------
// 6. Type picker
// ---------------------------------------------------------------------------

/// The CSS gradient for a palette's swatch, from the space-separated hex list the device sends.
///
/// One definition: the palette control, the swatch strip and the picker row all paint the same
/// gradient, and three copies of this had already started to drift in their empty-list handling.
function paletteGradientCss(colors) {
    const stops = String(colors || "").split(/\s+/).filter(Boolean).map(h => "#" + h);
    return stops.length ? `linear-gradient(to right, ${stops.join(",")})` : "none";
}

// Role → emoji, derived here rather than duplicated in every module's tags(): one home in the UI
// saves repeating the same character in ~90 module headers and a few bytes per type in /api/types.
// The dimensional chip comes from the type's `dim` the same way (DIM_EMOJI below), and both are
// merged with the module's own tags() in emojiTagsFor().
//
// What each emoji MEANS is documented once, for the people who read the chips:
// docs/tutorials/how-projectmm-works.md, "The emoji on every card". Re-listing the vocabulary here
// is how this comment came to name four emoji no module carries any more.
const ROLE_EMOJI = {
    effect:     "🔥",
    driver:     "☸️",
    modifier:   "💎",
    layout:     "🚥",
    layer:      "🥞",
    effects:    "🥞",   // the container a preset captures; same pancake as the Layers it holds
    service:    "🛰️",
    generic:    "⚙️",
};

// Role → hue, the color half of the same vocabulary ROLE_EMOJI carries. Four capturable roles get a
// distinct hue so a preset pad says what it covers before its name is read. Hues (not full colors) so
// one value drives the face, the border and the glow at different mixes, and so several roles can be
// averaged into one tint.
const ROLE_HUE = {
    layout:   210,   // blue
    effects:  280,   // violet
    driver:   150,   // green
    service:   35,   // amber
};

/// The tint for a preset's role. A preset carries exactly one role, so this is a lookup: a file
/// naming several is one an older build wrote, and gets no tint because it cannot be applied.
function roleHue(roles) {
    const hues = (roles || []).map(r => ROLE_HUE[r]).filter(h => h != null);
    return hues.length === 1 ? String(hues[0]) : null;
}

// Dim int → emoji. Effects, layouts and modifiers all carry `dim` (1/2/3); everything else has 0
// and contribute nothing here. Same MoonLight key. Keeps emojiTagsFor() the
// single place that assembles the chip set per type.
const DIM_EMOJI = {
    1: "📏",
    2: "🟦",
    3: "🧊",
};

// Split a string into grapheme clusters so multi-codepoint emoji (e.g. 🌫️,
// which is base char + variation selector) stay whole. Falls back to a plain
// code-point split if Intl.Segmenter is unavailable.
const _graphemeSeg = (typeof Intl !== "undefined" && Intl.Segmenter)
    ? new Intl.Segmenter(undefined, {granularity: "grapheme"})
    : null;
function graphemes(s) {
    if (!s) return [];
    if (_graphemeSeg) return [..._graphemeSeg.segment(s)].map(seg => seg.segment);
    return [...s];
}

// All emoji for a type: role first, then dimensional (effects only), then each
// curated tag emoji from tags(). Deduplicated, order preserved.
// A script is marked; a compiled module is not. Two chips in the picker filter on this, and the
// compiled one matches by ABSENCE, so adding it costs no tag on ninety existing modules.
const SCRIPTED_EMOJI = "\u{1F4DD}";      // 📝 the module runs a MoonLive script
// Not the gear: that is already the `generic` role's emoji, and reusing it drew the chip twice.
const COMPILED_EMOJI = "\u{1F4E6}";      // 📦 chip only: never a tag any module carries

// The POWER-FUNCTION tags: which kernels an effect is built on, rather than where it came from or
// what it listens to. They are ordered LAST and kept together so they read as one group on a card
// and sit adjacent in the picker's chip row, which is what makes "show me the fluid effects" a
// glanceable filter rather than a hunt through a mixed string. A separator character is not used:
// every chip is one grapheme, so a separator would become a chip of its own.
// What an effect LISTENS to. Their own group in the picker: "show me the effects that react to
// music" is the question a chip row is for, and the two notes answer it together.
const AUDIO_EMOJI = ["\u{1F3B5}", "\u{1F3B6}"];   // volume, frequency

const KERNEL_EMOJI = ["🖌️", "✨", "🌊", "💨", "🌫️", "🎡"];   // same order the legend lists them in

// What each chip MEANS, as a tooltip. The chips are a filter, so a reader who does not yet know the
// vocabulary has to guess what narrows the list; the title makes each one self-describing without
// spending any screen space. Wording follows the legend that documents them for people reading a
// card: docs/tutorials/how-projectmm-works.md, "The emoji on every card".
const EMOJI_LABEL = {
    // what the module IS
    "\u{1F525}": "effect",
    "\u2638\uFE0F": "driver",
    "\u{1F48E}": "modifier",
    "\u{1F6A5}": "layout",
    "\u{1F95E}": "layer",
    "\u{1F6F0}\uFE0F": "service",
    "\u2699\uFE0F": "generic",
    [SCRIPTED_EMOJI]: "runs a MoonLive script",
    [COMPILED_EMOJI]: "compiled into the firmware",
    // the shape it works in
    "\u{1F4CF}": "1D: a line",
    "\u{1F7E6}": "2D: a picture",
    "\u{1F9CA}": "3D: a volume",
    // where it came from
    "\u{1F4AB}": "MoonLight",
    "\u{1F319}": "MoonModules",
    "\u{1F419}": "WLED",
    "\u26A1\uFE0F": "FastLED",
    "\u{1F985}": "a named contributor",
    // what it listens to and what it does
    "\u{1F3B5}": "reacts to volume: how loud the room is",
    "\u{1F3B6}": "reacts to frequency: which notes are playing",
    "\u{1F4E1}": "takes its picture from the network",
    "\u{1F3AF}": "aims moving heads",
    "\u{1F47E}": "pixel art: games and sprites",
    "\u{1F9EC}": "a simulation: cells evolving off their own last frame",
    "\u{1F4F9}": "motion-tracking aware",
    // the power functions, shown together at the end of a row
    "\u{1F58C}\uFE0F": "power function: a shader, every pixel computed from its position",
    "\u2728": "power function: particles, born, moved by forces, and dying",
    "\u{1F30A}": "power function: a fluid working out its own motion",
    "\u{1F4A8}": "power function: transport, light carried and fading rather than redrawn",
    "\u{1F32B}\uFE0F": "power function: a noise field, the cloud and smoke family",
    "\u{1F3A1}": "power function: polar, composed around a center",
};

function emojiTagsFor(t) {
    const out = [];
    const kernels = [];
    const seen = new Set();
    const push = (ch) => {
        if (!ch || seen.has(ch)) return;
        seen.add(ch);
        (KERNEL_EMOJI.includes(ch) ? kernels : out).push(ch);
    };
    push(ROLE_EMOJI[t.role]);
    push(DIM_EMOJI[t.dim]);
    for (const ch of graphemes(t.tags || "")) push(ch);
    // Kernel chips in a fixed order, so two effects on the same kernels show the same run.
    kernels.sort((a, b) => KERNEL_EMOJI.indexOf(a) - KERNEL_EMOJI.indexOf(b));
    return out.concat(kernels);
}

// The type picker serves two modes:
//  - add (default): pick a type to create as a child of parentMod.
//  - replace: pick a type to swap parentMod for, at the same position.
// They differ only in the role filter and the commit action; the search box,
// list, and keyboard nav are shared.
/// The MoonLive module type that runs a script of this role.
///
/// Role, not extension: the picker knows what it is offering, and the catalog is already grouped
/// the same way. Null for a role with no scripted form.
function mlTypeForRole(role) {
    return role === "effect" ? "MoonLiveEffect"
         : role === "layout" ? "MoonLiveLayout"
         : role === "modifier" ? "MoonLiveModifier"
         : role === "service" ? "MoonLiveService" : null;
}

/// Every shipped script, as picker rows, for the roles a parent accepts.
///
/// One row per script rather than one "MoonLive" row: to a user, `dot` is a thing to add exactly
/// as `DemoReel` is, and which of the two is compiled is a property, not a category. The scripted
/// marker rides on the row so the merged list still says which is which, and `script` carries the
/// file the row would load.
async function mlScriptItems(roles) {
    const cat = await mlFetchCatalog().catch(() => null);
    if (!cat) return [];                       // no catalog: the picker still offers every type
    // What the device already holds, so a row can say when picking it costs a download. One listing
    // for every role: the scripts share a directory.
    const local = new Set((await fmFetchDir(cat.dir).catch(() => [])).map(e => e.name));
    const out = [];
    for (const role of roles) {
        if (!mlTypeForRole(role)) continue;
        const g = cat[role + "s"] || {};       // "effect" -> catalog group "effects"
        (g.names || []).forEach((n, i) => {
            const tags = (g.tags && g.tags[i]) || "";
            const isRemote = !local.has(n);
            out.push({
                name: n,
                remote: isRemote,
                // Without the extension: it is the file's business, not the reader's, and it keeps
                // the row sorting next to the compiled modules rather than in a block of ".mle".
                displayName: (isRemote ? "\u2601 " : "") + n.replace(/\.ml[elmsp]$/i, ""),
                role,
                // The scripted marker is what makes the row's kind visible, so it is added rather
                // than assumed: a script whose own tags happen to omit it still reads correctly.
                tags: tags.includes(SCRIPTED_EMOJI) ? tags : SCRIPTED_EMOJI + tags,
                dim: (g.dim && g.dim[i]) || 0,
                script: n,
            });
        });
    }
    return out;
}

/// Make sure a picked script is ON the device, downloading it if it is only in the catalog.
///
/// Creating or replacing a module before the file exists leaves a card reporting "script not found",
/// so this runs first on both paths. Returns false when the download failed and the caller must not
/// proceed; it has already told the user why.
async function mlEnsureLocal(item) {
    if (!item.remote) return true;
    try {
        await mlDownloadScript(item.script, item.role + "s");
        return true;
    } catch (e) {
        alert("could not download " + item.script + ": " + (e && e.message ? e.message : e));
        return false;
    }
}

/// Add a module for a picked row, whether it is a compiled type or a script.
///
/// A script becomes a MoonLive module holding it, named after the script: the user picked `dot`, so
/// the card says `dot`. `id` is how POST /api/modules names a module, and it is deliberately
/// idempotent (an existing name is success, not a rename), so a collision is retried with a suffix
/// rather than silently landing on the module already there.
/// Point a card at a script, and make sure the card catches up.
///
/// The re-render is the point. A card is built BEFORE its script is set (created or replaced first,
/// pointed at the file second), so its picker reads "(none)" and its editor is blank until something
/// rebuilds it. A script that COMPILES hides this: defining controls changes the module's schema,
/// which fires a full resync. A script that FAILS defines nothing, the schema signature is unchanged,
/// and only the status text arrives, leaving a card that contradicts itself: an error about a script
/// it claims not to have, correct again after a manual refresh.
async function setCardScript(moduleName, script) {
    await sendControl(moduleName, "script", script);
    await refetchState();
}

async function addPickedType(item, parentName) {
    if (!item.script) return addModule(item.name, parentName);
    const type = mlTypeForRole(item.role);
    if (!type) return;
    if (!await mlEnsureLocal(item)) return;
    // The cloud marker is a property of the row, not of the name the card takes.
    const base = item.displayName.replace(/^\u2601\s*/, "");
    for (let n = 1; n <= 20; n++) {
        const id = n === 1 ? base : `${base}-${n}`;
        const created = await addModule(type, parentName, id);
        if (created) {
            await setCardScript(created, item.script);
            return created;
        }
    }
    // Twenty siblings of one name, all taken. Unlikely, but silence would leave the user clicking
    // create and watching nothing happen, which is the one outcome worse than an error.
    alert(`could not add ${base}: twenty modules of that name already exist here`);
}

function openTypePicker(parentMod, anchorEl) {
    const roles = rolesAcceptedBy(parentMod);
    // One candidate = no choice to make, so don't stage a picker to ask a question with one answer:
    // "+" on Effects just adds a Layer. (Same filter openPicker uses, so the two can't disagree about
    // what the candidates are.) Counted over the TYPES alone: the scripts arrive asynchronously, and
    // a role with one type but many scripts is still a real choice, which the await below sees.
    const candidates = availableTypes.filter(t => roles.includes(t.role));
    mlScriptItems(roles).then((scripts) => {
        if (candidates.length === 1 && !scripts.length) {
            addModule(candidates[0].name, parentMod.name);
            return;
        }
        openPicker(anchorEl, {
            items: [...candidates, ...scripts],
            actionLabel: "create",
            commit: (name, item) => addPickedType(item, parentMod.name),
        });
    });
}

// Replace mode: filter to the target module's own role (effect ↔ effect), and
// pre-select the module's CURRENT type so the cursor lands on it (not the first row).
function openReplacePicker(targetMod, anchorEl) {
    const roles = [targetMod.role];
    // Scripts here for the same reason they are in the add picker: swapping an effect for a script
    // is the same question as adding one, and a list that offers scripts in one place and not the
    // other makes the distinction visible again exactly where it should not be.
    mlScriptItems(roles).then((scripts) => {
        openPicker(anchorEl, {
            items: [...availableTypes.filter(t => roles.includes(t.role)), ...scripts],
            actionLabel: "replace",
            currentType: targetMod.type,
            commit: (name, item) => replacePickedType(targetMod, item),
        });
    });
}

/// Replace a module with a picked row, whether it is a compiled type or a script.
///
/// A script replaces in place: the module becomes the MoonLive type for its role and then loads the
/// file, so the slot keeps its position in the layer. It is also RENAMED, like the add path: a card
/// is named after what it runs, so swapping what it runs renames it.
async function replacePickedType(targetMod, item) {
    if (!item) return;
    // A replace ALWAYS renames: a card is named after what it runs, so swapping what it runs renames
    // it. Simple and predictable, and it is why the device's replace takes a name at all: left to
    // itself it preserves any name that is not the old type's default, which kept a card auto-named
    // "MoonLive-3" labeled that after it became a Lissajous.
    const picked = item.displayName.replace(/^\u2601\s*/, "");
    if (!item.script) return replaceModule(targetMod.name, item.name, picked);
    const type = mlTypeForRole(item.role);
    if (!type) return;
    if (!await mlEnsureLocal(item)) return;
    // Named after the script, the same as adding one: a card running dot.mle is called `dot` however
    // it got there, unless the user named it something of their own. The device disambiguates a
    // collision, so the name asked for is not always the name given.
    // WHERE the slot sits, captured BEFORE the replace. Afterwards the module answers to its new
    // name and the old one is gone, so nothing can be found by it: the position is the only handle
    // that survives. Looking the parent up afterwards found nothing, and the name asked for could
    // match a DIFFERENT card when it was already taken, which sent the script to the wrong module.
    const parent = findParentOf(targetMod.name);
    const index = parent ? (parent.children || []).findIndex(k => k.name === targetMod.name) : -1;
    const parentName = parent ? parent.name : null;

    await replaceModule(targetMod.name, type, picked);

    // The slot at that position, in the refreshed state. A replace swaps a child IN PLACE, so the
    // index is stable and the parent keeps its own name; the device may have suffixed the name it
    // was given ("dot-2") to keep it unique, which is exactly what the position answers.
    const after = parentName ? findModule(parentName) : null;
    const atIndex = after && index >= 0 && after.children ? after.children[index] : null;
    const slot = atIndex ? atIndex.name : (findModule(picked) ? picked : null);
    if (slot) await setCardScript(slot, item.script);
}

function openPicker(anchorEl, opts) {
    // Close any existing picker
    // Its MODAL, not just the inner block: removing only the picker would leave an open
    // dialog with an empty backdrop over the page.
    document.querySelectorAll(".type-picker-modal").forEach(d => d.remove());
    document.querySelectorAll(".type-picker").forEach(p => p.remove());

    // The items are the CALLER's, defaulting to the registered types filtered by role. Everything
    // below works on {name, displayName, role, tags, dim}, so anything describing itself that way
    // gets this widget: the MoonLive script picker passes its scripts and inherits the search box,
    // the emoji chip filter, the keyboard handling and the row layout rather than growing its own.
    //
    // Sorting stays here so every picker is ordered the same way, and stays alphabetical by display
    // name so the list is scannable regardless of registration order (localeCompare:
    // case-insensitive, locale-aware).
    const source = opts.items || availableTypes.filter(t => opts.roles.includes(t.role));
    // Sorted on the NAME, not on any marker in front of it: a row prefixed with the cloud glyph is
    // still that script alphabetically, and prefixed rows would otherwise collect in a block of
    // their own instead of sitting where the reader looks for them.
    const sortKey = (t) => (t.displayName || t.name).replace(/^[^\p{L}\p{N}]+/u, "");
    // `keepOrder` leaves the caller's order alone. A PALETTE is chosen by INDEX, and that index is
    // what the control surface's knob steps through and what Home Assistant shows, so an
    // alphabetical picker would disagree with both: turning the knob would jump around the list a
    // user is reading. Where the order carries no meaning (module types, scripts) the alphabetical
    // sort stands, because there a name is the only thing to look something up by.
    const filtered = opts.keepOrder
        ? [...source]
        : [...source].sort((a, b) => sortKey(a).localeCompare(sortKey(b)));

    const picker = document.createElement("div");
    picker.className = "type-picker";
    // Dismiss: closes the modal when there is one (which removes it), and falls back to removing
    // the picker itself. One function so every exit path (cancel, Enter, double-click, commit)
    // dismisses the same way.
    const closePicker = () => {
        const dlg = picker.closest("dialog");
        if (dlg) dlg.close(); else picker.remove();
    };

    const search = document.createElement("input");
    search.type = "text";
    search.placeholder = "search…";
    search.className = "type-picker-search";
    picker.appendChild(search);

    // Emoji chip filter row: every distinct emoji across the role-filtered types.
    // Toggling chips narrows the list (AND: a type must carry all active chips).
    const activeChips = new Set();
    const chipRow = document.createElement("div");
    chipRow.className = "type-picker-chips";
    const chipSeen = new Set();
    const present = [];
    for (const t of filtered) {
        for (const ch of emojiTagsFor(t)) {
            if (!chipSeen.has(ch)) { chipSeen.add(ch); present.push(ch); }
        }
    }
    // The compiled chip is not a tag anything carries, so it cannot be discovered from the rows the
    // way every other chip is: it is offered when the list actually holds both kinds, which is the
    // only situation where filtering by kind means anything.
    if (chipSeen.has(SCRIPTED_EMOJI) && filtered.some(t => !emojiTagsFor(t).includes(SCRIPTED_EMOJI))) {
        present.push(COMPILED_EMOJI);
    }
    // Grouped rather than in first-seen order, so the row reads as the legend does: the scripted
    // marker, then what a module IS (role, then dimension), then where it came from, then what it
    // can do. A chip whose category is unknown falls in the last group rather than vanishing, so a
    // new emoji is visible before anyone remembers to classify it.
    const CHIP_GROUPS = [
        [SCRIPTED_EMOJI, COMPILED_EMOJI],                // what a row IS: scripted or compiled
        Object.values(ROLE_EMOJI),                       // type
        Object.values(DIM_EMOJI),                        // dimension
        ["\u{1F4AB}", "\u{1F319}", "\u{1F419}", "\u26A1\uFE0F"],   // origin
        AUDIO_EMOJI,                                     // what it listens to
        KERNEL_EMOJI,                                    // which power functions it is built on
    ];
    const groups = CHIP_GROUPS.map(g => present.filter(e => g.includes(e)));
    const classified = new Set(CHIP_GROUPS.flat());
    groups.push(present.filter(e => !classified.has(e)));   // capabilities and anything new

    let first = true;
    for (const group of groups) {
        if (!group.length) continue;
        // A separator between groups, never leading or trailing: it marks a boundary, and a
        // boundary with nothing on one side is just a mark.
        if (!first) {
            const sep = document.createElement("span");
            sep.className = "type-picker-chip-sep";
            sep.setAttribute("aria-hidden", "true");
            chipRow.appendChild(sep);
        }
        first = false;
        for (const emoji of group) {
            const chip = document.createElement("button");
            chip.className = "type-picker-chip";
            chip.textContent = emoji;
            const label = EMOJI_LABEL[emoji];
            if (label) { chip.title = label; chip.setAttribute("aria-label", label); }
            chip.addEventListener("click", () => {
                if (activeChips.has(emoji)) { activeChips.delete(emoji); chip.classList.remove("active"); }
                else { activeChips.add(emoji); chip.classList.add("active"); }
                refresh();
            });
            chipRow.appendChild(chip);
        }
    }
    if (present.length > 0) picker.appendChild(chipRow);

    const list = document.createElement("div");
    list.className = "type-picker-list";
    picker.appendChild(list);

    const actions = document.createElement("div");
    actions.className = "type-picker-actions";
    const cancelBtn = document.createElement("button");
    cancelBtn.textContent = "cancel";
    cancelBtn.addEventListener("click", () => closePicker());
    const createBtn = document.createElement("button");
    createBtn.className = "create";
    createBtn.textContent = opts.actionLabel;
    createBtn.disabled = true;
    actions.appendChild(cancelBtn);
    actions.appendChild(createBtn);
    picker.appendChild(actions);

    let selectedType = null;
    let selectedItem = null;      // the row itself: commit needs more than its name

    // Types matching the search box AND all active emoji chips. The query matches
    // against both the raw typeName ("RainbowEffect") and the displayName
    // ("Rainbow") supplied by /api/types so typing either form finds the row.
    function currentMatches() {
        const q = search.value.toLowerCase();
        return filtered.filter(t => {
            if (q) {
                const raw = t.name.toLowerCase();
                const disp = (t.displayName || t.name).toLowerCase();
                if (!raw.includes(q) && !disp.includes(q)) return false;
            }
            if (activeChips.size > 0) {
                const has = new Set(emojiTagsFor(t));
                for (const chip of activeChips) {
                    // The compiled chip matches what carries NO scripted marker, since a compiled
                    // module has no tag of its own: the two kind chips are each other's opposite.
                    const ok = chip === COMPILED_EMOJI ? !has.has(SCRIPTED_EMOJI) : has.has(chip);
                    if (!ok) return false;
                }
            }
            return true;
        });
    }

    function refresh() {
        const matches = currentMatches();
        list.innerHTML = "";
        // Highlight the module's current type if it's in the list (replace mode lands the cursor
        // on what's already there); otherwise the first row.
        let selIdx = opts.currentType ? matches.findIndex(t => t.name === opts.currentType) : -1;
        if (selIdx < 0) selIdx = 0;
        matches.forEach((t, i) => {
            const item = document.createElement("div");
            item.className = "type-picker-item" + (i === selIdx ? " selected" : "");
            // An optional COLOR SWATCH, for a list whose rows are colors rather than modules. Only
            // palettes carry `colors`, so every other picker is untouched: the column simply is not
            // built. Same gradient the native palette dropdown paints, so a palette looks the same
            // whichever way it is chosen.
            // `role: "palette"` builds the column, not `colors`: a scripted palette that is not
            // downloaded yet HAS no gradient (it produces one only once it runs), and skipping the
            // swatch for those rows would leave their names hanging in the emoji column.
            if (t.colors || t.role === "palette") {
                const sw = document.createElement("span");
                sw.className = "type-picker-item-swatch"
                             + (t.colors ? "" : " type-picker-item-swatch-empty");
                if (t.colors) sw.style.background = paletteGradientCss(t.colors);
                item.appendChild(sw);
            }
            const emoji = document.createElement("span");
            emoji.className = "type-picker-item-emoji";
            renderEmojiTags(emoji, emojiTagsFor(t));
            item.appendChild(emoji);
            // Show the factory-stripped name ("Rainbow") not the typeName
            // ("RainbowEffect"); the role text on the right already conveys
            // "effect", so repeating it in the name would just be noise.
            item.appendChild(document.createTextNode(t.displayName || t.name));
            const role = document.createElement("span");
            role.className = "role";
            role.textContent = t.role;
            item.appendChild(role);
            item.addEventListener("click", () => {
                list.querySelectorAll(".selected").forEach(x => x.classList.remove("selected"));
                item.classList.add("selected");
                selectedType = t.name;
                selectedItem = t;
                createBtn.disabled = false;
            });
            item.addEventListener("dblclick", () => {
                opts.commit(t.name, t);
                closePicker();
            });
            list.appendChild(item);
        });
        selectedItem = matches.length > 0 ? matches[selIdx] : null;
        selectedType = selectedItem ? selectedItem.name : null;
        createBtn.disabled = !selectedType;
        // Scroll the pre-selected row into view (it may be below the fold for a long list).
        const selEl = list.querySelector(".type-picker-item.selected");
        if (selEl) selEl.scrollIntoView({ block: "nearest" });
    }

    search.addEventListener("input", refresh);
    search.addEventListener("keydown", (e) => {
        const items = Array.from(list.querySelectorAll(".type-picker-item"));
        const sel = list.querySelector(".selected");
        const idx = items.indexOf(sel);
        if (e.key === "ArrowDown") {
            e.preventDefault();
            if (idx < items.length - 1) {
                sel?.classList.remove("selected");
                items[idx + 1].classList.add("selected");
                selectedItem = filteredAt(idx + 1);
                selectedType = selectedItem?.name;
            }
        } else if (e.key === "ArrowUp") {
            e.preventDefault();
            if (idx > 0) {
                sel?.classList.remove("selected");
                items[idx - 1].classList.add("selected");
                selectedItem = filteredAt(idx - 1);
                selectedType = selectedItem?.name;
            }
        } else if (e.key === "Enter") {
            e.preventDefault();
            if (selectedType) {
                opts.commit(selectedType, selectedItem);
                closePicker();
            }
        } else if (e.key === "Escape") {
            closePicker();
        }
    });

    function filteredAt(i) {
        return currentMatches()[i];
    }

    createBtn.addEventListener("click", () => {
        if (selectedType) {
            opts.commit(selectedType, selectedItem);
            closePicker();
        }
    });

    // A MODAL, not a block appended to whatever opened it. Appending made the picker inherit its
    // anchor's width and position: from a card footer it drew at the bottom of the card, far from
    // the field being changed, and from a toolbar button it collapsed to an unreadable sliver.
    // A dialog sits above the page at its own size wherever it is opened from.
    //
    // The native <dialog>, the same one the File Manager's editor uses: Esc and the backdrop are
    // the browser's job, so there is no overlay, no focus trap and no scroll lock to maintain here.
    // Where the anchor sits BEFORE the modal opens: showModal() can move the page under it (the
    // body's scrollbar goes), so a rect read afterwards describes a layout that has already shifted.
    const anchorRect = anchorEl && anchorEl.getBoundingClientRect
        ? anchorEl.getBoundingClientRect() : null;

    const dlg = document.createElement("dialog");
    dlg.className = "type-picker-modal";
    dlg.appendChild(picker);
    document.body.appendChild(dlg);
    // Esc closes it (the browser fires `close`), and so does clicking the backdrop: the dialog
    // element itself is the backdrop, so a click that lands on it rather than on the picker inside
    // is a click outside.
    dlg.addEventListener("close", () => dlg.remove());
    dlg.addEventListener("click", (e) => { if (e.target === dlg) dlg.close(); });
    dlg.showModal();
    refresh();

    // Centered over the CARDS column, not the viewport. showModal() centers on the page, which puts
    // the list far from the card whose control opened it: the eye is on the right-hand column and
    // the answer appears in the middle of the preview. Falls back to the page center when the
    // column is absent (the PiP layout, where cards are full width anyway).
    //
    // AFTER refresh(): the rows are what give the picker its height, so measuring before them read
    // an empty list (111px against a real 311px) and the bottom-of-screen clamp never fired.
    const col = document.getElementById("main");
    const d = dlg.getBoundingClientRect();
    const c = col ? col.getBoundingClientRect() : null;
    if (c && c.width > d.width) {
        // VERTICALLY the search box lands on what was clicked, so the list opens under the hand
        // rather than jumping the eye across the screen. Clamped upward when the picker would run
        // off the bottom, and never above the top edge: on a short window it simply starts at the
        // top and the list scrolls, which beats a dialog with its buttons out of reach.
        const margin = 8;
        // MEASURE FIRST, then write: reading a rect after setting `left`/`margin` reads a box that
        // has already moved, and using that as an offset walks the dialog down the page.
        const s = picker.querySelector(".type-picker-search");
        const inset = s ? s.getBoundingClientRect().top - d.top : 0;   // dialog top to search box
        // The search box lands just BELOW the control that opened it, so the thing clicked stays
        // visible above the picker rather than being covered by it.
        //
        // Clamped so the whole picker stays on screen: it rises rather than hanging off the bottom.
        // On a window too short to hold it at all, `max` wins over `min` and it starts at the top
        // margin with the list scrolling, which beats putting the buttons out of reach.
        dlg.style.position = "fixed";
        dlg.style.left = Math.round(c.left + (c.width - d.width) / 2) + "px";
        dlg.style.margin = "0";
        // Clamp against the height the dialog has ONCE POSITIONED. `d` was measured while it was
        // still centered by showModal(), and a fixed dialog lays out to a different height, so
        // clamping against the stale number let it hang off the bottom of a short window.
        const h = dlg.getBoundingClientRect().height;
        let top = (anchorRect ? anchorRect.bottom + margin : d.top) - inset;
        top = Math.min(top, window.innerHeight - h - margin);
        top = Math.max(margin, top);
        dlg.style.top = Math.round(top) + "px";
    }
    search.focus();
}

// ---------------------------------------------------------------------------
// 7. Drag-to-reorder (HTML5 DnD on desktop; touchstart-gated on mobile)
// ---------------------------------------------------------------------------

function attachDragHandlers(card, mod) {
    card.draggable = true;

    // Why we toggle `draggable` on mousedown instead of vetoing in dragstart:
    // HTML5 dragstart's `e.target` is always the draggable element (the card),
    // not the deepest element under the mouse: so closest(".control-row")
    // never matches. The reliable signal is the *mousedown* target. Disable
    // drag at mousedown when the grab landed on a control, re-enable on
    // mouseup so the next click on the card body can still drag.
    const gate = (e) => {
        card.draggable = !e.target.closest(".control-row, .card-controls-collapse > summary");
    };
    card.addEventListener("mousedown", gate, true);   // capture: runs before the input
    card.addEventListener("touchstart", gate, {capture: true, passive: true});

    card.addEventListener("dragstart", (e) => {
        // Innermost card wins: without stopPropagation a nested child's
        // dragstart would bubble to the parent and the parent's listener
        // would overwrite dataTransfer with its own name.
        e.stopPropagation();
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", mod.name);
        card.classList.add("dragging");
    });
    card.addEventListener("dragend", () => {
        card.classList.remove("dragging");
        document.querySelectorAll(".drag-over").forEach(c => c.classList.remove("drag-over"));
    });
    card.addEventListener("dragover", (e) => {
        // Only allow drop on a true sibling: same .card-children container.
        // Cards now nest, so equal data-depth is no longer enough: two effects
        // under different Effects share a depth but aren't siblings.
        const src = document.querySelector(".card.dragging");
        if (!src || src === card) return;
        if (src.parentElement === card.parentElement &&
            card.parentElement &&
            card.parentElement.classList.contains("card-children")) {
            e.preventDefault();
            card.classList.add("drag-over");
        }
    });
    card.addEventListener("dragleave", () => {
        card.classList.remove("drag-over");
    });
    card.addEventListener("drop", (e) => {
        e.preventDefault();
        // Innermost card wins: without stopPropagation the drop bubbles to every
        // ancestor card that also has a drop handler, firing a SECOND move onto
        // the grandparent's child list (e.g. dropping onto Mirror also dropped
        // onto the Layer card → move into Effects, index 0 → undoing the first
        // move). Same reason dragstart stops propagation above.
        e.stopPropagation();
        card.classList.remove("drag-over");
        const srcName = e.dataTransfer.getData("text/plain");
        if (!srcName || srcName === mod.name) return;
        // Insert semantics (not swap): the dropped item takes the target row's
        // slot and the others shift to fill: the standard reorderable-list
        // behavior (Finder, Trello, VS Code, SortableJS). Because we drop ONTO a
        // row (not into a between-rows gap), the landing is the target's absolute
        // index: dragging down lands after the target, dragging up lands before
        // it. That's consistent ("take the target's slot"), just not always-before.
        //
        // Compute target absolute index within parent's children. Identify the
        // drop-target by name, not by object identity: state is replaced on
        // every WS push, so `mod` captured at render time is stale within ~1s.
        const parent = findParent(mod.name);
        if (!parent) return;
        const targetIdx = (parent.children || []).findIndex(c => c.name === mod.name);
        if (targetIdx < 0) return;
        moveModuleTo(srcName, targetIdx);
    });
}

// ---------------------------------------------------------------------------
// 8. Status bar wiring
// ---------------------------------------------------------------------------

function setupStatusBarButtons() {
    document.getElementById("preview-reset")?.addEventListener("click", () => {
        preview.resetCamera();
    });

    // Reboot: press once to arm, again to confirm: see armPressTwice. The glyph
    // stays (no armedText); only the title changes.
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn) {
        armPressTwice(rebootBtn, rebootDevice, {armedTitle: "Click again to reboot"});
    }
    document.getElementById("theme-toggle")?.addEventListener("click", () => {
        theme = (theme === "dark") ? "light" : "dark";
        localStorage.setItem(LS_THEME, theme);
        applyTheme(theme);
        // Repaint the preview to the new theme's background: a live preview would
        // pick it up on its next frame, but an idle one (no incoming frames) needs
        // a nudge so the canvas doesn't keep the previous theme's clear color.
        preview.redraw();
    });

    // Hamburger: toggles the side nav. On wide screens it collapses/expands the
    // static column; on narrow screens (<820px) the same class drives a slide-in
    // drawer over an overlay (CSS handles the responsive difference).
    document.getElementById("nav-toggle")?.addEventListener("click", () => {
        document.body.classList.toggle("nav-open");
    });
    document.getElementById("nav-overlay")?.addEventListener("click", closeNavDrawer);
    document.addEventListener("keydown", (e) => {
        if (e.key === "Escape") closeNavDrawer();
    });
}

// Close the side nav. On wide screens this collapses the column; on narrow
// screens it dismisses the slide-in drawer + overlay.
function closeNavDrawer() {
    document.body.classList.remove("nav-open");
}

function applyTheme(t) {
    document.body.dataset.theme = t;
    const btn = document.getElementById("theme-toggle");
    if (btn) btn.textContent = (t === "dark") ? "☀" : "🌙";
}

function updateStatusBar() {
    if (!state || !state.modules) return;
    const sys = state.modules.find(m => m.type === "SystemModule")
             || state.modules.find(m => m.name === "System");
    if (!sys) return;
    const ctrls = sys.controls || [];

    // Device name → header span + document.title
    const nameCtrl = ctrls.find(c => c.name === "deviceName");
    const nameSpan = document.getElementById("device-name");
    if (nameCtrl && nameSpan && nameCtrl.value) {
        setText(nameSpan, String(nameCtrl.value ?? ""));
        if (document.title !== "MoonLight: " + nameCtrl.value) {
            document.title = "MoonLight: " + nameCtrl.value;
        }
    }

    // System stats: "uptime · 🧠 NNK · 🧱 NNKB"
    // 🧠 = internal-RAM free (heap progress total−used); 🧱 = largest contiguous
    // internal-RAM block (the maxBlock control, already maxInternalAllocBlock).
    // Both matter: free can be ample while fragmentation leaves no single block
    // big enough for the next allocation.
    const uptimeCtrl = ctrls.find(c => c.name === "uptime");
    const heapCtrl = ctrls.find(c => c.name === "heap");
    const blockCtrl = ctrls.find(c => c.name === "maxBlock");
    const fpsCtrl = ctrls.find(c => c.name === "fps");
    const statsEl = document.getElementById("sys-stats");
    if (statsEl) {
        const parts = [];
        if (uptimeCtrl) parts.push(uptimeCtrl.value);
        // The WHOLE pipeline's rate, which is the one place fps is the honest unit: the device
        // really does complete this many frames a second, where a per-module fps is one part's
        // cost inverted. Before the memory badges because it is the number that moves.
        if (fpsCtrl && fpsCtrl.value) {
            const f = Number(fpsCtrl.value);
            parts.push("🕒 " + (f >= 1000 ? Math.round(f / 1000) + "K fps" : f + " fps"));
        }
        if (heapCtrl && heapCtrl.value !== undefined && heapCtrl.total) {
            const freeKb = Math.round((heapCtrl.total - heapCtrl.value) / 1024);
            parts.push("🧠 " + freeKb + "K");
        }
        // Skip on desktop, where the platform stub reports "0KB" (no real
        // block measurement / unlimited heap): same reason heap free above
        // only shows when the heap progress control is present.
        if (blockCtrl && blockCtrl.value && blockCtrl.value !== "0KB") {
            parts.push("🧱 " + blockCtrl.value);
        }
        setText(statsEl, parts.join(" · "));
    }

    // Hide reboot button on desktop builds: platform::reboot() just exits the process,
    // which is not useful from the UI and can be mistaken for a crash.
    const chipCtrl = ctrls.find(c => c.name === "chip");
    const rebootBtn = document.getElementById("reboot-btn");
    if (rebootBtn) {
        // Hidden on a desktop, where `reboot()` EXITS THE PROCESS and nothing restarts it. Keyed on
        // `deviceModel`, which the desktop platform seeds with its own name (macos-arm64,
        // linux-x64, windows-x64, docker) and a board leaves for tooling to fill from the catalog.
        //
        // NOT on `chip`: that used to read "desktop" and now reads the real architecture (arm64,
        // x64), so the old test silently stopped matching and put a process-killing button on every
        // desktop card.
        const modelCtrl = ctrls.find(c => c.name === "deviceModel");
        const model = String(modelCtrl?.value ?? "");
        rebootBtn.hidden = /^(macos|linux|windows)-|^docker$/.test(model);
    }

    // bootReason → crashed-state styling on reboot button
    const reasonCtrl = ctrls.find(c => c.name === "bootReason");
    if (rebootBtn && reasonCtrl) {
        const crashed = ["PANIC", "INT_WDT", "TASK_WDT", "BROWNOUT"].includes(reasonCtrl.value);
        rebootBtn.dataset.crashed = crashed ? "true" : "false";
        if (crashed) rebootBtn.title = "Last boot: " + reasonCtrl.value + " (click to reboot)";
        else rebootBtn.title = "Reboot device";
    }

    // Cache-first update check: instant from the localStorage cache, background-fetches only
    // when stale (>1 h). Fire-and-forget: best-effort, never blocks the status-bar render.
    checkFirmwareUpdate(false);
}

// ---------------------------------------------------------------------------
// 8b. Firmware-update badge
// ---------------------------------------------------------------------------
// Browser-side "a newer firmware is out" check, modelled on ESP32-sveltekit's
// UpdateIndicator (the upstream firmware lineage): our own code. The device fetches
// nothing; the browser compares the running version (FirmwareUpdateModule.version, pure
// semver) against GitHub releases and, when newer AND a compatible .bin exists, shows the
// status-bar badge. Two channels:
//   - STABLE: a device compares against the newest stable release (the /latest endpoint
//     excludes prereleases). Applies to every device.
//   - DEV (latest): a device already on a prerelease build (-dev.<N>) ALSO compares against
//     the moving `latest` release, so a stale latest build is nudged to the newest latest.
//     The `latest` release's tag is "latest" (not a semver), so its version is read from the
//     per-firmware manifest (manifest-<firmware>.json carries "version", e.g. 2.1.0-dev.7).
// A stable update wins over a dev update. Cached in localStorage (1 h TTL) so it doesn't
// slow page load; a fresh check is forced when the Firmware card opens. Best-effort: any
// failure hides the badge, never throws.

const RELEASES_API = "https://api.github.com/repos/MoonModules/projectMM/releases";
const UPDATE_TTL_MS = 60 * 60 * 1000;                     // 1 h: best-effort, well under GitHub's rate limit
const PICKER_RELEASE_KEY = "MoonLight.picker.releaseTag"; // install-picker restores from this on init

function safeLocalGet(key) { try { return localStorage.getItem(key); } catch (_) { return null; } }
function safeLocalSet(key, v) { try { localStorage.setItem(key, v); } catch (_) { /* ignore */ } }

// In-flight fetches keyed by cache slot. updateStatusBar() calls checkFirmwareUpdate(false)
// every WS tick (1 Hz); on a cold cache they'd each start a duplicate releases/latest request
// before the first writes the cache. Share the pending promise so concurrent callers reuse it.
const inFlightFetches = {};

// A cached JSON fetch: returns the parsed body, re-fetching only when the cache is older than
// the TTL or `force` is set, and serving stale on a fetch failure. `key` is the cache slot.
async function cachedJson(url, key, force) {
    if (!force) {
        const raw = safeLocalGet(key);
        if (raw) {
            try {
                const obj = JSON.parse(raw);
                if (Date.now() - obj.ts < UPDATE_TTL_MS) return obj.data;
            } catch (_) { /* fall through to fetch */ }
        }
    }
    // Coalesce concurrent fetches for the same slot onto one request.
    if (inFlightFetches[key]) return inFlightFetches[key];
    const p = (async () => {
        try {
            const res = await fetch(url, { headers: { accept: "application/json" } });
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            safeLocalSet(key, JSON.stringify({ ts: Date.now(), data }));
            return data;
        } catch (e) {
            // console.debug, not warn: an update check failing is routine and not
            // actionable (the device may simply be offline, or GitHub rate-limited),
            // so keep it out of the default console: debug is hidden unless the user
            // opts into verbose. Both callers hit api.github.com, which sends
            // Access-Control-Allow-Origin and so reads fine from the device origin;
            // the failure path here is for the no-network / rate-limit case.
            console.debug("[update] fetch failed:", url, e && e.message ? e.message : e);
            const raw = safeLocalGet(key);                   // serve stale on failure
            if (raw) {
                try {
                    const obj = JSON.parse(raw);
                    // Refresh the timestamp so the per-tick check doesn't re-attempt a
                    // failing fetch every second: back off until the next TTL window.
                    safeLocalSet(key, JSON.stringify({ ts: Date.now(), data: obj.data }));
                    return obj.data;
                } catch (_) { /* none */ }
            }
            // No stale entry to serve: NEGATIVE-CACHE the failure (data:null) with a
            // fresh timestamp so the TTL guard above suppresses the next attempt for
            // the back-off window. Without this, every status-bar render (≈4×/s on
            // each WS push) re-runs the failing fetch: an error storm in the console
            // whenever the device is offline. A null cache hit returns "no update".
            safeLocalSet(key, JSON.stringify({ ts: Date.now(), data: null }));
            return null;
        } finally {
            delete inFlightFetches[key];                     // clear once settled, ok or not
        }
    })();
    inFlightFetches[key] = p;
    return p;
}

// Which release asset would UPDATE this machine? An ESP32 installs a firmware-<variant>-<ver>.bin
// through the Firmware card; a desktop downloads an archive and replaces its own binary by hand.
// Both still want to be TOLD there is a newer version, which is what the badge is for.
//
// A desktop reports firmware "unknown" (there is no variant to name), which is how we tell them
// apart. Before this, that value was truthy and the check looked for a firmware-unknown-*.bin that
// no release has ever contained, so a desktop user was never told anything.
function desktopAssetPrefix() {
    const p = (navigator.platform || "") + " " + (navigator.userAgent || "");
    if (/Win/i.test(p)) return "projectMM-windows-x64-v";
    if (/Mac/i.test(p)) return "projectMM-macos-arm64-v";
    if (/Linux|X11/i.test(p)) return "projectMM-linux-x64-v";
    return null;                                   // an OS we do not package: say nothing
}

// Read the device's running version + firmware-variant key off the FirmwareUpdateModule.
function deviceFirmwareInfo() {
    if (!state || !state.modules) return null;
    const fw = findModule("Firmware") || (state.modules.find(m => m.type === "FirmwareUpdateModule"));
    if (!fw) return null;
    const ctrls = fw.controls || [];
    // ONLY WHILE THE CARD DESCRIBES THE APP. `image` rebinds version/build/firmware/partition to
    // whichever partition is selected, so on the MoonBase tab these read MoonBase: the badge would
    // then compare a recovery image's version against app releases and look for a firmware asset
    // named after a chip. The badge is global chrome, so it would stay wrong for as long as the
    // user left that tab selected.
    const image = (ctrls.find(c => c.name === "image") || {}).value;
    if (image === 1) return null;
    const version = (ctrls.find(c => c.name === "version") || {}).value;
    const firmware = (ctrls.find(c => c.name === "firmware") || {}).value;
    // "unknown" is what a desktop build reports: no ESP32 variant to name.
    const isDesktop = !firmware || firmware === "unknown";
    return version ? { version, firmware: isDesktop ? null : firmware, isDesktop } : null;
}

// Light the badge for an available update. `tag` is the release the picker should pre-select
// (a vX.Y.Z stable tag, or "latest"); `label` is what the badge shows.
function showUpdateBadge(badge, tag, label, isDesktop) {
    // setText, not a raw write: this runs on every state patch, and rewriting identical text
    // still destroys a selection inside the element.
    setText(badge, `⬆ ${label}`);
    // A desktop cannot install from the Firmware card: it has no OTA, and updating means
    // downloading an archive and replacing the binary. So the badge sends it to the release page
    // instead of a card that would offer it nothing.
    badge.title = isDesktop
        ? `Update available: ${label}. Open the release page to download`
        : `Firmware update available: ${label}. Open Firmware to install`;
    badge.dataset.tag = tag;
    badge.dataset.desktop = isDesktop ? "1" : "";
    badge.hidden = false;
}

// The key that names a release asset, from the device's own `firmware` control. On the app tab
// that is already the variant ("esp32s3-n16r8"); on the MoonBase tab the device reports the chip
// ("ESP32-S3"), which the asset spells "esp32s3" after the build directory's IDF target.
//
// Null when the control has not arrived: the picker treats that as "offer everything" rather than
// as a key nothing matches, which is the difference between a full list and a silently empty one.
function moonbaseAssetKeyFrom(fw) {
    if (!fw) return null;
    return /^ESP32/.test(fw) ? fw.toLowerCase().replace(/-/g, "") : fw;
}


// Is there a newer STABLE release than the device's version, with a compatible .bin?
// Returns the stable tag (e.g. "v2.1.0") or null. /latest excludes prereleases.
async function stableUpdate(dev, force) {
    const rel = await cachedJson(`${RELEASES_API}/latest`, "MoonLight.update.latest.v1", force);
    if (!rel || !rel.tag_name) return null;
    const assetNames = (rel.assets || []).map(a => a.name);
    const prefix = dev.isDesktop ? desktopAssetPrefix() : null;
    const hasBinary = dev.isDesktop
        // A desktop is told about a release that actually ships a build for ITS OS: an archive
        // named for the platform, rather than a firmware .bin it could not install anyway.
        ? !!prefix && assetNames.some(n => n.startsWith(prefix))
        : !dev.firmware ||
          assetNames.some(n => n === `firmware-${dev.firmware}-${rel.tag_name}.bin`);
    return (isNewer(rel.tag_name, dev.version) && hasBinary) ? rel.tag_name : null;
}

// For a device already on a -dev build: is the moving `latest` release newer? Returns its
// version string (e.g. "2.1.0-dev.7") or null. The latest release's tag is "latest"; its
// version is published as the release `name` (release.yml), which the GitHub API exposes
// CORS-readably: unlike the manifest-*.json asset, whose release-asset URL redirects to
// release-assets.githubusercontent.com (no CORS header), so the device-hosted UI can't read it.
// We also require the matching firmware .bin asset so the badge never points at a build the
// device can't install.
async function devUpdate(dev, force) {
    if (!dev.firmware && !dev.isDesktop) return null;        // can't match an asset without the key
    const rel = await cachedJson(`${RELEASES_API}/tags/latest`, "MoonLight.update.dev.v1", force);
    const v = rel && rel.name;
    if (!v) return null;
    // Assets are versioned, not tagged: the `latest` release ships
    // firmware-<fw>-v<version>.bin (release.yml stages PREFIX="firmware-...-v$V").
    const prefix = dev.isDesktop ? desktopAssetPrefix() : null;
    const hasBinary = dev.isDesktop
        ? !!prefix && (rel.assets || []).some(a => a.name.startsWith(prefix))
        : (rel.assets || []).some(a => a.name === `firmware-${dev.firmware}-v${v}.bin`);
    return (hasBinary && isNewer(v, dev.version)) ? v : null;
}

// Show/hide the badge. `force` bypasses the cache (used when the Firmware card opens).
// Stable update takes precedence; a -dev device additionally checks the latest channel.
async function checkFirmwareUpdate(force) {
    const badge = document.getElementById("fw-update-badge");
    if (!badge) return;
    const dev = deviceFirmwareInfo();
    if (!dev) { badge.hidden = true; return; }

    const stableTag = await stableUpdate(dev, force);
    if (stableTag) { showUpdateBadge(badge, stableTag, stableTag, dev.isDesktop); return; }

    // Only a prerelease (-dev…) build follows the moving latest channel; a stable device is
    // not nudged toward an unreleased build.
    const onPrerelease = (parse(dev.version)?.prerelease.length || 0) > 0;
    if (onPrerelease) {
        const devVer = await devUpdate(dev, force);
        if (devVer) { showUpdateBadge(badge, "latest", `latest (${devVer})`, dev.isDesktop); return; }
    }
    badge.hidden = true;
}

// Badge click → pre-select the new release in the picker (it restores from PICKER_RELEASE_KEY
// on init) and open the Firmware card, so the user lands one click from Install.
function setupUpdateBadge() {
    const badge = document.getElementById("fw-update-badge");
    if (!badge) return;
    badge.addEventListener("click", () => {
        if (badge.dataset.desktop) {
            const tag = badge.dataset.tag || "latest";
            window.open(`https://github.com/MoonModules/projectMM/releases/tag/${tag}`, "_blank",
                        "noopener");
            return;
        }
        if (badge.dataset.tag) safeLocalSet(PICKER_RELEASE_KEY, badge.dataset.tag);
        selectModule("Firmware");
    });
}

// ---------------------------------------------------------------------------
// 8b. File Manager view
// ---------------------------------------------------------------------------

// Format a byte count as a short human size (matches the fs seam's uint32 sizes).
function fmSize(n) {
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
    return (n / (1024 * 1024)).toFixed(1) + " MB";
}

// Per-module File Manager UI state, kept across state refreshes (the DOM is rebuilt on refetch, but
// the tree's expand-state + selection are the user's, not the module's). Keyed by module name.
const fmStateByMod = {};
function fmState(mod) {
    return (fmStateByMod[mod.name] ||= { expanded: new Set(["/"]), selected: "/" });
}

// Fetch one directory's children (name/isDir/size) from /api/dir. `hidden` includes dotfiles.
// The MoonLive factory catalog: which scripts EXIST upstream, as opposed to which are on this
// device. Cached for the session because it is compiled into the firmware and cannot change while
// the device runs.
let mlCatalog = null;
async function mlFetchCatalog() {
    if (mlCatalog) return mlCatalog;
    const res = await fetch("/api/scripts");
    if (!res.ok) throw new Error(await errorMessage(res));
    mlCatalog = await res.json();
    return mlCatalog;
}

/// Which catalog group a picker's extension belongs to, so a script picker offers only its own role.
function mlGroupForExt(ext) {
    return ext === ".mle" ? "effects" : ext === ".mll" ? "layouts"
         : ext === ".mlm" ? "modifiers" : ext === ".mls" ? "services"
         : ext === ".mlp" ? "palettes" : null;
}

// Download one factory script and save it to the device.
//
// The BROWSER fetches it, not the device: raw.githubusercontent.com sends
// `access-control-allow-origin: *`, so the page can read it directly, and the device then needs no
// TLS stack, no certificate bundle and no internet of its own. A rig on an isolated network is
// served by whatever laptop is looking at its UI. Same approach as WLED-MM's arti-fx.
//
// Pinned to the firmware's own tag (the endpoint decides which), so a script always matches the
// engine that will run it rather than whatever main happens to hold.
async function mlDownloadScript(name, group) {
    const cat = await mlFetchCatalog();
    const folder = (cat[group] || {}).folder;
    if (!folder) throw new Error("unknown script kind");
    const url = "https://raw.githubusercontent.com/MoonModules/projectMM/"
              + encodeURIComponent(cat.tag) + "/moonlive/" + folder + "/" + encodeURIComponent(name);
    const res = await fetch(url);
    // A 404 has two causes now. A RELEASE build fetches from its tag, so a missing script means the
    // release does not carry it. A DEV build fetches from the commit it was built from, so a 404
    // usually means that commit was never pushed: the scripts exist locally and GitHub has never
    // seen them. Say which, because the fix differs (wait for a release vs push the branch).
    if (!res.ok) {
        if (res.status !== 404) throw new Error("download failed");
        const dev = !String(cat.tag || "").startsWith("v");
        throw new Error(dev
            ? name + " is not on GitHub at commit " + cat.tag + " (push the branch, or the script is new)"
            : name + " is not in this firmware's release");
    }
    const text = await res.text();
    if (!text.trim()) throw new Error("downloaded script is empty");
    // The factory directory has to exist first: POST /api/file does not create parents, so on a
    // device where nothing has been downloaded yet the write fails with "write failed" and no clue
    // why. mkdir is idempotent, so this costs one request and only on the first download.
    await fetch("/api/dir?path=" + encodeURIComponent(cat.dir), { method: "POST" }).catch(() => {});
    // Straight to the factory directory, never the user's: an edit is what puts a copy there, and
    // that copy is what shadows this one.
    const save = await fetch("/api/file?path=" + encodeURIComponent(cat.dir + "/" + name), {
        method: "POST", headers: { "Content-Type": "application/octet-stream" },
        body: new Blob([text]),
    });
    if (!save.ok) throw new Error(await errorMessage(save));
}

async function fmFetchDir(absPath, hidden) {
    const res = await fetch("/api/dir?path=" + encodeURIComponent(absPath) + (hidden ? "&hidden=1" : ""));
    if (!res.ok) throw new Error(await errorMessage(res));
    const rows = await res.json();
    return Array.isArray(rows) ? rows : [];
}

// Render the File Manager panel: a lazy expand/collapse folder tree (the standard VS Code / Explorer
// shape: an expanded folder's children are loaded from /api/dir), plus a toolbar (show
// hidden, new folder, delete on the selected node). Filesystem ops go through the module's controls
// (path/new folder/delete); browsing is pure UI over /api/dir, so the module stays minimal.
// MoonCloud Stats: what everyone else reported, rendered on the card that asked for consent.
//
// Reads only: no id, no report and no configuration leave the browser here. The charts are drawn
// EMPTY until this member's consent is on, so what saying yes gets you is visible before you say
// it, and a server that cannot be reached says so rather than rendering as an empty one.
// Where the aggregates come from: the SAME `server` / `serverPort` controls the firmware posts
// its report to, read off the module rather than hardcoded here. One setting drives both
// directions, so pointing a device at a local or self-hosted MoonCloud moves the fetch with it
// instead of leaving the card looking at somewhere else.
// THE MoonCloud address, the same constant the firmware compiles in. Not read from a control:
// there is one MoonCloud, and moving it is a release rather than a setting somebody can mistype.
// Whether this member's own consent is on. The data a MoonCloud card shows is what contributing
// earns back, so an unconsented card renders the same charts with nothing in them.
function consented(mod) {
    return Boolean((mod?.controls || []).find(c => c.name === "consent")?.value);
}

const kMoonCloudUrl = "https://mooncloud-stats.moonmodules.workers.dev";

let moonCloudStatsCache = new Map();   // filter query -> stats, so switching back is free
let moonCloudFilter = {};              // dimension -> value the reader chose
let moonCloudGeneration = 0;           // bumped on a control write; older answers are dropped
let moonTalkCache = null;

const kMoonCloudColors = ["#4a9eff", "#ff8c42", "#3ecf8e", "#e8618c", "#a78bfa",
                          "#f6c445", "#4dd0e1", "#9aa5b1"];

// An SVG arc from one angle to another. The whole-circle case is separate because an arc whose
// start and end coincide draws NOTHING, so a single-slice pie would come out blank.
function moonCloudArc(cx, cy, r, a0, a1) {
    if (a1 - a0 >= Math.PI * 2 - 1e-9) {
        return `M ${cx} ${cy - r} A ${r} ${r} 0 1 1 ${cx - 0.001} ${cy - r} Z`;
    }
    const x0 = cx + r * Math.sin(a0), y0 = cy - r * Math.cos(a0);
    const x1 = cx + r * Math.sin(a1), y1 = cy - r * Math.cos(a1);
    return `M ${cx} ${cy} L ${x0} ${y0} ` +
           `A ${r} ${r} 0 ${a1 - a0 > Math.PI ? 1 : 0} 1 ${x1} ${y1} Z`;
}

// Past the 7th slice everything becomes one "other" wedge: a pie with twenty slivers reads as
// noise, and the exact tail is a question for the raw API rather than a card.
function moonCloudTopSlices(rows, keep = 7) {
    if (!rows || rows.length <= keep) return rows || [];
    const head = rows.slice(0, keep);
    const tail = rows.slice(keep).reduce((sum, r) => sum + r.count, 0);
    return tail ? head.concat([{ name: "other", count: tail }]) : head;
}

function moonCloudPie(rows, onPick) {
    const NS = "http://www.w3.org/2000/svg";
    const total = rows.reduce((sum, r) => sum + r.count, 0) || 1;
    const svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 200 200");
    svg.setAttribute("class", "mooncloud-pie");

    // No rows: one filled circle, so a placeholder reads as a pie waiting for data rather than as
    // the blank sliver a zero-angle slice produces.
    if (!rows.length) {
        const disc = document.createElementNS(NS, "circle");
        disc.setAttribute("cx", "100");
        disc.setAttribute("cy", "100");
        disc.setAttribute("r", "92");
        disc.setAttribute("class", "mooncloud-pie-placeholder");
        svg.appendChild(disc);
        return svg;
    }

    let angle = 0;
    rows.forEach((r, i) => {
        const next = angle + (r.count / total) * Math.PI * 2;
        const path = document.createElementNS(NS, "path");
        path.setAttribute("d", moonCloudArc(100, 100, 92, angle, next));
        path.setAttribute("fill", kMoonCloudColors[i % kMoonCloudColors.length]);
        const pickable = onPick && r.name && r.name !== "other";
        path.setAttribute("class", pickable ? "mooncloud-slice mooncloud-pickable" : "mooncloud-slice");
        const title = document.createElementNS(NS, "title");
        title.textContent = `${r.name}: ${r.count} (${Math.round((r.count / total) * 100)}%)`
            + (pickable ? ", click to filter" : "");
        path.appendChild(title);
        if (pickable) {
            path.addEventListener("click", () => onPick(r.name));
            // Reachable without a mouse, the same way a collapsible card header is: a filter nobody
            // can operate from the keyboard is a control only some people have.
            path.setAttribute("tabindex", "0");
            path.setAttribute("role", "button");
            path.addEventListener("keydown", (e) => {
                if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onPick(r.name); }
            });
        }
        svg.appendChild(path);
        angle = next;
    });
    return svg;
}

function moonCloudLegend(rows, onPick) {
    const box = document.createElement("div");
    box.className = "mooncloud-legend";
    rows.forEach((r, i) => {
        const line = document.createElement("div");
        const swatch = document.createElement("span");
        swatch.className = "mooncloud-swatch";
        swatch.style.background = kMoonCloudColors[i % kMoonCloudColors.length];
        const name = document.createElement("span");
        name.className = "mooncloud-legend-name";
        name.textContent = r.name || "unknown";
        const count = document.createElement("span");
        count.className = "mooncloud-legend-count";
        count.textContent = String(r.count);
        line.append(swatch, name, count);
        // `other` is a bucket of everything past the top slices, so it names no value to filter by.
        if (onPick && r.name && r.name !== "other") {
            line.className = "mooncloud-pickable";
            line.addEventListener("click", () => onPick(r.name));
            line.tabIndex = 0;
            line.setAttribute("role", "button");
            line.addEventListener("keydown", (e) => {
                if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onPick(r.name); }
            });
        }
        box.appendChild(line);
    });
    return box;
}

function renderMoonCloudStats(host, mod) {
    const section = document.createElement("div");
    section.className = "mooncloud-stats";
    host.appendChild(section);

    // As in MoonTalk: `reached` separates "the server said there is nothing" from "the server never
    // answered". Only the first may render as no statistics; the second says so.
    const draw = (stats, reached = true) => {
        section.textContent = "";
        if (!reached) {
            const failed = document.createElement("div");
            failed.className = "mooncloud-stats-title mooncloud-unreachable";
            failed.textContent = "Cannot reach the MoonCloud server.";
            section.appendChild(failed);
            return;
        }
        const head = document.createElement("div");
        head.className = "mooncloud-stats-head";
        const title = document.createElement("div");
        title.className = "mooncloud-stats-title";
        title.textContent = stats
            ? `What everyone else is running: ${stats.installations} installations`
            : "What everyone else is running";
        head.appendChild(title);
        // Fetched once per card build, so this is how to ask again without reloading. Only with
        // consent: without it there is nothing to refresh, because nothing is fetched.
        if (consented(mod)) {
            const refresh = document.createElement("button");
            refresh.className = "mooncloud-refresh";
            refresh.textContent = "\u21bb";
            refresh.title = "Refresh";
            refresh.addEventListener("click", () => { moonCloudStatsCache.clear(); load(); });
            head.appendChild(refresh);
        }

        section.appendChild(head);

        // A filter is always something the reader chose, so it is stated plainly with a way back.
        const active = stats ? Object.entries(moonCloudFilter) : [];
        if (active.length) {
            const bar = document.createElement("div");
            bar.className = "mooncloud-filter";
            const what = document.createElement("span");
            // A bounds pair reads as one range, not two numbers: `lightCountMin` + `lightCountMax`
            // came from a single slice and a reader thinks of it that way.
            const seen = new Set();
            const parts = [];
            for (const [k, v] of active) {
                if (k.endsWith("Label")) continue;   // display text for a bounds pair, not a filter
                const base = k.replace(/(Min|Max)$/, "");
                if (base !== k) {
                    if (seen.has(base)) continue;
                    seen.add(base);
                    // The slice's own label, not the bounds: the reader clicked "64-128 KB" and
                    // "65537 to 131072" is the same fact in a form nobody chose.
                    parts.push(moonCloudFilter[`${base}Label`] ?? moonCloudFilter[`${base}Min`]);
                    continue;
                }
                parts.push(k === "dev" ? (v === "1" ? "development" : "released") : v);
            }
            what.textContent = "Filtered to " + parts.join(", ");
            const clear = document.createElement("button");
            clear.className = "mooncloud-filter-clear";
            clear.textContent = "Clear";
            clear.addEventListener("click", () => { moonCloudFilter = {}; load(); });
            bar.append(what, clear);
            section.appendChild(bar);
        }

        const grid = document.createElement("div");
        grid.className = "mooncloud-charts";

        // The four the server aggregates. Country is included even though a device never reports
        // it (the edge derives it from the connection): withholding it here while serving it to
        // anyone who calls /api/stats would be a distinction without a difference, and a
        // country-level count is the same coarse figure every comparable dashboard shows.
        // Each chart names the query parameter its slices filter by, so clicking one re-counts
        // every other chart within it. `Build` maps to `dev`, whose values are 0 and 1 rather than
        // the labels shown.
        // The list names a KEY, not a value: reading `stats.versions` here dereferenced a null
        // answer while the array was built, which threw before any per-chart guard could run and
        // took the whole card down with it.
        for (const [label, key, param] of [["Version", "versions", "version"],
                                           ["Chip", "chips", "chip"],
                                           ["Board", "deviceModels", "deviceModel"],
                                           ["Flash", "flash", "flash"],
                                           ["PSRAM", "psram", "psram"],
                                           ["SDK", "sdk", "sdk"],
                                           ["Event", "events", "event"],
                                           ["Upgraded from", "previousVersions", "previousVersion"],
                                           ["Drivers", "drivers", "driver"],
                                           ["Services", "services", "service"],
                                           ["Layouts", "layouts", "layout"],
                                           ["Effects", "effects", "effect"],
                                           ["Modifiers", "modifiers", "modifier"],
                                           ["Lights", "lightCounts", "lightCount"],
                                           ["Free memory", "freeMemory", "freeHeap"],
                                           ["Total memory", "totalMemory", "totalHeap"],
                                           ["FPS", "fps", "fps"],
                                           ["Build", "builds", "dev"],
                                           ["Country", "countries", "country"]]) {
            // No stats: the heading over an empty circle, so the card shows what consent unlocks
            // without naming a single value somebody else reported.
            const shown = stats ? moonCloudTopSlices(stats[key]) : [];
            if (stats && !shown.length) continue;
            const chart = document.createElement("div");
            chart.className = "mooncloud-chart";
            const heading = document.createElement("div");
            heading.className = "mooncloud-chart-title";
            heading.textContent = label;
            // A bucketed slice carries `min`/`max` rather than a value the column could equal, so
            // it filters by BOUNDS: the label is a range, the column holds the raw number.
            const bounded = shown.some(r => r.min !== undefined);
            const pick = (name) => {
                if (bounded) {
                    const row = shown.find(r => r.name === name);
                    if (!row) return;
                    delete moonCloudFilter[`${param}Min`];
                    delete moonCloudFilter[`${param}Max`];
                    delete moonCloudFilter[`${param}Label`];
                    if (row.min !== undefined) moonCloudFilter[`${param}Min`] = String(row.min);
                    if (row.max !== undefined) moonCloudFilter[`${param}Max`] = String(row.max);
                    moonCloudFilter[`${param}Label`] = name;   // what the reader actually clicked
                } else {
                    moonCloudFilter[param] = param === "dev"
                        ? (name === "development" ? "1" : "0")
                        : name;
                }
                load();
            };
            chart.append(heading, moonCloudPie(shown, pick), moonCloudLegend(shown, pick));
            grid.appendChild(chart);
        }
        section.appendChild(grid);
    };

    // Cached per filter for the session: these numbers move on the scale of days, a card rebuild
    // happens on every unrelated state push, and clicking back to a filter already read is free.
    const load = () => {
        // Without consent NOTHING is fetched: the headings are a constant in this file, so the card
        // draws its own shape. A request would carry no identifier and still be a call to our server
        // from a device whose owner said no.
        if (!consented(mod)) { draw(null, true); return; }
        // `*Label` is what the filter bar shows, not something the server knows: the bounds are the
        // filter, and sending a label would be an unknown parameter.
        const sent = Object.fromEntries(
            Object.entries(moonCloudFilter).filter(([k]) => !k.endsWith("Label")));
        const query = new URLSearchParams(sent).toString();
        if (moonCloudStatsCache.has(query)) { draw(moonCloudStatsCache.get(query), true); return; }
        const generation = moonCloudGeneration;
        fetch(kMoonCloudUrl + "/api/stats" + (query ? `?${query}` : ""), { cache: "no-store" })
            .then(r => r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`)))
            .then(stats => {
                if (generation !== moonCloudGeneration) return;   // a newer write superseded this
                moonCloudStatsCache.set(query, stats);
                draw(stats, true);
            })
            // Not cached: a failure is a fact about right now, so the next card build retries.
            .catch(() => draw(null, false));
    };
    load();
}

// MoonTalk: the public board, read straight onto the card that posts to it.
//
// Reading sends no identifier: no id, no name, nothing about this device. It still happens only
// while consent is on, because a request from a device whose owner said no is still a request to
// our server.
//
// Polled on card build rather than streamed. A chat wants to feel live, but a device UI rebuilds
// this card on every unrelated state push, so a socket per card would be a connection per render.
// The refresh button is the honest version until MoonCloud Sync brings a real transport.
function renderMoonTalk(host, mod) {
    const section = document.createElement("div");
    section.className = "moontalk";
    host.appendChild(section);

    const header = document.createElement("div");
    header.className = "moontalk-header";
    const title = document.createElement("span");
    title.textContent = "Messages";
    header.appendChild(title);

    const list = document.createElement("div");
    list.className = "moontalk-list";
    section.append(header, list);

    // `reached` says whether the server answered at all. An unreachable server and an empty board
    // are different facts and lead to different actions (check the network, versus wait for
    // someone to post), so they must not render as the same nothing.
    const draw = (messages, reached = true, asked = true) => {
        list.textContent = "";
        if (!reached) {
            const failed = document.createElement("div");
            failed.className = "moontalk-empty moontalk-unreachable";
            failed.textContent = "Cannot reach the MoonCloud server.";
            list.appendChild(failed);
            return;
        }
        if (!messages || !messages.length) {
            const empty = document.createElement("div");
            empty.className = "moontalk-empty";
            // "No messages yet." is a claim about the board. Without consent it was never read, so
            // the card says what is true instead: nothing was asked.
            empty.textContent = asked ? "No messages yet."
                                      : "Turn on consent to read the board.";
            list.appendChild(empty);
            return;
        }
        // Oldest at the top, like every chat: the API returns newest first for paging.
        for (const m of messages.slice().reverse()) {
            const row = document.createElement("div");
            row.className = "moontalk-message";

            const who = document.createElement("span");
            // `named` says whether the sender consented to share a device name. Without it the
            // server sends the first 8 characters of the installation id, shown in a monospace
            // face so it reads as an identifier rather than as somebody's name.
            who.className = m.named ? "moontalk-from" : "moontalk-from moontalk-from-id";
            who.textContent = m.from;
            // A named sender keeps its id in brackets: a device name is whatever someone typed, so
            // two devices can share one, and the id is what tells them apart.
            if (m.named && m.senderId) {
                const id = document.createElement("span");
                id.className = "moontalk-from-id";
                id.textContent = `(${m.senderId})`;
                who.appendChild(document.createTextNode(" "));
                who.appendChild(id);
            }

            const text = document.createElement("span");
            text.className = "moontalk-text";
            text.textContent = m.text;   // textContent, never innerHTML: this is a stranger's text

            const when = document.createElement("span");
            when.className = "moontalk-when";
            when.textContent = new Date(m.sentAt).toLocaleTimeString([], {
                hour: "2-digit", minute: "2-digit",
            });
            when.title = new Date(m.sentAt).toLocaleString();

            row.append(who, text, when);
            list.appendChild(row);
        }
        // Oldest first means the newest is at the BOTTOM of a scrolling box, so a refresh that did
        // not scroll left the message someone just sent out of sight.
        list.scrollTop = list.scrollHeight;
    };

    // A card is rebuilt on every unrelated state push, so a fetch per build is a request storm
    // against a device serving its own UI. Cached for the session like the stats are, and the
    // refresh button forces a re-read: `force` is what tells the two apart.
    const load = (force) => {
        // Same rule as Stats: without consent the board is not fetched at all. The cache is CLEARED
        // rather than left holding the empty draw, because a cached `[]` from an unconsented card is
        // not an answer: consenting rebuilt the card, the cache hit below returned that empty array,
        // and the board stayed blank until a manual refresh.
        if (!consented(mod)) { moonTalkCache = null; draw([], true, false); return; }
        if (!force && moonTalkCache) { draw(moonTalkCache, true); return; }
        const generation = moonCloudGeneration;
        fetch(kMoonCloudUrl + "/api/talk", { cache: "no-store" })
            .then(r => r.ok ? r.json() : Promise.reject(new Error(`HTTP ${r.status}`)))
            .then(d => {
                if (generation !== moonCloudGeneration) return;   // a newer write superseded this
                moonTalkCache = d?.messages ?? [];
                draw(moonTalkCache, true);
            })
            // Not cached: a failure is a fact about right now, so the next build retries.
            .catch(() => draw(null, false));
    };
    // Created AFTER `load`, which is a const arrow: a listener attached above its declaration
    // throws on click rather than hoisting. Only with consent, since without it nothing is fetched
    // and a button that does nothing is worse than none.
    if (consented(mod)) {
        const refresh = document.createElement("button");
        refresh.className = "mooncloud-refresh";
        refresh.textContent = "\u21bb";
        refresh.title = "Refresh";
        refresh.addEventListener("click", () => load(true));
        header.appendChild(refresh);
    }

    load(false);
}

function renderFileManager(mod, host) {
    const ctrl = (n) => (mod.controls || []).find(c => c.name === n);
    const st = fmState(mod);
    // The toggle is UI-owned: seed it from the persisted control on first render, then `st` is the
    // source of truth. Reading it back from `mod` each render would revert the checkbox, since an
    // in-panel re-render runs against the same (stale) state snapshot, before /api/state updates.
    if (st.showHidden === undefined) st.showHidden = !!ctrl("show hidden")?.value;
    const hidden = st.showHidden;

    // Render into a single stable panel that we REPLACE on every re-render (expand / collapse /
    // select / op): reusing it in place rather than appending, so the tree updates inline instead
    // of stacking a fresh copy below the old one.
    let panel = host.querySelector(":scope > .fm-panel");
    if (panel) panel.replaceChildren();
    else { panel = document.createElement("div"); panel.className = "fm-panel"; host.appendChild(panel); }

    // Show-hidden toggle: owned by the panel (not the generic control list) so flipping it can
    // re-fetch the tree with the new `hidden` filter immediately, rather than waiting for a state
    // refresh that never re-runs the /api/dir fetch. Reuses the same .switch pill markup every
    // other bool control uses (common patterns first), so it reads consistently.
    const hiddenRow = document.createElement("label");
    hiddenRow.className = "fm-hidden-toggle";
    hiddenRow.appendChild(document.createTextNode("show hidden"));
    const sw = document.createElement("span");
    sw.className = "switch";
    const hiddenBox = document.createElement("input");
    hiddenBox.type = "checkbox";
    hiddenBox.checked = hidden;
    hiddenBox.addEventListener("change", () => {
        st.showHidden = hiddenBox.checked;                    // UI-owned source of truth
        sendControl(mod.name, "show hidden", hiddenBox.checked);   // persist (best-effort, no await)
        renderFileManager(mod, host);                         // re-list with the new filter
    });
    const track = document.createElement("span");
    track.className = "switch-track";
    sw.appendChild(hiddenBox);
    sw.appendChild(track);
    hiddenRow.appendChild(sw);
    panel.appendChild(hiddenRow);

    // Select a path level (from a breadcrumb crumb): update the selection, tidy the tree to just the
    // path, and re-render. "Reveal and collapse siblings": keep the ancestor chain root→…→crumb
    // open, fold every other branch AND the clicked node's own descendants. Selecting a directory
    // drives where ＋ folder/＋ file create and what delete targets.
    const selectPath = (absPath, isDir) => {
        st.selected = absPath;
        st.selectedIsDir = isDir;
        // Rebuild `expanded` as the ancestor chain root→…→crumb, plus the clicked node itself when
        // it's a directory (so you see one level into it): every other branch and anything deeper
        // folds. Root is always expanded (the tree shows its children).
        const segs = absPath.split("/").filter(Boolean);   // "/.config/foo" → [".config","foo"]
        st.expanded = new Set(["/"]);
        for (let i = 0; i < segs.length; i++) {
            const p = "/" + segs.slice(0, i + 1).join("/");
            // Include every ancestor; include the clicked node only if it's a directory.
            if (i < segs.length - 1 || isDir) st.expanded.add(p);
        }
        renderFileManager(mod, host);
    };

    // Breadcrumb of the selected path, on its OWN row above the toolbar (a deep path wraps freely
    // without crowding the buttons: key on narrow displays). Click a crumb to jump there; `root`
    // deselects back to /.
    const crumbs = document.createElement("div");
    crumbs.className = "fm-crumbs";
    const mkCrumb = (label, absPath, isDir) => {
        const b = document.createElement("button");
        b.className = "fm-crumb" + (absPath === st.selected ? " fm-crumb--here" : "");
        b.textContent = label;
        b.addEventListener("click", () => selectPath(absPath, isDir));
        return b;
    };
    // (Re)build the breadcrumb from the current selection, into the stable `crumbs` element: called
    // on first render and by refreshSelectionControls() when a file click updates the selection in place.
    const rebuildCrumbs = () => {
        crumbs.replaceChildren();
        crumbs.appendChild(mkCrumb("root", "/", true));   // always present: the way back to /
        const segs = st.selected.split("/").filter(Boolean);   // "/.config/foo" → [".config","foo"]
        segs.forEach((s, i) => {
            crumbs.appendChild(document.createTextNode(" / "));
            // A crumb is a directory unless it's the last segment of a selected *file*.
            const isLast = i === segs.length - 1;
            const isDir = !isLast || st.selectedIsDir;
            crumbs.appendChild(mkCrumb(s, "/" + segs.slice(0, i + 1).join("/"), isDir));
        });
    };
    rebuildCrumbs();
    panel.appendChild(crumbs);

    // Refresh the controls that depend on the current selection WITHOUT rebuilding the tree: the
    // breadcrumb and the delete button's disabled state. Used by the in-place file-click path so a
    // file's row survives for its dblclick. (delBtn's press-twice handler reads st.selected live.)
    const refreshSelectionControls = () => {
        rebuildCrumbs();
        delBtn.disabled = st.selected === "/";
    };

    // Toolbar (own row below the breadcrumb): New folder / New file / Delete / Refresh.
    const bar = document.createElement("div");
    bar.className = "fm-bar";

    // A filesystem op is an HTTP call on /api/dir?path= (POST=mkdir, DELETE=remove): the path
    // rides the query, so nothing is stored/persisted on the device. On failure, surface the
    // server's error; on success, re-render the tree from disk.
    const runOp = async (op, targetPath) => {
        const method = op === "delete" ? "DELETE" : "POST";
        try {
            const res = await fetch("/api/dir?path=" + encodeURIComponent(targetPath), { method });
            if (!res.ok) alert(`${op} failed: ${await errorMessage(res)}`);
        } catch (err) {   // a network error (offline / reset) would otherwise be an unhandled rejection
            alert(`${op} failed: ${err.message || err}`);
        }
        renderFileManager(mod, host);  // rebuild the tree from /api/dir (fresh listing), success or handled failure
    };

    // A new file/folder is created inside the selected node if it's a folder, else next to it (in
    // the selected file's parent). Shared by both create buttons.
    const createBase = () => (st.selectedIsDir ? st.selected : fmParent(st.selected));

    // Icon-only toolbar: each button shows a glyph; the `title` carries the word for the tooltip +
    // screen readers (the standard icon-button pattern: an icon with an accessible label).
    const newBtn = document.createElement("button");
    newBtn.className = "fm-tool fm-tool--icon";
    newBtn.textContent = "📁";
    newBtn.title = "New folder: create a folder inside the selected folder";
    newBtn.addEventListener("click", async () => {
        const base = createBase();
        const name = (prompt("New folder name in " + base + ":") || "").trim();
        if (!name) return;             // blank or whitespace-only → no-op
        st.expanded.add(base);         // reveal the new child
        await runOp("new folder", joinFsPath(base, name));
    });
    bar.appendChild(newBtn);

    // New file: no module op needed: the /api/file POST creates a file at a path (empty body), the
    // same endpoint the editor saves through. Then re-render the tree from disk.
    const newFileBtn = document.createElement("button");
    newFileBtn.className = "fm-tool fm-tool--icon";
    newFileBtn.textContent = "📝";
    newFileBtn.title = "New file: create an empty file inside the selected folder";
    newFileBtn.addEventListener("click", async () => {
        const base = createBase();
        const name = (prompt("New file name in " + base + ":") || "").trim();
        if (!name) return;             // blank or whitespace-only → no-op
        const r = await fmCreateFile(base, name);
        if (!r.ok) { alert("create file failed: " + r.message); return; }
        st.expanded.add(base);         // reveal the new file
        renderFileManager(mod, host);  // re-list from disk
    });
    bar.appendChild(newFileBtn);

    // Upload: pick desktop files and stream them into the selected folder via the same /api/file
    // POST the drag-drop path uses (fmDropUpload): a button for people who don't drag. A hidden
    // <input type=file multiple> is the recognizable browser file-picker; clicking the button opens it.
    const upBtn = document.createElement("button");
    upBtn.className = "fm-tool fm-tool--icon";
    upBtn.textContent = "↥";   // pairs with the per-row ↓ download glyph
    upBtn.title = "Upload: upload files from your computer into the selected folder";
    const upInput = document.createElement("input");
    upInput.type = "file";
    upInput.multiple = true;
    upInput.style.display = "none";
    upInput.addEventListener("change", async () => {
        const files = Array.from(upInput.files || []);
        upInput.value = "";                       // reset so re-picking the same file re-fires change
        if (!files.length) return;
        const base = createBase();
        st.expanded.add(base);                    // reveal the destination folder
        const skipped = await fmDropUpload(base, files);
        renderFileManager(mod, host);             // re-list from disk
        if (skipped.length) alert("Not uploaded:\n" + skipped.join("\n"));
    });
    upBtn.addEventListener("click", () => upInput.click());
    bar.appendChild(upBtn);
    bar.appendChild(upInput);

    // Backup / Restore: the bulk counterparts of the per-file download/upload. Backup walks the
    // whole filesystem into one JSON bundle (fmBackupConfig); Restore uploads a bundle back and
    // reports what didn't carry over (fmRestoreConfig). Restore overwrites config, so it shares
    // the delete button's armed double-press.
    const bakBtn = document.createElement("button");
    bakBtn.className = "fm-tool fm-tool--icon";
    bakBtn.textContent = "⤓";
    bakBtn.title = "Backup device config, download every file (config, scripts, presets) as one JSON bundle. Keep it private: it contains the WiFi password.";
    bakBtn.addEventListener("click", async () => {
        bakBtn.disabled = true;
        bakBtn.textContent = "…";   // the walk takes seconds; show work in progress at once
        try { await fmBackupConfig(); }
        catch (err) { alert("Backup failed: " + err.message); }
        finally { bakBtn.disabled = false; bakBtn.textContent = "⤓"; }
    });
    bar.appendChild(bakBtn);

    const restInput = document.createElement("input");
    restInput.type = "file";
    restInput.accept = ".json,application/json";
    restInput.style.display = "none";
    restInput.addEventListener("change", async () => {
        const file = (restInput.files || [])[0];
        restInput.value = "";
        if (!file) return;
        try { await fmRestoreConfig(file, () => renderFileManager(mod, host)); }
        catch (err) { alert("Restore failed: " + err.message); }
    });
    const restBtn = document.createElement("button");
    restBtn.className = "fm-tool fm-tool--icon fm-tool--danger";
    restBtn.textContent = "⟲";
    restBtn.title = "Restore config from a backup bundle, overwrites the device's files, then reports anything that didn't carry over";
    armPressTwice(restBtn, () => restInput.click(), { armedText: "✓" });
    bar.appendChild(restBtn);
    bar.appendChild(restInput);

    const delBtn = document.createElement("button");
    delBtn.className = "fm-tool fm-tool--icon fm-tool--danger";
    delBtn.textContent = "🗑";
    // A folder goes with everything in it: emptying one by hand before it would delete was busywork
    // on a folder of scripts. The two-press arm is the confirmation.
    delBtn.title = "Delete: delete the selected file, or a folder and everything in it";
    delBtn.disabled = st.selected === "/";   // never delete the root
    armPressTwice(delBtn, () => runOp("delete", st.selected), { armedText: "✓" });
    bar.appendChild(delBtn);

    const refBtn = document.createElement("button");
    refBtn.className = "fm-tool fm-tool--icon";
    refBtn.textContent = "⟳";
    refBtn.title = "Refresh";
    refBtn.addEventListener("click", () => renderFileManager(mod, host));
    bar.appendChild(refBtn);
    panel.appendChild(bar);

    // The tree. Root ("/") is always present and expanded; its children populate asynchronously.
    const tree = document.createElement("div");
    tree.className = "fm-tree";
    // Dropping desktop files onto the tree's empty space uploads them into root.
    fmMakeDropTarget(tree, "/", hidden, () => renderFileManager(mod, host), st);
    panel.appendChild(tree);

    // Render one directory's children into `container` at `depth`, recursing into expanded folders.
    const renderChildren = async (dirPath, container, depth) => {
        let rows;
        try {
            rows = await fmFetchDir(dirPath, hidden);
        } catch (err) {
            const e = document.createElement("div");
            e.className = "fm-empty";
            e.textContent = "list failed: " + err.message;
            container.appendChild(e);
            return;
        }
        if (rows.length === 0) {
            const e = document.createElement("div");
            e.className = "fm-empty";
            e.style.paddingLeft = (depth * 16 + 20) + "px";
            e.textContent = "empty";
            container.appendChild(e);
            return;
        }
        // Folders first, then files; each alphabetical: the conventional file-manager sort.
        rows.sort((a, b) => (b.isDir - a.isDir) || a.name.localeCompare(b.name));
        for (const entry of rows) {
            const childPath = joinFsPath(dirPath, entry.name);
            const isOpen = st.expanded.has(childPath);

            const rowEl = document.createElement("div");
            rowEl.className = "fm-row" + (childPath === st.selected ? " fm-row--sel" : "");
            rowEl.style.paddingLeft = (depth * 16 + 4) + "px";

            // Chevron: only folders can expand; a file gets a spacer so names line up.
            const chev = document.createElement("span");
            chev.className = "fm-chev";
            chev.textContent = entry.isDir ? (isOpen ? "▾" : "▸") : "";
            rowEl.appendChild(chev);

            const icon = document.createElement("span");
            icon.className = "fm-icon";
            icon.textContent = entry.isDir ? (isOpen ? "📂" : "📁") : "📄";
            rowEl.appendChild(icon);

            const name = document.createElement("span");
            name.className = "fm-name";
            name.textContent = entry.name;
            rowEl.appendChild(name);

            const size = document.createElement("span");
            size.className = "fm-size";
            size.textContent = entry.isDir ? "" : fmSize(entry.size || 0);
            rowEl.appendChild(size);

            // Per-file download (device → desktop): a plain <a download> on /api/file forces a save
            // with the right name, every browser, any file type: the portable counterpart to the
            // drag-drop upload (a true drag-*out* has no cross-browser API). Folders get no ⤓;
            // folder-as-zip is backlogged (needs a bundled zip lib + recursion).
            if (!entry.isDir) {
                const dl = document.createElement("a");
                dl.className = "fm-dl";
                dl.textContent = "⤓";
                dl.title = "download";
                dl.href = "/api/file?path=" + encodeURIComponent(childPath);
                dl.setAttribute("download", entry.name);
                dl.addEventListener("click", (ev) => ev.stopPropagation());   // don't select/open
                rowEl.appendChild(dl);
            }

            // Single-click SELECTS (highlights, sets the op target for delete); a folder also
            // toggles expand. DOUBLE-click OPENS a file in the editor: the VS Code / Finder /
            // Explorer norm, and it keeps a distinct "selected but not opened" state for future
            // rename / multi-select / context-menu features.
            rowEl.addEventListener("click", (ev) => {
                ev.stopPropagation();
                st.selected = childPath;
                st.selectedIsDir = entry.isDir;
                if (entry.isDir) {
                    // A folder click also expands/collapses: rows appear/disappear, so a full
                    // re-render is needed.
                    if (isOpen) st.expanded.delete(childPath);
                    else st.expanded.add(childPath);
                    renderFileManager(mod, host);
                } else {
                    // A file click only moves the selection: update the highlight IN PLACE, never
                    // re-render. A re-render here would destroy this row mid-gesture, so the dblclick
                    // that follows a double-click would land on a fresh element and openFileEditor
                    // might not fire. Move the --sel class + refresh the selection-dependent controls.
                    for (const r of panel.querySelectorAll(".fm-row--sel")) r.classList.remove("fm-row--sel");
                    rowEl.classList.add("fm-row--sel");
                    refreshSelectionControls();
                }
            });
            if (!entry.isDir) {
                rowEl.addEventListener("dblclick", (ev) => {
                    ev.stopPropagation();
                    openFileEditor(childPath, entry.size);   // size lets the editor detect a truncated read
                });
            }

            // Drag-drop upload target: dropping desktop files onto a FOLDER row uploads them into
            // that folder (tier 1: text/config ≤8KB: see fmDropUpload).
            if (entry.isDir) fmMakeDropTarget(rowEl, childPath, hidden, () => renderFileManager(mod, host), st);
            container.appendChild(rowEl);

            // Recurse into an expanded folder (its own indented sub-container).
            if (entry.isDir && isOpen) {
                const sub = document.createElement("div");
                sub.className = "fm-subtree";
                container.appendChild(sub);
                await renderChildren(childPath, sub, depth + 1);
            }
        }
    };

    renderChildren("/", tree, 0);

    // Filesystem usage bar below the tree: the File Manager's own `filesystem` control (used /
    // total bytes from the platform). Absent (e.g. desktop fs total 0) → nothing shown.
    const fsCtrl = fmFilesystemUsage(mod);
    if (fsCtrl) {
        const usage = document.createElement("div");
        usage.className = "fm-usage";
        const name = document.createElement("span");
        name.className = "fm-usage-name";
        name.textContent = "Used";      // the bar/value is used space out of total (see the trailing label)
        const bar = document.createElement("progress");
        bar.value = Number(fsCtrl.value) || 0;
        bar.max = Number(fsCtrl.total) || 1;
        const lbl = document.createElement("span");
        lbl.className = "fm-usage-label";
        lbl.textContent = fmtProgressLabel(fsCtrl);
        usage.appendChild(name);
        usage.appendChild(bar);
        usage.appendChild(lbl);
        panel.appendChild(usage);
    }

    // "last saved" readout below the usage bar: how long ago config was persisted. The value is
    // owned by the FilesystemModule engine (non-UI); the File Manager displays it because this is
    // where filesystem state is topical (same reasoning as the usage bar).
    const savedCtrl = (mod?.controls || []).find(c => c.name === "lastSaved");
    if (savedCtrl) {
        const row = document.createElement("div");
        row.className = "fm-lastsaved";
        row.textContent = `saved: ${savedCtrl.value ?? "never"}`;
        panel.appendChild(row);
    }
}

// The File Manager's own `filesystem` usage progress control (used/total bytes), or null if the
// platform reports no partition. Rendered as the bar below the tree; skipped in the generic control
// loop so it appears only here.
function fmFilesystemUsage(mod) {
    return (mod?.controls || []).find(c => c.name === "filesystem") || null;
}

// Format a file's text for the editor, by extension. JSON is re-indented (2 spaces) so the persisted
// config files are readable; anything that doesn't parse is shown verbatim rather than erroring.
// Extension seam for later: MoonLive `.ml` source wants syntax *highlighting* (a color layer over
// the textarea), not reformatting: that's a bigger editor change, added when MoonLive `.ml` files
// land on disk.
function fmPrettify(text, relPath) {
    if (relPath.endsWith(".json")) {
        try { return JSON.stringify(JSON.parse(text), null, 2); } catch (_) {}
    }
    return text;
}

// The parent directory of an absolute path ("/a/b" → "/a", "/a" → "/").
function fmParent(absPath) {
    const cut = absPath.lastIndexOf("/");
    return cut <= 0 ? "/" : absPath.slice(0, cut);
}

// Join a dir + name with one slash (UI-side path building for the editor's path= query).
function joinFsPath(dir, name) {
    return dir.endsWith("/") ? dir + name : dir + "/" + name;
}

// Drag-drop upload (desktop → device). The device streams the body straight to the file (any size,
// binary-safe), so the only client-side bound is a sanity cap matching the device's kUploadMax; a
// file over it is skipped with a visible note (no silent drop). A too-big-for-free-space file is
// also rejected device-side with a "not enough space" message.
const FM_UPLOAD_CAP = 256 * 1024;   // matches HttpServerModule::kUploadMax

// Wire an element as a drop target that uploads dropped files into `destDir`, then re-renders.
function fmMakeDropTarget(el, destDir, hidden, rerender, st) {
    el.addEventListener("dragover", (e) => {
        e.preventDefault();
        e.stopPropagation();
        el.classList.add("fm-row--drop");
    });
    el.addEventListener("dragleave", (e) => {
        e.stopPropagation();
        el.classList.remove("fm-row--drop");
    });
    el.addEventListener("drop", async (e) => {
        e.preventDefault();
        e.stopPropagation();
        el.classList.remove("fm-row--drop");
        const files = Array.from(e.dataTransfer?.files || []);
        if (!files.length) return;
        const skipped = await fmDropUpload(destDir, files);
        if (destDir !== "/") st.expanded.add(destDir);   // reveal where they landed
        rerender();
        if (skipped.length) {
            // Report what wasn't uploaded (over the size cap or a write error) rather than dropping
            // it silently. The limit is derived from FM_UPLOAD_CAP so the text never drifts from it.
            alert(`Not uploaded (over ${fmSize(FM_UPLOAD_CAP)} or failed):\n` + skipped.join("\n"));
        }
    });
}

// ---- Config backup & restore (tier 1: pure browser over the existing file API) ----
// The engine core (walk, ordering, report) lives in migrate.js so node tests cover it;
// this is the wiring to the real endpoints plus the download / report UI. Serial requests
// throughout: the device serves one connection at a time and closes each.

async function fmBackupConfig() {
    const fetchDir = async (path) => fmFetchDir(path, true);   // hidden=1: .config must list
    const fetchFile = async (path) => {
        const res = await fetch("/api/file?path=" + encodeURIComponent(path));
        if (!res.ok) throw new Error(path + ": HTTP " + res.status);
        // Not res.text(): that strips a leading UTF-8 BOM, which would both fail the
        // byte-length check and restore a BOM-prefixed file without its BOM.
        return new TextDecoder("utf-8", { ignoreBOM: true }).decode(await res.arrayBuffer());
    };
    const { files, skipped } = await collectFiles(fetchDir, fetchFile);
    // Device identity for the filename + header, straight off the live state.
    let device = location.hostname || "device", firmware = "", build = "";
    try {
        const st = await (await fetch("/api/state")).json();
        (function walk(mods) { for (const m of mods || []) {
            for (const c of m.controls || []) {
                if (c.name === "deviceName" && c.value) device = c.value;
                if (c.name === "firmware") firmware = c.value || "";
                if (c.name === "build") build = c.value || "";
            }
            walk(m.children);
        }})(st.modules);
    } catch (_) {}
    const bundle = {
        format: "MoonLight-config-backup", version: 1,
        capturedAt: new Date().toISOString(), origin: location.origin,
        device, firmware, build, files,
    };
    // The diagnostics-download tail: Blob, objectURL, a[download], revoke late (Safari races
    // an immediate revoke against the save dialog).
    const blob = new Blob([JSON.stringify(bundle, null, 1)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `MoonLight-config-${device}-${new Date().toISOString().slice(0, 10)}.json`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 4000);
    if (skipped.length) {
        alert("Backed up. Skipped (not text, the bundle carries text only):\n" + skipped.join("\n"));
    }
}

async function fmRestoreConfig(file, refresh) {
    const bundle = JSON.parse(await file.text());
    // Restore is the one supported upgrade path, so it reads the format the previous name wrote as well as its own.
    const kBackupFormats = ["MoonLight-config-backup", "projectMM-config-backup"];   // rename-keep: a bundle already saved carries the name it was saved under
    if (!kBackupFormats.includes(bundle.format) || !bundle.files) {
        throw new Error("not a config backup");
    }
    if (bundle.version !== 1) {
        throw new Error(`backup version ${bundle.version} is newer than this firmware understands`);
    }
    // The rename map first: MIGRATING.md's schema breaks, applied client-side and reported.
    const { files, report } = applyMigrations(bundle.files);
    // Directories before files (mkdir is non-recursive); existing dirs answer 500, harmless.
    for (const dir of restoreDirs(files)) {
        await fetch("/api/dir?path=" + encodeURIComponent(dir), { method: "POST", body: "" }).catch(() => {});
    }
    const failed = [];
    // Network config last: it applies LIVE, and joining another network mid-restore would cut
    // off the writes still to come (the AP flow's whole point).
    const ordered = Object.entries(files).sort(
        ([a], [b]) => (a.endsWith("/NetworkModule.json") ? 1 : 0) - (b.endsWith("/NetworkModule.json") ? 1 : 0));
    for (const [path, content] of ordered) {
        let res = null;
        // One retry: the device serves a single connection at a time, so a busy moment
        // drops a socket transiently (seen on the bench); only a retried failure is real.
        for (let attempt = 0; attempt < 2 && (!res || !res.ok); attempt++) {
            if (attempt) await new Promise(r => setTimeout(r, 300));
            res = await fetch("/api/file?path=" + encodeURIComponent(path), {
                method: "POST", headers: { "Content-Type": "application/octet-stream" },
                body: new Blob([content]),
            }).catch(() => null);
        }
        if (!res || !res.ok) {
            failed.push({ kind: "control", where: path, detail: "write failed (" + (res ? res.status : "network") + ")" });
        }
    }
    // The report: what this firmware no longer understands, plus everything the rename map did.
    let live = [], typeNames = [];
    try { live = (await (await fetch("/api/state")).json()).modules || []; } catch (_) {}
    try { typeNames = ((await (await fetch("/api/types")).json()).types || []).map(t => t.name); } catch (_) {}
    const entries = [...report, ...failed, ...diffRestore(files, live, typeNames)];
    // Network bring-up is not live-re-appliable (it opts out device-side); its restored file
    // waits for the next boot, so the dialog offers the restart exactly when that matters.
    const hasNetwork = Object.keys(files).some(p => p.endsWith("/NetworkModule.json"));
    fmShowRestoreReport(entries, refresh, hasNetwork);
}

// The report dialog. Not an alert: the list can be long and deserves reading.
function fmShowRestoreReport(entries, refresh, offerRestart) {
    const ov = document.createElement("div");
    ov.className = "fw-overlay";
    const box = document.createElement("div");
    box.className = "fw-overlay-box";
    const h = document.createElement("h2");
    h.textContent = "Restore complete";
    const msg = document.createElement("p");
    msg.className = "fw-overlay-msg";
    box.appendChild(h);
    box.appendChild(msg);
    if (entries.length === 0) {
        msg.textContent = "Everything carried over.";
    } else {
        msg.textContent = "Carried over with notes (see MIGRATING.md for the breaking-change log):";
        const list = document.createElement("ul");
        list.style.cssText = "text-align:left;max-height:14rem;overflow-y:auto;font-size:.85rem";
        for (const e of entries) {
            const li = document.createElement("li");
            li.textContent = `[${e.kind}] ${e.where}: ${e.detail}`;
            list.appendChild(li);
        }
        box.appendChild(list);
    }
    // Everything applies live except what the device itself declares boot-only: network
    // bring-up (its module opts out of the live apply) and the web server's listen port
    // (binds at boot). The restart button appears exactly when the bundle carried those.
    const note = document.createElement("p");
    note.className = "fw-overlay-msg";
    note.textContent = offerRestart
        ? "Settings are applied live, except network settings: restart the device to apply them (after the restart it joins the network the backup names)."
        : "Settings are applied live.";
    box.appendChild(note);
    if (offerRestart) {
        const restart = document.createElement("button");
        restart.textContent = "restart device";
        restart.addEventListener("click", async () => {
            try { await fetch("/api/reboot", { method: "POST" }); } catch (_) {}
            ov.remove();
        });
        box.appendChild(restart);
    }
    const close = document.createElement("button");
    close.textContent = "close";
    close.addEventListener("click", () => { ov.remove(); if (refresh) refresh(); });
    box.appendChild(close);
    ov.appendChild(box);
    document.body.appendChild(ov);
}


// Upload each dropped file into destDir via /api/file. Returns the names skipped (too big / failed)
// so the caller can report them. The File blob is sent as the body directly: the browser streams
// its raw bytes (binary-safe), matching the device's streamed, byte-exact write.
async function fmDropUpload(destDir, files) {
    const skipped = [];
    for (const file of files) {
        if (file.size > FM_UPLOAD_CAP) { skipped.push(file.name + " (" + fmSize(file.size) + ")"); continue; }
        try {
            const res = await fetch("/api/file?path=" + encodeURIComponent(joinFsPath(destDir, file.name)), {
                method: "POST", headers: { "Content-Type": "application/octet-stream" }, body: file,
            });
            if (!res.ok) throw new Error(await errorMessage(res));   // surfaces "not enough space (N free)" etc.
        } catch (err) {
            skipped.push(file.name + " (" + err.message + ")");
        }
    }
    return skipped;
}

// --- the file editor, shared by the File Manager modal and the filepath control ---------------
//
// One implementation, two hosts. The File Manager opens it in a <dialog> from a tree row; a
// `filepath` control mounts the same pane inline on a module's card. The guards below are the
// reason this is worth sharing rather than re-typing: each one exists because a re-save could
// otherwise destroy a file the editor could not faithfully represent.

// Load `relPath` into `textarea`. Returns {readOnly, message}: read-only when the file cannot be
// safely round-tripped through a <textarea>, so a save can never write a lossy copy back over it.
async function fmLoadInto(textarea, relPath, expectedSize, signal) {
    try {
        const res = await fetch("/api/file?path=" + encodeURIComponent(relPath), { signal });
        // Surface the server's own message (e.g. "not found") rather than a bare status code.
        if (!res.ok) throw new Error(await errorMessage(res));
        const text = await res.text();
        // Truncation guard: serveFileContents streams the whole file but stops short on a read error
        // (a filesystem fault mid-stream). Saving a short read back would overwrite the file with a
        // truncated copy: so if the received byte count is under the size the listing reported, load
        // read-only. TextEncoder gives the byte length (text.length is chars, not bytes).
        if (typeof expectedSize === "number" &&
            new TextEncoder().encode(text).length < expectedSize) {
            textarea.value = text;
            return { readOnly: true, message: "truncated read: read-only (save would corrupt the file)" };
        }
        // The editor is text/config only: a <textarea> can't faithfully round-trip non-text bytes,
        // so a re-save would corrupt the file. Treat it as binary: read-only: if it has a NUL OR
        // if res.text()'s UTF-8 decode left a replacement char (U+FFFD), which means the bytes
        // weren't valid UTF-8 and are already lossy in the textarea. Use the per-row ⤓ to download
        // such files intact.
        if (text.indexOf("\0") !== -1 || text.indexOf("\uFFFD") !== -1) {
            textarea.value = text;
            return { readOnly: true, message: "binary / non-text file: read-only" };
        }
        textarea.value = fmPrettify(text, relPath);
        return { readOnly: false, message: "" };
    } catch (err) {
        // A superseded load was cancelled on purpose (the caller has already moved to another
        // file): leave the pane alone and say so, so the newer load owns what is on screen.
        if (err && err.name === "AbortError") return { aborted: true, readOnly: false, message: "" };
        textarea.value = "";
        // Read-only on a failed load so a Save can never post an empty body over a file that is
        // merely unreachable.
        return { readOnly: true, message: "load failed: " + err.message };
    }
}

// Write `textarea`'s contents to `relPath`. Returns {ok, message}.
async function fmSaveFrom(textarea, relPath) {
    try {
        const res = await fetch("/api/file?path=" + encodeURIComponent(relPath), {
            method: "POST",
            headers: { "Content-Type": "text/plain" },
            body: textarea.value,
        });
        if (!res.ok) throw new Error(await errorMessage(res));
        return { ok: true, message: "saved" };
    } catch (err) {
        return { ok: false, message: "save failed: " + err.message };
    }
}

// Create a file at `dir`/`name`, holding `content` (empty by default). The same POST the editor
// saves through. A control whose module declares a template passes it here, so a new file is a
// working example rather than a blank one: for anything the device parses, empty is invalid, and
// the first thing a new file would say is an error message.
async function fmCreateFile(dir, name, content = "") {
    const filePath = joinFsPath(dir, name);
    try {
        const res = await fetch("/api/file?path=" + encodeURIComponent(filePath), {
            method: "POST", headers: { "Content-Type": "text/plain" }, body: content,
        });
        if (!res.ok) throw new Error(await errorMessage(res));
        return { ok: true, path: filePath, message: "" };
    } catch (err) {
        return { ok: false, path: filePath, message: err.message };
    }
}

// Build the editing pane (textarea + status + Save) into `host` and wire its save triggers.
//
// Saves on BLUR, on Ctrl/Cmd+S and on the Save button, not on every keystroke. A save writes to
// flash and re-derives whatever depends on the file, so autosaving would do both against text that
// is mid-edit and usually invalid. Blur plus an explicit key is what every editor a user already
// knows does, and the dot marks unsaved work the way a modified tab does.
//
// `onSaved(relPath)` fires after each successful save. Returns a handle so a caller can point the
// same pane at a different file without rebuilding it.
/// Prism's C++ grammar plus the three type names MoonLive adds.
///
/// `byte`, `fixed` and `string` are the language's own aliases (a C++ reader would meet uint8_t,
/// a Q16.16 int and a const char*), so a stock C++ grammar leaves them unpainted beside the `int`
/// next to them. Extending rather than writing a grammar: everything else in a script IS C++, which
/// is what test_scripts_are_cpp.py holds each shipped script to.
///
/// Built once and cached: the highlighter runs on every keystroke.
let mlGrammarCache = null;
function mlGrammar() {
    if (mlGrammarCache) return mlGrammarCache;
    // A COPY of the C++ grammar with one rule replaced: its keyword pattern, widened to also match
    // MoonLive's three type names. Prism.languages.insertBefore was the other route and it is the
    // wrong tool here (it rebuilds the language in place and expects a rule object, not a bare
    // RegExp), which silently produced a grammar that highlighted nothing at all.
    const cpp = Prism.languages.cpp;
    mlGrammarCache = Object.assign({}, cpp, {
        keyword: [/\b(?:byte|fixed|string)\b/].concat(cpp.keyword || []),
    });
    return mlGrammarCache;
}

function fmMountEditor(host, relPath, opts = {}) {
    // `savePath(readPath)` lets a caller WRITE somewhere other than it read. The script picker uses
    // it: a factory script is read from the read-only library directory, and editing it must create
    // the user's own copy rather than overwrite what shipped. Defaults to writing back where it
    // read, which is what every other caller wants.
    const { expectedSize, onSaved, onDispose, sizeKey, saveButton, statusEl, savePath,
            initialStatus } = opts;
    const wrap = document.createElement("div");
    wrap.className = "fm-editor-pane";
    // The footer carries Save and the status line, UNLESS the host supplies both: a card already has
    // a toolbar of file actions, so they belong there, and an empty strip under the box is a gap
    // rather than a layout.
    // Highlighting is for SCRIPTS: a .mle/.mll/.mlm/.mls/.mlp is MoonLive, which is C++, so Prism's own C++
    // grammar paints it with nothing of ours to maintain. A .json or a .txt edits as plain text.
    const hlOn = /\.(mle|mll|mlm|mls|mlp)$/i.test(relPath || "");

    // The highlight layer sits BEHIND a transparent textarea, both sharing one box and one set of
    // font metrics: a textarea cannot color its own text, and this is the standard way around that.
    // The textarea keeps every editing behavior (caret, selection, undo, IME); the <pre> only paints.
    wrap.innerHTML =
        '<div class="fm-editor-stack">' +
        '<pre class="fm-editor-hl" aria-hidden="true"><div class="fm-editor-err" hidden></div><code></code></pre>' +
        '<textarea class="fm-editor-body" spellcheck="false" wrap="off"></textarea>' +
        '<div class="fm-editor-grip" title="drag to resize"></div>' +
        '</div>' +
        '<div class="fm-editor-foot">' +
        (statusEl   ? '' : '  <span class="fm-editor-status"></span>') +
        '  <span class="fm-editor-caret"></span>' +
        (saveButton ? '' : '  <button class="action-btn fm-editor-save">Save</button>') +
        '</div>';
    const body = wrap.querySelector(".fm-editor-body");
    if (!hlOn) wrap.querySelector(".fm-editor-stack").classList.add("plain");
    let errorLine = -1;                 // 0-based, -1 for none: set by markError below

    /// A character offset as the line and column a person counts in, both 1-based.
    ///
    /// The device reports an OFFSET, because that is what the parser has; nobody reads a script by
    /// offset. The editor holds the same text, so it is the one place that can do the conversion.
    const lineColAt = (off) => {
        const upto = body.value.slice(0, Math.max(0, Math.min(off, body.value.length)));
        const nl = upto.lastIndexOf("\n");
        return { line: upto.split("\n").length, col: upto.length - nl };
    };
    const hl = wrap.querySelector(".fm-editor-hl");
    const hlCode = hl.querySelector("code");
    const errBand = hl.querySelector(".fm-editor-err");

    /// Repaint the layer under the caret, and keep it aligned.
    ///
    /// Only for a MoonLive script: Prism is given the C++ grammar because that is what the language
    /// is (test/python/test_scripts_are_cpp.py holds every shipped script to it), so `class`, the
    /// types and the comments light up with no grammar of our own. Any other file edits as plain
    /// text, which is what a .json or a .txt should look like.
    ///
    /// A trailing newline gets a space: a <pre> collapses the last empty line where a textarea
    /// keeps it, and without this the two drift by one line at the end of a file.
    const paintHighlight = () => {
        if (!hlOn) return;
        const src = body.value;
        hlCode.textContent = src.endsWith("\n") ? src + " " : src;
        if (window.Prism && Prism.languages.cpp) {
            hlCode.innerHTML = Prism.highlight(hlCode.textContent, mlGrammar(), "cpp");
        }
        // The failing line, marked with a BAND positioned over it rather than by wrapping its text.
        // Wrapping meant splitting Prism's serialized HTML on newlines, which cuts any token that
        // spans lines (a block comment is one element) in half and leaves unbalanced tags for the
        // parser to re-balance, shifting the coloring of everything after it. A band cannot touch
        // the markup at all, and it lands on the same line because both use the same line height.
        errBand.hidden = errorLine < 0;
        if (errorLine >= 0) errBand.style.top = `calc(${errorLine} * 1.5em)`;
        syncHighlightScroll();
    };
    /// Move the paint under the text by TRANSFORM rather than by scrolling it: a scrollTop the
    /// element cannot reach (it has overflow:hidden) is silently clamped, which is what left the
    /// last lines of a file unreachable.
    /// Mark the line a compile failed on, and return the status with a readable position.
    ///
    /// Converts the device's offset against the text this editor HOLDS, so it must not run before
    /// the file has loaded: every position would resolve to line 1.
    const markErrorAt = (statusText) => {
        errorLine = -1;
        let shown = statusText || "";
        // Two forms, because this is called with both: the raw device status "message @<offset>" on
        // every update, and an ALREADY rewritten "message (line L, col C)" when a second editor
        // opens on a module whose status was converted before it existed.
        const at = /@(\d+)\s*$/.exec(shown);
        const lc = /\(line (\d+), col \d+\)\s*$/.exec(shown);
        if (at) {
            const p = lineColAt(Number(at[1]));
            errorLine = p.line - 1;
            // The offset is replaced, not appended to: line and column is the only half a person can
            // act on, and the editor marks the line anyway.
            shown = shown.slice(0, at.index).trimEnd() + ` (line ${p.line}, col ${p.col})`;
        } else if (lc) {
            errorLine = Number(lc[1]) - 1;
        }
        paintHighlight();
        return shown;
    };

    const syncHighlightScroll = () => {
        // The band follows VERTICALLY only: it spans the full width, so a horizontal shift would
        // just walk it off the box while the line it marks stays put.
        hlCode.style.transform = `translate(${-body.scrollLeft}px, ${-body.scrollTop}px)`;
        errBand.style.transform = `translateY(${-body.scrollTop}px)`;
    };
    const status = statusEl || wrap.querySelector(".fm-editor-status");
    const saveBtn = saveButton || wrap.querySelector(".fm-editor-save");
    host.appendChild(wrap);

    let path = relPath;
    let dirty = false;
    const setDirty = (d) => {
        dirty = d;
        saveBtn.classList.toggle("dirty", d);
        if (d) status.textContent = "unsaved changes";
        // The button says it too, for a host that shows no status line: the dot marks unsaved work
        // and the tooltip spells it out, which is where a user looks when a control has a dot on it.
        if (saveBtn.title !== undefined) {
            saveBtn.title = d ? "Save (unsaved changes)" : "Save (or click away, or Ctrl/Cmd+S)";
        }
    };

    // Restore a previously dragged height, the same view-state the plain textarea control keeps, so
    // an editor a user sized once stays that size. Keyed per control, or per path in the modal.
    const key = sizeKey || ("fm:" + path);
    const savedH = textareaSizes[key];
    const stack = wrap.querySelector(".fm-editor-stack");
    if (typeof savedH === "number" && savedH > 0) stack.style.height = savedH + "px";
    let taRaf = 0, taPrevH = Math.round(savedH > 0 ? savedH : 0);
    const taObserver = new ResizeObserver((entries) => {
        const h = Math.round(entries[0].contentRect.height);
        if (taRaf || h <= 0 || h === taPrevH) return;
        taRaf = requestAnimationFrame(() => { taRaf = 0; taPrevH = h; saveTextareaSize(key, h); });
    });
    taObserver.observe(stack);

    // Resize by dragging the grip.
    //
    // Ours rather than the browser's `resize`, which cannot work here: the textarea is positioned
    // over the whole stack including the corner the native gesture starts in, so the press lands on
    // the text and the drag never begins. The handle is a real element above both layers, and
    // setPointerCapture keeps the drag alive once the pointer leaves those few pixels.
    const grip = wrap.querySelector(".fm-editor-grip");
    let dragFrom = 0, dragH = 0;
    grip.addEventListener("pointerdown", (e) => {
        dragFrom = e.clientY;
        dragH = stack.getBoundingClientRect().height;
        grip.setPointerCapture(e.pointerId);
        e.preventDefault();                    // no text selection while dragging
    });
    grip.addEventListener("pointermove", (e) => {
        if (!dragFrom) return;
        // No floor here: the stack's own min-height clamps it, so one rule owns the minimum.
        stack.style.height = `${dragH + (e.clientY - dragFrom)}px`;
    });
    const endDrag = (e) => {
        if (!dragFrom) return;
        dragFrom = 0;
        if (grip.hasPointerCapture(e.pointerId)) grip.releasePointerCapture(e.pointerId);
        // The ResizeObserver above persists the new height; nothing to store here.
    };
    grip.addEventListener("pointerup", endDrag);
    grip.addEventListener("pointercancel", endDrag);

    // Blur, Cmd+S and the Save button all call save(), and a blur fires when the button takes
    // focus: so without this guard one edit issues overlapping POSTs of the same file. `dirty`
    // cannot serve as the guard: it is only cleared after the await, so a second trigger passes
    // the check while the first request is still in flight. Saves are serialized rather than
    // dropped, because the second trigger may carry newer keystrokes than the first.
    let saving = null;
    /// The file's text as it was LOADED, for the no-op fork check in save().
    let loadedText = null;
    const save = () => {
        if (body.readOnly || !dirty || !path) return Promise.resolve();
        saving = (saving || Promise.resolve()).then(async () => {
            if (body.readOnly || !dirty || !path) return;   // re-check: a queued save may be moot
            const saved = body.value;                       // what THIS request writes
            status.textContent = "saving…";
            const dest = savePath ? savePath(path) : path;
            // A FORK THAT CHANGES NOTHING IS NOT A FORK. When the destination differs from the file
            // that was read, saving creates a user copy that shadows the shipped one forever: every
            // later library update becomes invisible behind it. If the text is byte-identical to
            // what was read, there is nothing to shadow it WITH, so write nothing and let the
            // shipped copy keep winning. Opening a script and pressing Save is otherwise enough to
            // pin it at today's version, which is the trap this whole mechanism exists to avoid.
            if (dest !== path && loadedText !== null && saved === loadedText) {
                status.textContent = "unchanged";
                setDirty(false);
                return;
            }
            const r = await fmSaveFrom(body, dest);
            status.textContent = r.message;
            // A failed write (no space, a vanished path) must not be silent. The modal shows it on
            // its status line; a host that supplied its own hidden one gets an alert, because the
            // work is still unsaved and the dot alone does not say why.
            if (!r.ok && statusEl && statusEl.hidden) alert(r.message);
            // Only clear dirty if the body still holds what we just wrote: typing during the
            // request means there are newer bytes on screen that nobody has saved yet.
            if (r.ok && body.value === saved) {
                setDirty(false);
                // The fork EXISTS now, so this pane is editing it rather than the file it came
                // from. Without this the no-op guard above keeps comparing against the shipped
                // copy: editing back to the original text would report "unchanged" and skip the
                // write, leaving the user's file holding the previous edit.
                path = dest;
                loadedText = saved;
                // Report where it LANDED, not where it came from: a caller that refreshes a listing
                // needs to know a new file now exists in the user's directory.
                if (onSaved) onSaved(dest);
            }
        });
        return saving;
    };

    // Where the caret IS, so a reported line and column can be found by moving to it. Updated from
    // every event that can move a caret; `selectionchange` alone does not fire on a plain textarea
    // in every browser, so the input and key/click events cover it.
    const caretEl = wrap.querySelector(".fm-editor-caret");
    const showCaret = () => {
        if (!caretEl) return;
        const at = lineColAt(body.selectionStart);
        caretEl.textContent = `Ln ${at.line}, Col ${at.col}`;
    };
    ["input", "click", "keyup", "select", "focus"].forEach(e => body.addEventListener(e, showCaret));

    body.addEventListener("input", () => { if (!body.readOnly) setDirty(true); paintHighlight(); });
    // Scroll is not an input event: the layer has to follow the box it sits under.
    body.addEventListener("scroll", () => syncHighlightScroll());
    body.addEventListener("blur", save);
    body.addEventListener("keydown", (e) => {
        if ((e.metaKey || e.ctrlKey) && (e.key === "s" || e.key === "S")) { e.preventDefault(); save(); }
    });
    saveBtn.addEventListener("click", save);

    // Picking a second file before the first finishes loading must not paint the first file's
    // contents over it. The in-flight request is ABORTED rather than merely ignored, because
    // fmLoadInto writes the textarea itself: a late response would overwrite the newer file's
    // contents before any check here could run.
    let loadAbort = null;
    const load = async (p, size) => {
        if (loadAbort) loadAbort.abort();
        const ac = new AbortController();
        loadAbort = ac;
        path = p;
        setDirty(false);
        if (!path) {
            body.value = ""; body.readOnly = true; saveBtn.disabled = true; status.textContent = "";
            // The mark and the caret readout belong to text that is no longer there: left alone, an
            // empty pane keeps a red band over nothing and a stale Ln/Col.
            errorLine = -1;
            paintHighlight();
            showCaret();
            return;
        }
        const r = await fmLoadInto(body, path, size, ac.signal);
        if (r.aborted || ac !== loadAbort) return;   // a newer load started: that one owns the pane
        // What arrived, so save() can tell a real edit from an untouched file. Only used when the
        // save destination differs from the read path (a factory script being forked).
        loadedText = body.value;
        body.readOnly = r.readOnly;
        saveBtn.disabled = r.readOnly;
        paintHighlight();          // the file just arrived: paint what it says
        showCaret();
        status.textContent = r.message;
        // A compile error the module was ALREADY reporting when this editor was built. Marked here
        // rather than at construction, because markError converts an offset against the text: run
        // before the file arrives and every position resolves to line 1.
        if (initialStatus) { markErrorAt(initialStatus); initialStatus = null; }
    };
    load(path, expectedSize);

    return {
        textarea: body,
        load,
        // Flush pending edits before the pane is navigated away from or replaced by the modal.
        // Resolves once any in-flight save has completed, so the caller can safely re-read.
        save,
        isDirty: () => dirty,
        /// Still on the page? A card rebuild detaches an inline editor without disposing it.
        isMounted: () => wrap.isConnected,
        /// Mark the line a compile failed on, from the module's own status.
        ///
        /// The device reports "message @<offset>", an offset into the source it compiled, which is
        /// the only position anyone has: the parser records it and nothing else can reconstruct it.
        /// Passing "" or a status with no @ clears the mark, so a fixed script stops being flagged
        /// the moment it compiles.
        markError: markErrorAt,
        dispose: () => { taObserver.disconnect(); wrap.remove(); if (onDispose) onDispose(); },
    };
}

// Open the shared editor in a modal, for the File Manager's tree rows. Uses the native <dialog>,
// no bespoke overlay code, and mounts exactly the pane a card mounts inline.
async function openFileEditor(relPath, expectedSize, moduleName) {
    const dlg = document.createElement("dialog");
    dlg.className = "fm-editor";
    dlg.innerHTML =
        '<form method="dialog" class="fm-editor-head">' +
        '  <span class="fm-editor-path"></span>' +
        '  <button value="close" class="fm-editor-x" title="close">✕</button>' +
        '</form>';
    dlg.querySelector(".fm-editor-path").textContent = relPath;
    document.body.appendChild(dlg);
    // Registered under the module that opened it, exactly as the card's pane is: the status row is
    // rendered by the card either way, and setStatusText marks every editor on that module. Without
    // a module name (the File Manager's own rows) nothing registers and the modal simply highlights.
    let unregister = () => {};
    const ed = fmMountEditor(dlg, relPath, { expectedSize, onDispose: () => unregister() });
    unregister = mlEditorAdd(moduleName, ed);
    // Mark it NOW, from the status already on the card: registration only catches the next update,
    // and a compile failure that happened before the modal opened would otherwise show unmarked
    // until the module recompiles. The row is the card's, so it holds the same text either way.
    if (moduleName) {
        const row = (queryByName(`[data-status-mid="${cssEscape(moduleName)}"]`, "data-status-mid", moduleName)
            || {}).querySelector?.(".status-value");
        if (row) ed.markError(row.textContent);
    }
    dlg.showModal();
    // Resolves when the dialog CLOSES, not when it opens: a caller that re-reads the file
    // afterwards (the card's pane shows the same file) would otherwise read it before any edit.
    await new Promise((resolve) => {
        // AWAIT the save before disposing. Closing blurs the textarea, which starts a save; the
        // promise above exists so the caller can re-read the file afterwards, and resolving while
        // that write is still in flight is exactly the stale read it is meant to prevent.
        // ed.save() is a no-op when nothing is dirty, and its own queue makes a double-call safe.
        dlg.addEventListener("close", async () => {
            try { await ed.save(); } finally { ed.dispose(); dlg.remove(); resolve(); }
        }, { once: true });
    });
}

// ---------------------------------------------------------------------------
// 9. Boot
// ---------------------------------------------------------------------------

document.addEventListener("DOMContentLoaded", init);


/// The palette editor: a gradient strip whose stops are dragged, added and removed, plus the
/// pickers behind them. Lifted out of createControl's switch, where its 180 lines and the
/// filepath case's 496 made a dispatch table read as an implementation.
///
/// Same shape as buildKnob / buildListEntries / buildListPads above: one control type, one
/// function, taking exactly what it needs from the row createControl already built.
function buildPaletteControl(row, key, def, moduleName, ctrl) {
    // A color-palette dropdown where EVERY option shows its own gradient: so the colors
    // are visible before selecting, not just after. A native <select> can't do this
    // (browsers ignore a gradient background on <option>, and the macOS popup is OS-drawn),
    // so this is a custom dropdown: a trigger button (selected swatch + name + caret) that
    // toggles a list of styled rows, each a gradient swatch + name. The value still rides as
    // the option index. Prior art: MoonLight's palette control (same native-select limit).
    const opts = ctrl.options || [];
    const gradientFor = (i) => paletteGradientCss((opts[i] || {}).colors);
    const wrap = document.createElement("div");
    wrap.className = "palette-control";
    wrap.dataset.mid = moduleName;
    wrap.dataset.key = ctrl.name;
    wrap.dataset.value = ctrl.value;

    // Trigger: shows the currently-selected palette (swatch + name) and opens the list.
    const trigger = document.createElement("button");
    trigger.type = "button";
    trigger.className = "palette-trigger";
    const triSwatch = document.createElement("span");
    triSwatch.className = "palette-swatch";
    // The selected palette's emoji, beside the swatch, exactly as a module card shows its
    // type's emoji: the closed control should say what KIND of palette is running (scripted
    // or built-in, warm or cold) without opening the list to find out.
    const triEmoji = document.createElement("span");
    triEmoji.className = "palette-emoji";
    const triName = document.createElement("span");
    triName.className = "palette-name";
    const caret = document.createElement("span");
    caret.className = "palette-caret";
    caret.textContent = "▾";
    const paintTrigger = (i) => {
        const o = opts[i] || {};
        triSwatch.style.background = gradientFor(i);
        triEmoji.textContent = (o.live ? SCRIPTED_EMOJI : "") + (o.tags || "");
        triName.textContent = o.name || String(i);
    };
    paintTrigger(ctrl.value);
    trigger.append(triSwatch, triEmoji, triName, caret);

    // The LIST is the shared picker, the same widget the module and script pickers use, so a
    // palette gets search and emoji-chip filtering for free rather than through a second
    // hand-rolled dropdown that would have to grow both. The trigger above stays: it is what
    // makes a palette control recognizable at a glance, and a picker row cannot show the
    // current selection while it is closed.
    //
    // Rows carry `colors`, which is what makes the picker paint a gradient beside each name;
    // every other list passes none and is unchanged.
    // Factory palettes the device does NOT hold yet, offered alongside the ones it does:
    // without this a scripted palette can only be chosen once it is already downloaded, so
    // there is nothing to select in order TO download it. Same contract the script pickers
    // give every other MoonLive role.
    //
    // They sit LAST, after the local live palettes, because a palette is chosen by INDEX and
    // that index is what the knob and Home Assistant step through. Renumbering is confined
    // to the live section, which moves only when someone adds or removes a script.
    const remotePalettes = () => {
        const names = ((mlCatalog || {}).palettes || {}).names || [];
        const tags = ((mlCatalog || {}).palettes || {}).tags || [];
        // BOTH sides stripped of the extension before comparing: the device publishes a
        // live palette by its full filename ("drift.mlp") and so does the catalog, but
        // stripping only one side matched nothing, so every already-downloaded palette
        // showed a second time as a "download me" row.
        const bare = (s) => String(s).replace(/\.mlp$/, "");
        const have = new Set(opts.filter(o => o.live).map(o => bare(o.name)));
        return names.map((n, i) => ({ name: n, tags: tags[i] || "" }))
                    .filter(r => !have.has(bare(r.name)));
    };
    const openList = async () => {
        // The catalog is fetched lazily and cached, so ask for it BEFORE building the list:
        // on the first open mlCatalog is still null and every remote row would be missing.
        // A failure (offline device) is not fatal: the list falls back to what is local.
        await mlFetchCatalog().catch(() => {});
        const remote = remotePalettes();
        const local = opts.map((o, i) => ({
            name: String(i),                 // the VALUE: a palette is chosen by index
            displayName: o.name || String(i),
            // The SCRIPTED marker is the same 📝 a scripted effect carries, prepended
            // here rather than baked into the device's tag string: it is a UI fact
            // ("this row runs a script"), and the scripted/compiled chips must filter
            // palettes by the same rule they filter every other list by.
            tags: (o.live ? SCRIPTED_EMOJI : "") + (o.tags || ""),
            colors: o.colors || "",
            role: "palette",
        }));
        // `colors: ""` is what marks a row as not-yet-downloaded: its swatch renders as a
        // placeholder, because a scripted palette has no gradient until it has run once.
        const items = local.concat(remote.map(r => ({
            name: "\u0000" + r.name,        // not an index: a NAME, to download then select
            displayName: r.name.replace(/\.mlp$/, ""),
            tags: SCRIPTED_EMOJI + (r.tags || ""),
            colors: "",
            role: "palette",
        })));
        openPicker(trigger, {
            items,
            actionLabel: "use",
            keepOrder: true,          // index order: the knob and HA step through it
            currentType: String(ctrl.value),
            commit: async (name) => {
                // A remote row carries a filename, not an index: fetch it, then let the
                // rebuilt control (the device re-lists its .mlp files) select it by index.
                if (name.charCodeAt(0) === 0) {
                    const file = name.slice(1);
                    try {
                        await mlDownloadScript(file, "palettes");
                    } catch (e) {
                        alert("could not download " + file + ": " + (e && e.message ? e.message : e));
                        return;
                    }
                    // The write itself republishes the option list: POST /api/file ends in
                    // applyFileChanged() -> requestPrepareTree() -> Drivers::prepare(), which
                    // re-lists the `.mlp` files. So one fetch after the download sees the new
                    // palette and it selects in a single click, like every other script role.
                    mlCatalog = null;               // it is local now: drop the cached list
                    // Match on the FILENAME, extension included: the device publishes each
                    // scripted palette under its file name (`spectrum.mlp`), and only the
                    // picker's displayName drops the extension. Comparing the stripped form
                    // never matched, so a download that worked ended in an error.
                    const findCtrl = () => {
                        const mod = allModules().find(m => m.name === moduleName);
                        return mod && Array.isArray(mod.controls)
                             && mod.controls.find(x => x.name === ctrl.name);
                    };
                    await refetchState();
                    const idx = ((findCtrl() || {}).options || [])
                                  .findIndex(o => o.name === file);
                    if (idx < 0) {
                        // Only reachable if the device accepted the write and then did not
                        // list the file, which is a device-side fault rather than a wait.
                        alert("could not select " + file + ": the device saved it but does not list it");
                        return;
                    }
                    const fresh = queryByName(`.palette-control[data-mid="${cssEscape(moduleName)}"][data-key="${ctrl.name}"]`, "data-mid", moduleName);
                    if (fresh) fresh.dataset.value = idx;
                    // Repaint the trigger from findCtrl(), NOT paintTrigger()/opts: opts is
                    // the array captured when this control was first rendered, and the palette
                    // just downloaded is not in it (refetchState() updated state, not this
                    // closure). Reading the wrong array would paint a stray row or a blank one,
                    // so this mirrors updateModuleControls' own "palette" case: the same three
                    // fields, keyed off the freshly-fetched control.
                    const freshCtrl = ((findCtrl() || {}).options || [])[idx];
                    if (fresh && freshCtrl) {
                        const triSwatch = fresh.querySelector(".palette-trigger .palette-swatch");
                        if (triSwatch) triSwatch.style.background = paletteGradientCss(freshCtrl.colors || "");
                        const triEmoji = fresh.querySelector(".palette-trigger .palette-emoji");
                        if (triEmoji) triEmoji.textContent = (freshCtrl.live ? SCRIPTED_EMOJI : "") + (freshCtrl.tags || "");
                        const triName = fresh.querySelector(".palette-trigger .palette-name");
                        if (triName) setText(triName, freshCtrl.name || String(idx));
                    }
                    // Stamped the same as the normal (already-local) branch below: without it a
                    // WS state push landing in this window could overwrite the selection before
                    // the next render even reads it.
                    dragTs[key] = Date.now();
                    sendControl(moduleName, ctrl.name, idx);
                    return;
                }
                const i = Number(name);
                wrap.dataset.value = i;
                paintTrigger(i);
                dragTs[key] = Date.now();
                sendControl(moduleName, ctrl.name, i);
            },
        });
    };

    trigger.addEventListener("click", openList);

    wrap.append(trigger);
    row.appendChild(wrap);
    // Reset to the default palette like every other persisted control: re-paint the trigger
    // (sendControl is handled by appendResetButton). No list to re-mark: the picker builds
    // its rows fresh each time it opens, from the current value.
    appendResetButton(row, moduleName, ctrl, def, () => {
        wrap.dataset.value = def;
        paintTrigger(def);
    });
    return row;
}

/// The script picker: the file list, the inline editor, and the fork handling that decides
/// whether a save shadows a shipped script. The largest single control by far, and the reason
/// createControl had grown to 1252 lines.
function buildFilePathControl(row, label, key, moduleName, ctrl) {
    // A file NAME plus an editor for that file's contents. The value travels through
    // /api/control like any text control; the BODY never does (it cannot: only /api/file
    // may exceed the request buffer), so the pane below reads and writes it directly.
    //
    // `dir` and `ext` come from the module that declared the control, so nothing here knows
    // what kind of file this is.
    const dir = ctrl.dir || "";
    const ext = ctrl.ext || "";
    // No `dir` means the name IS the path: joinFsPath("", n) would return "/n" and point
    // at the filesystem root instead of the file the module named.
    const pathOf = (n) => (n ? (dir ? joinFsPath(dir, n) : n) : "");
    // Where a script actually IS, which is not always `dir`: a factory script sits in the
    // catalog's directory until an edit forks it into the user's. The device resolves the
    // same way (user copy first), so the editor has to look in both or it would open an
    // empty box for a script that is plainly listed.
    const scriptPathOf = async (n) => {
        if (!n || !dir) return pathOf(n);
        const local = joinFsPath(dir, n);
        if (!mlGroupForExt(ext)) return local;
        try {
            const here = await fmFetchDir(dir).catch(() => []);
            if (here.some(e => !e.isDir && e.name === n)) return local;
            const cat = await mlFetchCatalog();
            return joinFsPath(cat.dir, n);
        } catch (_) { return local; }
    };

    const stack = document.createElement("div");
    stack.className = "control-fileedit-stack";
    row.appendChild(stack);

    const bar = document.createElement("div");
    bar.className = "fileedit-bar";

    // A select-SHAPED button, opening the shared picker. It reads as a select (the current
    // name, then the ⌄ affordance) and behaves as one, but the list it opens is the same
    // widget the module picker uses: search, emoji chips, keyboard, one row per script.
    // A native <select> can only render plain text, so a script's emoji and dimension had
    // nowhere to go.
    //
    // It presents the SAME surface a <select> does (`value`, `options`, `disabled`, and a
    // `change` event), so everything around it (the fork/share/delete labels, the editor
    // load, the download-on-pick) is unchanged and unaware.
    const picker = document.createElement("button");
    picker.type = "button";
    picker.className = "control-select fileedit-pick";
    picker.dataset.mid = moduleName;
    picker.dataset.key = ctrl.name;
    // A READ-ONLY filepath names the file something else chose, so it must not offer a
    // second way to choose: the Drivers palette editor is the case, where `palette` owns the
    // selection and this pane only edits what that selection resolved to. Two selectors for
    // one value is how they end up disagreeing.
    if (ctrl.readonly) { picker.disabled = true; picker.classList.add("is-readonly"); }
    // The options, as data. `fillPicker` appends option elements exactly as it did to the
    // <select>; they are never rendered, they are the list the modal is built from.
    picker.options = [];
    picker.appendChild = (o) => { picker.options.push(o); return o; };
    const paintPicker = () => {
        const cur = picker.options.find(o => o.value === picker._value);
        picker.textContent = cur ? cur.textContent : "(none)";
        const caret = document.createElement("span");
        caret.className = "fileedit-pick-caret";
        caret.textContent = "\u2304";        // ⌄, the select affordance
        picker.append(caret);
    };
    Object.defineProperty(picker, "value", {
        get: () => picker._value ?? "",
        set: (v) => { picker._value = String(v ?? ""); paintPicker(); },
    });
    picker._value = "";
    // innerHTML = "" is how fillPicker clears the list; keep that meaning.
    Object.defineProperty(picker, "innerHTML", {
        set: (v) => { if (v === "") { picker.options = []; picker.replaceChildren(); } },
        get: () => "",
    });
    // Which scripts this picker can offer that are not on the device yet. Names only:
    // picking one downloads it. Empty for a filepath control that is not a script picker.
    let remote = [];
    // Local names that also exist in the catalog: a user edit shadowing a factory script.
    let forks = new Set();
    // Every name the catalog ships for this role, whether or not it is on the device.
    let catalogNames = new Set();
    // Names in the USER's directory: written or edited here, so worth proposing upstream.
    let localNames = new Set();
    const fillPicker = async () => {
        picker.innerHTML = "";
        const none = document.createElement("option");
        none.value = ""; none.textContent = "(none)";
        picker.appendChild(none);
        let names = [];
        if (dir) {
            try {
                const entries = await fmFetchDir(dir);
                names = entries.filter(e => !e.isDir && (!ext || e.name.endsWith(ext)))
                               .map(e => e.name);
            } catch (_) { /* an unreachable directory leaves just "none" */ }
        }
        // The user's OWN files, before the factory listing is merged in below: a name here
        // is something they wrote or edited, which is what the share button offers.
        localNames = new Set(names);
        // A script picker also lists the FACTORY directory, where downloads land. A name in
        // both is the user's edit shadowing the factory copy, which is what the device
        // resolves too, so it appears once.
        const group = mlGroupForExt(ext);
        let cat = null;
        if (group) {
            try {
                cat = await mlFetchCatalog();
                const factory = await fmFetchDir(cat.dir, true).catch(() => []);
                for (const e of factory)
                    if (!e.isDir && e.name.endsWith(ext) && !names.includes(e.name))
                        names.push(e.name);
            } catch (_) { /* no catalog: the picker still lists what is here */ }
        }
        names.sort();
        // A local name that ALSO exists in the catalog is a fork: the user edited a factory
        // script, so their copy shadows one that can be restored. Deleting it is a revert,
        // not a loss, and the delete button says so.
        // localNames, NOT names: by here `names` also carries the factory listing, so a
        // script that was downloaded and never touched counted as a fork. It showed the
        // revert arrow for an edit that does not exist, and reverting it deleted a
        // /moonlive path with nothing at it.
        forks = cat ? new Set(((cat[group] || {}).names || []).filter(n => localNames.has(n)))
                    : new Set();
        // Everything the catalog offers that is not here yet, listed after the local ones
        // so a user's own scripts stay at the top of the list.
        remote = cat ? ((cat[group] || {}).names || []).filter(n => !names.includes(n)) : [];
        // Every name the library ships for this role, downloaded or not: what the share
        // button uses to tell a user's own script from one of ours.
        catalogNames = new Set(cat ? ((cat[group] || {}).names || []) : []);

        // The current value may name a file the listing does not have (deleted underneath,
        // or a directory that could not be read). Keep it selectable so the card still
        // shows what the module is pointing at, rather than silently appearing unset.
        const cur = String(ctrl.value ?? "");
        if (cur && !names.includes(cur) && !remote.includes(cur)) names.unshift(cur);
        // What the catalog says each factory script is: its dimension and its own emoji,
        // read from the script's `int dimensions()` / `string tags()` at build time. So a
        // row reads like the module picker's rows do, BEFORE the script is downloaded. A
        // script the catalog does not carry (the user's own) simply has no prefix.
        const decl = (n) => {
            const g = cat && cat[group];
            if (!g || !g.names) return "";
            const i = g.names.indexOf(n);
            if (i < 0) return "";
            const marks = [];
            if (g.tags && g.tags[i]) marks.push(g.tags[i]);
            if (g.dim && DIM_EMOJI[g.dim[i]]) marks.push(DIM_EMOJI[g.dim[i]]);
            return marks.length ? marks.join("") + " " : "";
        };
        for (const n of names) {
            const o = document.createElement("option");
            o.value = n; o.textContent = decl(n) + n;
            picker.appendChild(o);
        }
        // Marked, because picking one costs a download and can fail. One list rather than
        // two groups: to the user it is one library, and where a script happens to live is
        // the device's business.
        for (const n of remote) {
            const o = document.createElement("option");
            o.value = n;
            o.textContent = "\u2601 " + decl(n) + n;   // cloud: not on this device yet
            picker.appendChild(o);
        }
        picker.value = cur;
        refreshDelLabel();
    };

    // Save sits with the other file actions rather than in a row of its own: the card is
    // already narrow, and the dot on it is what marks unsaved work.
    const saveBtn = document.createElement("button");
    saveBtn.className = "card-btn fm-editor-save fileedit-glyph-lg";
    // U+2398, the ISO "store" symbol. Not an arrow: ↥ and ↧ already mean upload and
    // download here, and ⤓ downloads a file in the tree, so an arrow would read as
    // "fetch this" on a button that writes. Not ✓ either, which is the ARMED DELETE
    // state one button along.
    saveBtn.textContent = "⎘";
    saveBtn.title = "Save (or click away, or Ctrl/Cmd+S)";

    // The same modal the File Manager opens from a file row: one editor, reached two ways,
    // so a script that needs room gets the full-size box without a second implementation.
    const popBtn = document.createElement("button");
    popBtn.className = "card-btn fileedit-glyph-lg";
    popBtn.textContent = "⤢";                  // expand, the usual glyph for a bigger view
    popBtn.title = "Open in a larger window";

    // No second status line: the module's own `status` control already reports what the
    // save produced ("2036 B", or the parse error), and it is the authoritative one because
    // the DEVICE writes it. A browser-side copy said the same thing in different words and
    // could only ever disagree. What the browser knows and the device cannot (unsaved work,
    // a save in flight, a failed write) rides the Save button instead: its dot, its
    // disabled state, and its tooltip.
    const statusEl = document.createElement("span");
    statusEl.hidden = true;

    const newBtn = document.createElement("button");
    newBtn.className = "card-btn";
    newBtn.textContent = "+";                  // the same + the module tree adds with
    newBtn.title = "New script";
    const delBtn = document.createElement("button");
    delBtn.className = "card-btn card-btn-del";
    delBtn.textContent = "×";                  // the card's own delete, red on the symbol
    delBtn.title = "Delete this script";
    // The SAME button reverts a factory script, because it is the same operation: the
    // editor only ever saves to the user directory, so an edited factory script is a second
    // file shadowing the first, and removing it brings the original back. Saying "delete"
    // there would misdescribe it, and a second button would make one act look like two.
    const shareBtn = document.createElement("button");
    shareBtn.className = "card-btn";
    shareBtn.textContent = "\u2197";           // north-east arrow: it leaves for somewhere else
    shareBtn.title = "Propose this script for the shared library";

    function refreshDelLabel() {
        const isFork = forks.has(picker.value);
        delBtn.textContent = isFork ? "\u21ba" : "\u00d7";   // undo arrow, or the delete cross
        delBtn.title = isFork
            ? "Revert to the shipped version (discards your changes)"
            : "Delete this script";
        delBtn.classList.toggle("card-btn-del", !isFork);
        // Offered for anything the user WROTE, which is a script of their own or a fork of
        // a shipped one: both are a change worth sending back, and the flow differs only in
        // which GitHub URL it opens. NOT offered for an untouched factory copy, where the
        // file on the device is byte-identical to the one in the repo and a pull request
        // would propose no change at all.
        const known = catalogNames.has(picker.value);
        const edited = localNames.has(picker.value);   // it sits in the USER directory
        shareBtn.hidden = !picker.value || !mlGroupForExt(ext) || !edited;
        shareBtn.title = known
            ? "Propose your changes to the shared library"
            : "Propose this script for the shared library";
    }
    // Share: open a pull request adding this script to the library.
    //
    // GitHub's "new file" URL takes the path and the contents as query parameters and opens
    // its editor pre-filled, forking the repo on the user's behalf when they propose it. So
    // a script someone wrote on their own device reaches the library with one click and no
    // API, no token and nothing stored here.
    //
    // Only for scripts a user WROTE: a factory script is already in the library, and a fork
    // of one would open a PR that recreates a file that exists.
    shareBtn.addEventListener("click", async () => {
        const name = picker.value;
        const group = mlGroupForExt(ext);
        if (!name || !group) return;
        await editor.save();                  // propose what is on screen, not the last save
        // A save that failed leaves the pane dirty, and the read below would then fetch the
        // PREVIOUS text from the device: the user would be proposing something other than
        // what they are looking at, which is the one outcome worth refusing outright.
        if (editor.isDirty()) {
            alert("Save the script first: it still has unsaved changes.");
            return;
        }
        let text = "";
        try {
            const res = await fetch("/api/file?path=" + encodeURIComponent(await scriptPathOf(name)));
            if (!res.ok) throw new Error(await errorMessage(res));
            text = await res.text();
        } catch (err) { alert("could not read the script: " + err.message); return; }

        const cat = await mlFetchCatalog().catch(() => null);
        const folder = cat && cat[group] ? cat[group].folder : group;
        // A name the library already ships is an EDIT of that file; anything else is a new
        // one. GitHub has a flow for each, and both fork on the user's behalf when they
        // propose the change, so neither needs write access to this repo.
        const repoPath = "moonlive/" + folder + "/" + name;
        const url = catalogNames.has(name)
            ? "https://github.com/MoonModules/projectMM/edit/main/" + repoPath
              + "?value=" + encodeURIComponent(text)
            : "https://github.com/MoonModules/projectMM/new/main"
              + "?filename=" + encodeURIComponent(repoPath)
              + "&value=" + encodeURIComponent(text);
        // The script rides in the query string, and browsers stop honoring a URL somewhere
        // past ~8 KB. Every shipped script is under 2.5 KB so this is headroom rather than a
        // real limit, but a long one would otherwise open a truncated editor and look fine.
        if (url.length > 7000) {
            await navigator.clipboard.writeText(text).catch(() => {});
            alert("This script is too long to send through a link.\n\n"
                + "It has been copied to your clipboard: open\n"
                + "github.com/MoonModules/projectMM, add a file under moonlive/" + folder
                + "/ and paste it there.");
            return;
        }
        window.open(url, "_blank", "noopener");
    });

    bar.appendChild(picker);
    const tools = document.createElement("div");
    tools.className = "fileedit-tools";
    tools.appendChild(saveBtn);
    tools.appendChild(popBtn);
    if (dir) { tools.appendChild(newBtn); tools.appendChild(shareBtn); tools.appendChild(delBtn); }
    bar.appendChild(tools);
    stack.appendChild(bar);

    const pane = document.createElement("div");
    pane.className = "control-fileedit";
    stack.appendChild(pane);

    // Saving re-derives on the device: a written file asks the tree to re-prepare, so the
    // module recompiles or reloads on its own. The browser sends nothing extra.
    const editor = fmMountEditor(pane, pathOf(ctrl.value), {
        sizeKey: key,
        // The status this module is ALREADY reporting, so a card built while its script is
        // broken shows the marked line straight away rather than waiting for a recompile.
        // The EDITOR applies it once the file has loaded: marking at construction would
        // convert the offset against an empty textarea and put every error on line 1.
        initialStatus: (findModule(moduleName) || {}).status || "",
        saveButton: saveBtn,
        statusEl,
        // Editing a factory script FORKS it: the read came from the library directory, but
        // the write goes to the user's, so the shipped copy stays untouched and the new one
        // shadows it. Without this an edit overwrote the library copy and there was nothing
        // left to revert to.
        savePath: (readPath) => {
            if (!dir) return readPath;
            const base = readPath.slice(readPath.lastIndexOf("/") + 1);
            return base ? joinFsPath(dir, base) : readPath;
        },
        // A save may have just created the fork, so what the picker thinks is local is out
        // of date: re-read it, which is also what turns the delete button into revert.
        onSaved: (written) => {
            if (!mlGroupForExt(ext)) return;
            if (!written.startsWith(dir + "/")) return;
            const sel = picker.value;
            fillPicker().then(() => { picker.value = sel; refreshDelLabel(); });
        },
    });
    mlEditorAdd(moduleName, editor);
    // Apply the status the card is ALREADY showing: registration only catches the next one,
    // so a card rendered while its script is broken would report the error with no line
    // marked until something recompiled. The modal does the same on open.
    {
        const row = (queryByName(`[data-status-mid="${cssEscape(moduleName)}"]`, "data-status-mid", moduleName)
            || {}).querySelector?.(".status-value");
        if (row) editor.markError(row.textContent);
    }

    // Re-read after the modal closes: it edits the same file through the same endpoints, so
    // whatever it saved is what this pane should now show.
    // One row per script, shaped like a type so the shared picker can render it: the name is
    // what the control stores, the dimension and emoji come from the catalog, and the role
    // is what the picker prints on the right.
    const openScriptPicker = async () => {
        if (picker.disabled) return;
        const group = mlGroupForExt(ext);
        const cat = group ? await mlFetchCatalog().catch(() => null) : null;
        const g = (cat && cat[group]) || {};
        const roleWord = group ? group.replace(/s$/, "") : "file";
        const seen = new Set();
        const items = [];
        const add = (n, remoteFlag) => {
            if (seen.has(n)) return;
            seen.add(n);
            const i = (g.names || []).indexOf(n);
            items.push({
                name: n,
                // The cloud marks a script the device does not hold yet: picking it costs a
                // download, which is worth knowing before choosing.
                displayName: (remoteFlag ? "\u2601 " : "") + n,
                role: roleWord,
                tags: i >= 0 && g.tags ? (g.tags[i] || "") : "",
                dim: i >= 0 && g.dim ? g.dim[i] : 0,
            });
        };
        for (const o of picker.options) if (o.value) add(o.value, remote.includes(o.value));
        for (const n of (g.names || [])) add(n, !localNames.has(n) && remote.includes(n));
        if (!items.length) return;
        // Anchored to the field itself: the picker opens under it as a modal, sized and
        // placed by openPicker rather than by whatever element it hangs from.
        openPicker(picker, {
            items,
            actionLabel: "use",
            currentType: picker.value,
            // Route through the <select>'s own change handler, which already downloads a
            // remote script, updates the delete label and loads the editor. One path for
            // both ways of choosing.
            commit: (name) => {
                picker.value = name;
                picker.dispatchEvent(new Event("change"));
            },
        });
    };
    picker.addEventListener("click", openScriptPicker);

    popBtn.addEventListener("click", async () => {
        if (!picker.value) return;
        // Flush unsaved edits first: the modal loads the file from the device, so opening
        // it on a dirty pane would show stale bytes and then save them back over the edit.
        // save() RESOLVES on a failed write (it reports, it does not throw), so the flush is
        // only trustworthy if the pane came clean: opening anyway would discard the edit.
        await editor.save();
        if (editor.isDirty()) { alert("Not opening: this script still has unsaved changes."); return; }
        const p = await scriptPathOf(picker.value);
        await openFileEditor(p, undefined, moduleName);
        await editor.load(p);
    });

    picker.addEventListener("change", async () => {
        // Same reason as the modal above: switching files discards the edit otherwise, and
        // a save that failed leaves the pane dirty while resolving normally.
        await editor.save();
        if (editor.isDirty()) {
            alert("Not switching: this script still has unsaved changes.");
            picker.value = String(ctrl.value ?? "");
            return;
        }
        const chosen = picker.value;
        // A factory script the device does not hold yet: download it BEFORE selecting it,
        // so the module never points at a file that is not there. A failure reports and
        // puts the picker back, rather than leaving the card pointing at nothing.
        if (remote.includes(chosen)) {
            const previous = String(ctrl.value ?? "");
            picker.disabled = true;
            try {
                await mlDownloadScript(chosen, mlGroupForExt(ext));
            } catch (e) {
                picker.disabled = false;
                alert("could not download " + chosen + ": " + (e && e.message ? e.message : e));
                picker.value = previous;
                return;
            }
            picker.disabled = false;
            await fillPicker();          // it is local now, so it loses its marker
            picker.value = chosen;
        }
        refreshDelLabel();
        dragTs[key] = Date.now();
        sendControl(moduleName, ctrl.name, chosen);
        editor.load(await scriptPathOf(chosen));
    });

    newBtn.addEventListener("click", async () => {
        let name = (prompt("New file name in " + dir + ":") || "").trim();
        if (!name) return;
        if (ext && !name.endsWith(ext)) name += ext;    // a name without its extension is a typo
        const r = await fmCreateFile(dir, name, ctrl.tmpl || "");
        if (!r.ok) { alert("create file failed: " + r.message); return; }
        await fillPicker();
        picker.value = name;
        dragTs[key] = Date.now();
        sendControl(moduleName, ctrl.name, name);
        await editor.load(pathOf(name));
        editor.textarea.focus();
    });

    // Two clicks to delete, the same arm-then-confirm the File Manager uses for its own
    // delete: destructive next to frequent is how people lose work.
    armPressTwice(delBtn, async () => {
        const victim = picker.value;
        if (!victim) return;
        const wasFork = forks.has(victim);
        try {
            // A fork is deleted from the USER directory, which is the whole revert: the
            // factory copy underneath is what resolves afterwards. Anything else is deleted
            // where it actually sits, because a downloaded factory script has no user copy
            // and a DELETE on /moonlive/<name> would report a failure for a file that was
            // never there.
            const target = wasFork ? pathOf(victim) : await scriptPathOf(victim);
            const res = await fetch("/api/dir?path=" + encodeURIComponent(target),
                                    { method: "DELETE" });
            if (!res.ok) throw new Error(await errorMessage(res));
        } catch (err) {
            alert((wasFork ? "revert failed: " : "delete failed: ") + err.message);
            return;
        }
        await fillPicker();
        if (wasFork) {
            // The factory script is what resolves now, so the module keeps running: stay on
            // it rather than unsetting the control, which is the whole point of a revert.
            picker.value = victim;
            refreshDelLabel();
            dragTs[key] = Date.now();
            sendControl(moduleName, ctrl.name, victim);
            await editor.load(await scriptPathOf(victim));
            return;
        }
        picker.value = "";
        refreshDelLabel();
        dragTs[key] = Date.now();
        sendControl(moduleName, ctrl.name, "");
        await editor.load("");
    }, { armedText: "✓", armedTitle: "Click again to confirm" });

    // The editor mounted on the USER path above, which is right for a script the user
    // wrote and wrong for a factory one that has never been edited. Resolving needs the
    // catalog, so it cannot happen during the synchronous mount: re-point it once the
    // listing is in, and only when it actually resolves elsewhere.
    fillPicker().then(async () => {
        const cur = String(ctrl.value ?? "");
        if (!cur || !mlGroupForExt(ext)) return;
        const real = await scriptPathOf(cur);
        if (real !== pathOf(cur)) await editor.load(real);
    });
    return row;
}