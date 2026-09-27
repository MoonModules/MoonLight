// MoonDeck UI

const logEl = document.getElementById("log");
const viewFrame = document.getElementById("view-frame");
const MOONDECK_MD = "/api/help";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

let scripts = [];
let firmwares = [];
let scenarios = [];   // [{name, module, also}]
let uiClips = [];     // [{name, description}] from test/uiscenarios/clips/*.json
let uiProjects = [];  // [{name, description}] from test/uiscenarios/projects/*.json
let testModules = []; // ["CamelCaseName", ...]
// Device-model catalog loaded from /api/device-models (served by moondeck.py from
// mooninstaller/deviceModels.json) — the same file the web installer fetches. Empty until
// init() loads it; renderDevices waits on init.
let deviceModels = []; // [{ name, firmwares: [...], ... }] — `name` is the identifier + label
                       // (single-name catalog, matched by b.name); firmwares[0] is the default.
// State shape (post-networks refactor):
//   { networks: [{name, subnet, wifi: {ssid, password}, port, devices: [...]}],
//     active_network: "Home",
//     active_network_user_pinned: bool,   // set true when user picks the dropdown
//     firmware, scenario, module, tab, flag_* }
// `firmware` is the variant flashed onto the ESP32 (esp32 / esp32-eth /
// esp32-16mb / esp32s3-n16r8) — separate from the per-device `deviceModel`
// (physical hardware) inside each network's devices list. See
// docs/explanation/architecture/mooninstaller.md § Firmware vs board.
// Devices and the active serial port now live INSIDE the active network.
// Migration from the legacy flat shape happens server-side in load_state().
let state = { networks: [], active_network: "", firmware: "", scenario: "", module: "" };

// Helper: the network record currently selected. Every read that used to
// touch state.devices or state.port now routes through this.
function getActiveNetwork() {
    return (state.networks || []).find(n => n.name === state.active_network) || null;
}

// A fetch abort signal that fires after `ms`. AbortSignal.timeout is unsupported on older browsers
// (and throws if called), which would abort the fetch before it starts — fall back to an
// AbortController + setTimeout so the request still runs.
function timeoutSignal(ms) {
    if (typeof AbortSignal !== "undefined" && AbortSignal.timeout) return AbortSignal.timeout(ms);
    const c = new AbortController();
    setTimeout(() => c.abort(), ms);
    return c.signal;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

async function init() {
    const resp = await fetch("/api/scripts");
    const data = await resp.json();
    scripts = data.scripts;
    firmwares = data.firmwares;

    const stateResp = await fetch("/api/state");
    state = await stateResp.json();

    // Migrate legacy persisted state: old saves keyed the firmware variant
    // as `state.board` (which collided with the per-device `board` field that
    // means physical hardware). Move it to `state.firmware`. Also drop the
    // value if it isn't in the new firmwares list so the default selection
    // (first firmware) wins.
    if (state.board !== undefined && state.firmware === undefined) {
        state.firmware = state.board;
        delete state.board;
    }
    if (!firmwares.includes(state.firmware)) state.firmware = "";
    // Migrate the provisioning-picker key from its old name (board → device model).
    if (state.provisionBoard !== undefined && state.provisionDeviceModel === undefined) {
        state.provisionDeviceModel = state.provisionBoard;
        delete state.provisionBoard;
    }
    // Restore the "online only" device filter from the persisted state (absent = show all).
    const onlineOnlyInit = document.getElementById("online-only");
    if (onlineOnlyInit) onlineOnlyInit.checked = !!state.onlineOnly;

    const scenResp = await fetch("/api/scenarios");
    const scenData = await scenResp.json();
    scenarios = scenData.scenarios || [];

    // The UI video runs. Failing soft: a bench without them still gets every other
    // card, and an empty dropdown says so plainly.
    try {
        uiClips = (await (await fetch("/api/uiclips")).json()).uiclips || [];
        uiProjects = (await (await fetch("/api/uiprojects")).json()).uiprojects || [];
    } catch {
        uiClips = [];
        uiProjects = [];
    }

    const modResp = await fetch("/api/test-modules");
    const modData = await modResp.json();
    testModules = modData.modules || [];

    try {
        const modelsResp = await fetch("/api/device-models");
        const modelsData = await modelsResp.json();
        deviceModels = modelsData.deviceModels || [];
    } catch {
        deviceModels = [];   // empty catalog → picker shows only "(unknown model)"
    }

    renderFirmwareSelect();
    renderDeviceModelSelect();
    renderScripts();
    renderNetworkBar();
    try { renderDevices(); } catch (e) { console.error("renderDevices:", e); }
    await updateRunningState();
    refreshPorts();
    setupTabs();
    setupPaneTabs();
    setupNetworkBar();
}

async function updateRunningState() {
    try {
        const resp = await fetch("/api/running");
        const running = await resp.json();
        for (const [scriptId, isRunning] of Object.entries(running)) {
            const btn = document.querySelector(`.run-btn[data-id="${scriptId}"]`);
            if (!btn) continue;
            const dot = document.querySelector(`.status-dot[data-id="${scriptId}"]`);
            if (isRunning && !btn.classList.contains("running")) {
                btn.classList.add("running");
                btn.textContent = "Stop";
                if (dot) dot.className = "status-dot running";
            } else if (!isRunning && btn.classList.contains("running")) {
                btn.classList.remove("running");
                btn.textContent = "Run";
                if (dot) dot.className = "status-dot";
            }
        }
    } catch {
        // ignore — non-critical
    }
}

// Poll running state every 5 seconds
setInterval(updateRunningState, 5000);

// ---------------------------------------------------------------------------
// Tabs (sidebar)
// ---------------------------------------------------------------------------

function setupTabs() {
    // ?tab=<name> URL param overrides saved state (used by screenshot automation)
    const urlTab = new URLSearchParams(location.search).get("tab");
    const activeTab = urlTab || state.tab;
    if (activeTab) {
        document.querySelectorAll(".tab").forEach(b => b.classList.remove("active"));
        document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
        const btn = document.querySelector(`.tab[data-tab="${activeTab}"]`);
        const content = document.getElementById("tab-" + activeTab);
        if (btn && content) {
            btn.classList.add("active");
            content.classList.add("active");
        }
        applyNetworkBarVisibility(activeTab);
    }

    document.querySelectorAll(".tab").forEach(btn => {
        btn.addEventListener("click", () => {
            document.querySelectorAll(".tab").forEach(b => b.classList.remove("active"));
            document.querySelectorAll(".tab-content").forEach(s => s.classList.remove("active"));
            btn.classList.add("active");
            document.getElementById("tab-" + btn.dataset.tab).classList.add("active");
            state.tab = btn.dataset.tab;
            saveState();
            applyNetworkBarVisibility(btn.dataset.tab);
        });
    });
}

// The network bar (selector + WiFi panel) only matters when the workflow
// involves a device on the LAN — ESP32 tab uses the active network's port
// + WiFi creds (Improv), Live tab uses the device list. The Desktop tab runs on
// localhost and has no network concept; hide the bar so it doesn't add noise.
function applyNetworkBarVisibility(tab) {
    const bar = document.getElementById("network-bar");
    if (!bar) return;
    bar.style.display = (tab === "esp32" || tab === "live") ? "" : "none";
}

// ---------------------------------------------------------------------------
// Pane tabs (main area: Log / View)
// ---------------------------------------------------------------------------

function setupPaneTabs() {
    document.querySelectorAll(".pane-tab").forEach(btn => {
        btn.addEventListener("click", () => {
            switchPane(btn.dataset.pane);
        });
    });
}

const viewNav = document.getElementById("view-nav");
const clearLogBtn = document.getElementById("clear-log");

function switchPane(pane) {
    document.querySelectorAll(".pane-tab").forEach(b => b.classList.remove("active"));
    document.querySelectorAll(".pane-content").forEach(p => p.classList.remove("active"));
    document.querySelector(`.pane-tab[data-pane="${pane}"]`).classList.add("active");
    document.getElementById("pane-" + pane).classList.add("active");
    viewNav.hidden = (pane !== "view");
    clearLogBtn.hidden = (pane !== "log");
}

function viewNavAction(fn) {
    try { fn(viewFrame.contentWindow); } catch (_) {}
}
document.getElementById("view-back").addEventListener("click", () => viewNavAction(w => w.history.back()));
document.getElementById("view-forward").addEventListener("click", () => viewNavAction(w => w.history.forward()));
document.getElementById("view-refresh").addEventListener("click", () => {
    if (viewFrame.src) viewFrame.src = viewFrame.src;
});
// Open the currently-viewed URL (the device UI) in a real browser tab — escapes the sandboxed
// iframe entirely, so anything the frame restricts (modals, popups, downloads) works natively.
document.getElementById("view-open").addEventListener("click", () => {
    if (viewFrame.src) window.open(viewFrame.src, "_blank", "noopener");
});

function showInView(url) {
    viewFrame.src = url;
    switchPane("view");
}

window.addEventListener("message", (e) => {
    if (e.source !== viewFrame.contentWindow) return;  // only accept from our iframe
    if (e.data?.type === "moondeck-nav" && typeof e.data.url === "string"
            && e.data.url.startsWith("/api/")) {
        showInView(e.data.url);
    }
});

// ---------------------------------------------------------------------------
// Script cards
// ---------------------------------------------------------------------------

// How long a script takes, as one emoji before the buttons — so you can tell at a glance
// whether clicking costs a second or a coffee. `speed` comes from moondeck_config.json.
// All three tiers are shown: a blank would be ambiguous between "medium" and "nobody set a
// speed on this card", and only a visible badge makes a missing one obvious.
const SPEED_BADGE = {
    instant: { icon: "⚡", title: "Instant — runs in about a second" },
    medium:  { icon: "⏱️", title: "Medium — a few seconds up to about 30" },
    slow:    { icon: "🐌", title: "Slow — takes more than 30 seconds" },
};

// Replay a script's stored last run into the log pane. Shared by the button rendered at load
// and the one created when a first run completes.
let logLoadToken = 0;

async function showLastRun(script) {
    switchPane("log");
    // Clear BEFORE the fetch, and token the request: clicking two cards quickly must not let
    // the slower response paint over the log you actually asked for last.
    const token = ++logLoadToken;
    logEl.textContent = "";
    appendLog(`— loading last run for ${script.label} —`);
    try {
        const r = await fetch(`/api/log/${script.id}`);
        if (token !== logLoadToken) return;          // superseded by a later click
        logEl.textContent = "";
        appendLog(r.ok ? await r.text() : `— no stored run for ${script.label} —`);
    } catch (err) {
        if (token !== logLoadToken) return;
        logEl.textContent = "";
        appendLog(`— could not read log: ${err} —`);
    }
}

function speedBadge(speed) {
    const b = SPEED_BADGE[speed];
    return b ? `<span class="speed-badge" title="${b.title}">${b.icon}</span>` : "";
}

// Build a module-filter row: <select> with "all modules" + every test module,
// plus a Tests button that opens the per-module unit-test list. Used both for
// the shared row above a group and the per-card row inside a script-card.
// `onChange(mod)` fires after state.module is saved so the caller can refresh
// dependent scenario dropdowns.
function buildModuleRow({ rowClass, onChange }) {
    const row = document.createElement("div");
    row.className = rowClass;
    row.innerHTML = `
        <select class="module-select" title="Filter by module"></select>
        <button class="tests-btn" title="Show the selected module's unit tests">Tests</button>
    `;
    const modSel = row.querySelector(".module-select");
    const testsBtn = row.querySelector(".tests-btn");
    const allOpt = document.createElement("option");
    allOpt.value = "";
    allOpt.textContent = "all modules";
    modSel.appendChild(allOpt);
    for (const name of testModules) {
        const opt = document.createElement("option");
        opt.value = name;
        opt.textContent = name;
        if (name === state.module) opt.selected = true;
        modSel.appendChild(opt);
    }
    // Tests button is meaningful only for a specific module — same shape as
    // Steps for scenarios. Disabled when "all modules" is selected.
    const syncTestsBtn = () => { testsBtn.disabled = !modSel.value; };
    syncTestsBtn();
    modSel.addEventListener("change", () => {
        state.module = modSel.value;
        saveState();
        syncTestsBtn();
        if (onChange) onChange(state.module);
    });
    testsBtn.addEventListener("click", () => {
        if (modSel.value) showInView("/api/unit-tests/" + encodeURIComponent(modSel.value));
    });
    return row;
}

function renderScripts() {
    const containers = {
        desktop: document.getElementById("scripts-desktop"),
        esp32: document.getElementById("scripts-esp32"),
        live: document.getElementById("scripts-live"),
    };
    // The esp32 tab splits its scripts across three containers so the
    // dropdowns (Firmware, Port) can sit *between* the script groups they
    // belong to. Setup → top, Build → after the Firmware dropdown, Flash +
    // run → after the Port dropdown (the main container). Every other tab
    // uses a single container.
    const esp32SetupContainer = document.getElementById("scripts-esp32-setup");
    const esp32BuildContainer = document.getElementById("scripts-esp32-build");

    for (const [tab, container] of Object.entries(containers)) {
        container.innerHTML = "";
        if (tab === "esp32") {
            esp32SetupContainer.innerHTML = "";
            esp32BuildContainer.innerHTML = "";
        }
        const tabScripts = scripts.filter(s => s.tab === tab);

        // Per-container last-group tracking: each target keeps its own
        // header state so we don't suppress a header just because the same
        // group name appeared in the *other* container.
        const lastGroupByTarget = new WeakMap();
        // Per-(tab,group) shared module row: when >=2 cards in the same group
        // declare needs_module, we render ONE module dropdown at the top of the
        // group and skip the per-card module rows. groupModSelects[key] holds
        // the scenario-select repopulate callbacks the shared dropdown drives.
        const groupModSelects = {};
        const groupKey = (script, target) => `${tab}::${script.group}::${target === container ? "main" : "side"}`;
        const sharedModuleGroup = (script, target) => {
            const k = groupKey(script, target);
            return (groupModSelects[k] && groupModSelects[k].count >= 2) ? k : null;
        };

        // First pass: count needs_module cards per (tab,group,target) so we
        // know whether to render a shared module row when the group opens.
        for (const script of tabScripts) {
            let target = container;
            if (tab === "esp32") {
                if (script.group === "setup") target = esp32SetupContainer;
                else if (script.group === "build") target = esp32BuildContainer;
            }
            if (!script.needs_module) continue;
            const k = groupKey(script, target);
            if (!groupModSelects[k]) groupModSelects[k] = { count: 0, repopulators: [] };
            groupModSelects[k].count++;
        }

        for (const script of tabScripts) {
            let target = container;
            if (tab === "esp32") {
                if (script.group === "setup") target = esp32SetupContainer;
                else if (script.group === "build") target = esp32BuildContainer;
            }
            const lastGroup = lastGroupByTarget.get(target) || "";
            if (script.group !== lastGroup) {
                lastGroupByTarget.set(target, script.group);
                const header = document.createElement("div");
                header.className = "group-header";
                header.textContent = script.group;
                target.appendChild(header);
                // If this group has a shared module dropdown, render it now —
                // right under the header, before any card in the group.
                const sharedKey = sharedModuleGroup(script, target);
                if (sharedKey) {
                    const row = buildModuleRow({
                        rowClass: "shared-module-row",
                        onChange: (mod) => {
                            // Re-populate every scenario select wired to this group.
                            for (const repop of groupModSelects[sharedKey].repopulators) {
                                repop(mod);
                            }
                        },
                    });
                    target.appendChild(row);
                }
            }

            const usesSharedModule = !!sharedModuleGroup(script, target);
            const renderOwnModuleRow = script.needs_module && !usesSharedModule;
            const card = document.createElement("div");
            const needsUiRun = script.needs_uiclip || script.needs_uiproject;
            const hasExtras = script.needs_scenario || needsUiRun || renderOwnModuleRow || (script.flags && script.flags.length > 0);
            card.className = "script-card" + (hasExtras ? " script-card--has-select" : "");
            card.innerHTML = `
                <div class="card-row">
                    <span class="status-dot" data-id="${script.id}"></span>
                    <span class="label">${script.label}</span>
                    ${speedBadge(script.speed)}
                    ${script.hasLog ? `<button class="log-btn" title="Show this script's last run" aria-label="Show this script's last run">📄</button>` : ""}
                    <button class="help-btn" title="Help">?</button>
                    <button class="run-btn" data-id="${script.id}">Run</button>
                </div>
                ${script.needs_scenario ? `<div class="scenario-row">
                    <select class="scenario-select"></select>
                    <button class="steps-btn" title="Show the selected scenario's steps">Steps</button>
                </div>` : ""}
                ${needsUiRun ? `<div class="scenario-row">
                    <select class="uirun-select" aria-label="${script.needs_uiclip ? "Clip to record" : "Project to cut"}" title="${script.needs_uiclip ? "Which UI clip to record" : "Which video project to cut"}"></select>
                </div>` : ""}
                ${script.flags && script.flags.length > 0 ? `<div class="flag-row"></div>` : ""}
            `;

            // Helper: which scenarios apply to the currently-selected module?
            const filterScenariosByModule = (mod) => {
                if (!mod) return scenarios;
                return scenarios.filter(s => s.module === mod || (s.also || []).includes(mod));
            };

            // Per-card module dropdown — only when this card isn't covered by
            // a shared module row above its group. Inserted between the card-row
            // and the scenario-row (if any) using the same builder the shared
            // row uses, so the two cases stay visually and behaviourally identical.
            if (renderOwnModuleRow) {
                const row = buildModuleRow({
                    rowClass: "module-row",
                    onChange: (mod) => {
                        const scenSel = card.querySelector(".scenario-select");
                        if (scenSel) repopulateScenarioSelect(scenSel, mod);
                    },
                });
                card.insertBefore(row, card.children[1] || null);
            }

            const repopulateScenarioSelect = (sel, mod) => {
                const prev = sel.value;
                sel.innerHTML = "";
                const allOpt = document.createElement("option");
                allOpt.value = "";
                allOpt.textContent = "all";
                sel.appendChild(allOpt);
                let kept = false;
                for (const s of filterScenariosByModule(mod)) {
                    const opt = document.createElement("option");
                    opt.value = s.name;
                    opt.textContent = s.name;
                    if (s.name === prev) { opt.selected = true; kept = true; }
                    else if (s.name === state.scenario && !prev) { opt.selected = true; kept = true; }
                    sel.appendChild(opt);
                }
                if (!kept) {
                    // The previously-selected scenario no longer matches the module —
                    // fall back to "all" and persist so the next render is consistent.
                    state.scenario = "";
                    saveState();
                    sel.value = "";
                }
                sel.dispatchEvent(new Event("change"));
            };

            if (needsUiRun) {
                const sel = card.querySelector(".uirun-select");
                const kind = script.needs_uiclip ? "uiclip" : "uiproject";
                const list = script.needs_uiclip ? uiClips : uiProjects;
                const stateKey = kind;              // one choice per kind, shared by cards
                // DOM, not innerHTML: a run file's description is free text, and
                // escaping it by hand covered quotes and nothing else.
                sel.replaceChildren();
                if (list.length) {
                    for (const r of list) {
                        const opt = document.createElement("option");
                        opt.value = r.name;
                        opt.textContent = r.name;
                        if (r.description) opt.title = r.description;
                        sel.appendChild(opt);
                    }
                } else {
                    const opt = document.createElement("option");
                    opt.value = "";
                    opt.textContent = "(none found)";
                    sel.appendChild(opt);
                }
                if (state[stateKey] && list.some(r => r.name === state[stateKey])) {
                    sel.value = state[stateKey];
                } else if (list.length) {
                    state[stateKey] = list[0].name;
                } else {
                    // CLEAR a stale choice. Keeping it sent a name the server no longer
                    // offers, which the launcher then rejected as unknown.
                    state[stateKey] = "";
                }
                sel.addEventListener("change", () => {
                    state[stateKey] = sel.value;
                    saveState();
                });
            }

            if (script.needs_scenario) {
                const sel = card.querySelector(".scenario-select");
                const stepsBtn = card.querySelector(".steps-btn");
                repopulateScenarioSelect(sel, script.needs_module ? state.module : "");
                // Steps button is meaningful only for a specific scenario — the "all"
                // (empty) value has no single steps file to show.
                const syncStepsBtn = () => { stepsBtn.disabled = !sel.value; };
                syncStepsBtn();
                sel.addEventListener("change", () => {
                    state.scenario = sel.value;
                    saveState();
                    syncStepsBtn();
                });
                stepsBtn.addEventListener("click", () => {
                    if (sel.value) showInView("/api/scenarios/" + encodeURIComponent(sel.value));
                });
                // Register with the shared module dropdown (if any) so changing it
                // re-populates this card's scenario list.
                const sharedKey = sharedModuleGroup(script, target);
                if (sharedKey) {
                    groupModSelects[sharedKey].repopulators.push((mod) => {
                        repopulateScenarioSelect(sel, mod);
                    });
                }
            }

            if (script.flags && script.flags.length > 0) {
                const flagRow = card.querySelector(".flag-row");
                for (const flag of script.flags) {
                    const stateKey = `flag_${script.id}_${flag.id}`;
                    const checked = stateKey in state ? state[stateKey] : flag.default;
                    const label = document.createElement("label");
                    label.className = "flag-label";
                    const cb = document.createElement("input");
                    cb.type = "checkbox";
                    cb.checked = checked;
                    state[stateKey] = checked;
                    cb.addEventListener("change", () => {
                        state[stateKey] = cb.checked;
                        saveState();
                    });
                    label.appendChild(cb);
                    label.append(" " + flag.label);
                    flagRow.appendChild(label);
                }
            }

            card.querySelector(".help-btn").addEventListener("click", () => {
                showInView(MOONDECK_MD + "?" + script.help);
            });

            card.querySelector(".run-btn").addEventListener("click", (e) => {
                runScript(script, e.target);
            });

            // Its own button rather than a click on the status dot: the dot is a status
            // INDICATOR, and making it secretly clickable gave a new reader no way to guess the
            // feature existed. The server tees every stream to build/moondeck-logs/<id>.log, so
            // this answers "what did this do last time" after a page reload or a switch to
            // another card — the case a live-only stream cannot.
            card.querySelector(".log-btn")?.addEventListener("click", () => showLastRun(script));

            target.appendChild(card);
        }
    }
}

// ---------------------------------------------------------------------------
// Script execution
// ---------------------------------------------------------------------------

async function runScript(script, btn) {
    // Destructive actions (erase flash, future reset-to-defaults) require a
    // confirmation step — native browser confirm() to match MoonDeck's
    // vanilla style. Done before the long-running / live-tab branches so an
    // already-running destructive script (none exist today, future-proofing)
    // can still be stopped without re-confirming.
    if (script.destructive && !btn.classList.contains("running")) {
        if (!confirm(`${script.label} is destructive — are you sure?`)) return;
    }
    // Any running script can be stopped — long_running ones are the typical case
    // (run_desktop, monitor_esp32, …) but a foreground script that's taking longer
    // than expected (e.g. a full unit-test run) should also be killable mid-flight.
    if (btn.classList.contains("running")) {
        appendLog(`\n--- Stopping ${script.label} ---\n`);
        await fetch("/api/kill/" + script.id, { method: "POST" });
        btn.classList.remove("running");
        btn.textContent = "Run";
        const dot = document.querySelector(`.status-dot[data-id="${script.id}"]`);
        if (dot) { dot.className = "status-dot"; }
        return;
    }

    // Live tab scripts: run against each selected device in the active network.
    //
    // ONLINE ones only. A checkbox survives a device going offline (the flag is user state,
    // deliberately kept across scans), so a stale tick would otherwise send the run at a board
    // that is not there — and each one costs a ~16 s connect timeout before failing, which buries
    // the results of the boards that did run. Offline picks are reported rather than dropped
    // silently, so a device that was expected to run and did not is visible.
    if (script.tab === "live" && script.needs_device) {
        const active = getActiveNetwork();
        const picked = ((active && active.devices) || []).filter(d => d.selected);
        // `online !== false`, the same predicate the device list and the status dot use: a device
        // discovered but never probed has NO `online` field, and treating that as offline would
        // refuse to run on a board that is very likely up.
        const devices = picked.filter(d => d.online !== false);
        const skipped = picked.filter(d => d.online === false);
        if (devices.length === 0) {
            switchPane("log");
            appendLog(picked.length
                ? `\n--- No ONLINE devices selected. ${skipped.length} selected `
                  + `device(s) are offline: ${skipped.map(d => d.deviceName || d.ip).join(", ")}. `
                  + `Scan to refresh. ---\n`
                : "\n--- No devices selected. Use Discover and check devices first. ---\n");
            return;
        }
        if (skipped.length) {
            switchPane("log");
            appendLog(`\n--- Skipping ${skipped.length} offline device(s): `
                      + `${skipped.map(d => d.deviceName || d.ip).join(", ")} ---\n`);
        }
        let allPassed = true;
        for (const device of devices) {
            const ok = await runScriptOnce(script, btn, { host: device.ip });
            if (!ok) allPassed = false;
        }
        return;
    }

    await runScriptOnce(script, btn, {});
}

async function runScriptOnce(script, btn, extraParams) {
    const params = { ...extraParams };
    if (script.needs_firmware) params.firmware = state.firmware;
    if (script.needs_port) params.port = (getActiveNetwork()?.port) || "";
    if (script.needs_scenario) params.scenario = state.scenario;
    if (script.needs_uiclip) params.uiclip = state.uiclip || "";
    if (script.needs_uiproject) params.uiproject = state.uiproject || "";
    if (script.needs_module) params.module = state.module;
    if (script.pass_device_model) params.device_model = state.provisionDeviceModel || "";
    for (const flag of (script.flags || [])) {
        const stateKey = `flag_${script.id}_${flag.id}`;
        params[`flag_${flag.id}`] = stateKey in state ? state[stateKey] : flag.default;
    }

    // Switch to log pane and show output
    switchPane("log");
    btn.classList.add("running");
    // Show Stop on every running script so the user can interrupt a slow run.
    btn.textContent = "Stop";
    const hostLabel = params.host ? ` - ${params.host}` : "";
    appendLog(`\n--- ${script.label}${hostLabel} ---\n`);

    const dot = document.querySelector(`.status-dot[data-id="${script.id}"]`);
    if (dot) { dot.className = "status-dot running"; }

    function resetBtn(exitCode) {
        btn.classList.remove("running");
        btn.textContent = "Run";
        if (dot) {
            dot.className = exitCode === 0 ? "status-dot pass" : "status-dot fail";
        }
    }

    return new Promise((resolve) => {
        fetch("/api/run/" + script.id, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(params),
        }).then(r => r.json()).then(result => {
            if (result.error) {
                appendLog("Error: " + result.error + "\n");
                resetBtn(1);
                resolve(false);
                return;
            }

            const evtSource = new EventSource("/api/stream/" + script.id);

            evtSource.onmessage = (e) => {
                appendLog(JSON.parse(e.data) + "\n");
            };

            evtSource.addEventListener("done", (e) => {
                evtSource.close();
                try {
                    const data = JSON.parse(e.data);
                    resetBtn(data.exitCode);
                    // The script may launch a detached process (e.g. run_desktop
                    // spawns MoonLight and exits). Refresh the running state now
                    // instead of waiting up to 5s for the poll, so the button
                    // flips back to "Stop" without a visible blink.
                    if (script.long_running) updateRunningState();
                    // A first run just created this script's log, so the 📄 appears without
                    // needing a page reload.
                    if (!script.hasLog) {
                        script.hasLog = true;
                        const row = document.querySelector(
                            `.status-dot[data-id="${script.id}"]`)?.parentElement;
                        if (row && !row.querySelector(".log-btn")) {
                            const b = document.createElement("button");
                            b.className = "log-btn";
                            b.title = "Show this script's last run";
                            b.setAttribute("aria-label", "Show this script's last run");
                            b.textContent = "📄";
                            b.addEventListener("click", () => showLastRun(script));
                            row.insertBefore(b, row.querySelector(".help-btn"));
                        }
                    }
                    resolve(data.exitCode === 0);
                } catch {
                    resetBtn(1);
                    resolve(false);
                }
            });

            evtSource.onerror = () => {
                evtSource.close();
                resetBtn(1);
                resolve(false);
            };
        }).catch(err => {
            appendLog("Failed: " + err.message + "\n");
            resetBtn(1);
            resolve(false);
        });
    });
}

// ---------------------------------------------------------------------------
// ESP32 controls
// ---------------------------------------------------------------------------

// Device-model picker for provisioning scripts (pass_device_model): deviceModels.json entries
// whose `firmwares` include the selected firmware. "(any model)" = no
// injection. Distinct state key from the LEGACY `state.board` (which meant
// firmware — see the migration in init) and from per-device deviceModels.
function renderDeviceModelSelect() {
    const select = document.getElementById("devicemodel-select");
    if (!select) return;
    select.innerHTML = "";
    const candidates = deviceModels.filter(b => (b.firmwares || []).includes(state.firmware));
    const options = [["", "(any model)"], ...candidates.map(b => [b.name, b.name])];
    if (state.provisionDeviceModel && !options.some(([v]) => v === state.provisionDeviceModel)) {
        state.provisionDeviceModel = "";   // firmware changed; stale pick no longer applies
    }
    for (const [val, lbl] of options) {
        const opt = document.createElement("option");
        opt.value = val;
        opt.textContent = lbl;
        if (val === (state.provisionDeviceModel || "")) opt.selected = true;
        select.appendChild(opt);
    }
    // onchange (assigned, not addEventListener) so a re-render replaces the
    // handler instead of stacking another — addEventListener here would fire
    // saveState() once per past render.
    select.onchange = async () => {
        state.provisionDeviceModel = select.value;
        await saveState();
    };
}

function renderFirmwareSelect() {
    const select = document.getElementById("firmware-select");
    select.innerHTML = "";
    // If no firmware persisted (fresh state, or legacy state migrated away),
    // default to the first option so Build / etc. always have a valid
    // --firmware argument to forward.
    if (!state.firmware && firmwares.length > 0) state.firmware = firmwares[0];
    for (const fw of firmwares) {
        const opt = document.createElement("option");
        opt.value = fw;
        opt.textContent = fw;
        if (fw === state.firmware) opt.selected = true;
        select.appendChild(opt);
    }
    select.addEventListener("change", async () => {
        state.firmware = select.value;
        await saveState();
        renderDeviceModelSelect();   // device-model candidates follow the firmware
    });
}

// Show which device MoonDeck last flashed on `port` — matched by the `last_port`
// breadcrumb on the active network's devices (set at flash time, keyed per-device by
// MAC). Names the board you're about to reflash so the port isn't just an opaque
// /dev/cu.* string. Blank when the port maps to no known device.
function updatePortDeviceHint(port) {
    const hint = document.getElementById("port-device-hint");
    if (!hint) return;
    const devices = getActiveNetwork()?.devices || [];
    const dev = port ? devices.find(d => d.last_port === port) : null;
    if (!dev) { hint.textContent = ""; return; }
    const model = dev.deviceModel ? ` · ${dev.deviceModel}` : "";
    hint.textContent = `last flashed: ${dev.deviceName || dev.ip}${model}`;
}

// Last port list from /api/ports, kept so the Identify button knows which
// entries are still path-only (no board match) and worth probing.
let lastPorts = [];

function renderPortOptions(ports) {
    lastPorts = ports;
    const select = document.getElementById("port-select");
    // Port is per-network — read and persist on the active network record;
    // renderPortOptions re-runs on every network switch so the dropdown's
    // selected value follows the active network's last-used port.
    const current = getActiveNetwork()?.port || "";
    select.innerHTML = '<option value="">--</option>';
    // Each entry is {path, chip, board, ip}: three levels of identity that
    // degrade gracefully (path always; chip when the USB descriptor or registry
    // knows it; board when a registry match exists). Show the richest we have.
    for (const p of ports) {
        const opt = document.createElement("option");
        opt.value = p.path;
        // Richest identity we have: board · chip, or the "not an ESP32" note for a
        // known non-ESP device (e.g. a monitor) so it never reads as "unidentified".
        const detail = [p.board, p.chip].filter(Boolean).join(" · ") || p.note;
        opt.textContent = detail ? `${p.path} — ${detail}` : p.path;
        if (p.path === current) opt.selected = true;
        select.appendChild(opt);
    }
    // Hint the port the dropdown ACTUALLY shows: `current` only when it's still a present port,
    // else the blank selection (the "--" the select falls back to when the stored port is gone).
    const effective = ports.some(p => p.path === current) ? current : "";
    updatePortDeviceHint(effective);
    // `.onchange = ...` (not addEventListener) so the handler is REPLACED on
    // each render rather than stacking — this re-runs on every network switch +
    // every Refresh/Identify, so addEventListener would queue up duplicate
    // saveState calls per user change.
    select.onchange = async () => {
        const active = getActiveNetwork();
        if (active) active.port = select.value;
        updatePortDeviceHint(select.value);
        await saveState();
    };
}

async function refreshPorts() {
    try {
        const resp = await fetch("/api/ports");
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const data = await resp.json();
        if (!data || !Array.isArray(data.ports)) throw new Error("malformed response");
        renderPortOptions(data.ports);
    } catch (e) {
        appendLog(`\nCouldn't list ports: ${e.message || e}\n`);
    }
}

// Refreshing the port list usually changes nothing visible — the same ports come back — so
// without feedback the click looks like it did nothing and invites a second press. Spin the
// ↻ glyph for one turn: the affordance the icon already implies, and it confirms the action
// ran without adding a message to read. The spin is on the click handler rather than inside
// refreshPorts() because the other callers (after a flash, after a scan) are not user
// clicks and should not animate.
document.getElementById("refresh-ports").addEventListener("click", async (e) => {
    const btn = e.currentTarget;
    btn.classList.add("spinning");
    // Hold the spin for at least one full turn even when the fetch resolves sooner, so a
    // fast refresh still reads as a deliberate action rather than a flicker.
    await Promise.all([refreshPorts(), new Promise(r => setTimeout(r, 500))]);
    btn.classList.remove("spinning");
});

// Identify: probe EVERY ESP-capable port with esptool to read chip + MAC. This
// is a dev bench where boards move between ports constantly, so it always does a
// full probe — including already-labeled ports — because a fresh read beats a
// cache a swap may have staled. Non-ESP devices (a monitor) are skipped. Probing
// RESETS each board, so it only runs on an explicit click.
document.getElementById("identify-ports").addEventListener("click", async () => {
    const btn = document.getElementById("identify-ports");
    const targets = lastPorts.filter(p => p.probeable).map(p => p.path);
    if (!targets.length) { appendLog("\nNo ESP32-capable ports to probe.\n"); return; }
    btn.disabled = true;
    const orig = btn.textContent;
    btn.textContent = "Probing…";
    appendLog(`\n--- Identifying ${targets.length} port(s) (resets each board) ---\n`);
    try {
        const resp = await fetch("/api/identify-ports", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ ports: targets }),
        });
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const data = await resp.json();
        if (!data || !Array.isArray(data.ports)) throw new Error("malformed response");
        let matched = 0;
        for (const [path, info] of Object.entries(data.probed || {})) {
            const board = (data.ports.find(p => p.path === path) || {}).board;
            if (board) matched++;
            appendLog(`  ${path} → ${info.chip || "no chip"}${info.mac ? " (" + info.mac + ")" : ""}${board ? " = " + board : ""}\n`);
        }
        renderPortOptions(data.ports);
        appendLog(`--- Identify done: ${matched}/${targets.length} matched to a board ---\n`);
    } catch (e) {
        appendLog(`\nIdentify failed: ${e.message || e}\n`);
    } finally {
        btn.disabled = false;
        btn.textContent = orig;
    }
});

// Port watcher: a passive plug/unplug monitor. Polls /api/ports, diffs against
// the last snapshot, and narrates each change with its 3-level identity (path →
// chip → board). Read-only — the USB descriptor + registry cache carry the
// identity, so it never probes/resets a board (that's the Identify button, which
// the watcher points you to for an unidentified port). Toggles on/off.
//
// Flap detection: a cheap ESP32 dev board (e.g. a LOLIN D32) with a marginal
// regulator or a bad cable browns out and re-enumerates every poll — a wall of
// Add/Remove lines for one port. When a port crosses FLAP_LIMIT transitions in
// FLAP_WINDOW_MS, it's collapsed to ONE warning and further churn is suppressed
// until it goes quiet for FLAP_RECOVER_MS (then a "stabilized" line clears it).
const FLAP_LIMIT = 4;            // transitions (add or remove) that trip the flap warning
const FLAP_WINDOW_MS = 8000;     // ...within this rolling window
const FLAP_RECOVER_MS = 6000;    // quiet this long → declare it stabilized
// Auto-stop so a forgotten watcher doesn't poll /api/ports forever. You watch
// while actively plugging things, then walk away — the timeout catches that.
const WATCH_AUTO_STOP_MS = 60000;
const _watch = { timer: null, autoStop: null, seen: null, hist: new Map(), flapping: new Map() };

// One-line identity for the log: "path — Board · chip", "path — chip",
// "path — Device Name" (non-ESP), or "path — ESP32? (click Identify)" when a
// probeable port has no chip/board yet.
function portLabel(p) {
    const detail = [p.board, p.chip].filter(Boolean).join(" · ")
        || p.note
        || (p.probeable ? "ESP32? (click Identify)" : "");
    return detail ? `${p.path} — ${detail}` : p.path;
}

// Record a transition for `path` at `t` and return true if it's now flapping
// (>= FLAP_LIMIT transitions within FLAP_WINDOW_MS). Prunes old entries.
function noteFlap(path, t) {
    const times = (_watch.hist.get(path) || []).filter(ts => t - ts < FLAP_WINDOW_MS);
    times.push(t);
    _watch.hist.set(path, times);
    return times.length >= FLAP_LIMIT;
}

async function pollWatch() {
    let ports;
    try {
        ports = (await (await fetch("/api/ports")).json()).ports;
    } catch (_) {
        return;   // transient fetch error — try again next tick
    }
    const t = Date.now();
    const now = new Map(ports.map(p => [p.path, p]));
    if (_watch.seen === null) { _watch.seen = now; return; }   // first tick: baseline only
    let changed = false;

    // A transition is an add OR a remove; label a flap by whichever side we know.
    const transitions = [];
    for (const [path, p] of now) if (!_watch.seen.has(path)) transitions.push([path, p, "🔌 Added  "]);
    for (const [path, p] of _watch.seen) if (!now.has(path)) transitions.push([path, p, "🔴 Removed"]);

    for (const [path, p, verb] of transitions) {
        changed = true;
        if (_watch.flapping.has(path)) {                 // already flagged — stay quiet
            _watch.flapping.set(path, t);                // refresh last-seen for recovery timer
            noteFlap(path, t);
            continue;
        }
        if (noteFlap(path, t)) {                          // just crossed the threshold
            _watch.flapping.set(path, t);
            appendLog(`⚠️  ${portLabel(p)} is flapping (re-enumerating) — check the USB cable / power; common on cheap ESP32 boards with a weak regulator. Suppressing further churn for this port.\n`);
        } else {
            appendLog(`${verb} ${portLabel(p)}\n`);
        }
    }

    // Recovery: a flapping port quiet for FLAP_RECOVER_MS is declared stable.
    for (const [path, last] of _watch.flapping) {
        if (t - last >= FLAP_RECOVER_MS) {
            _watch.flapping.delete(path);
            _watch.hist.delete(path);
            const p = now.get(path) || _watch.seen.get(path) || { path };
            appendLog(`✅ ${portLabel(p)} stabilized.\n`);
        }
    }

    _watch.seen = now;
    if (changed) renderPortOptions(ports);   // keep the dropdown in sync with reality
}

document.getElementById("watch-ports").addEventListener("click", () => {
    const btn = document.getElementById("watch-ports");
    if (_watch.timer) { stopWatch("stopped"); return; }
    _watch.seen = null;   // pollWatch's first tick sets the baseline (no false "added" burst)
    _watch.hist.clear();
    _watch.flapping.clear();
    btn.textContent = "Watching…";
    btn.classList.add("watching");
    appendLog(`\n--- Port watcher started — plug or unplug a device (auto-stops after ${WATCH_AUTO_STOP_MS / 1000}s) ---\n`);
    pollWatch();
    _watch.timer = setInterval(pollWatch, 1500);
    _watch.autoStop = setTimeout(() => stopWatch("auto-stopped after idle timeout"), WATCH_AUTO_STOP_MS);
});

// Tear down the watcher (manual toggle or the auto-stop timeout both land here).
function stopWatch(reason) {
    clearInterval(_watch.timer);
    clearTimeout(_watch.autoStop);
    _watch.timer = _watch.autoStop = _watch.seen = null;
    _watch.hist.clear();
    _watch.flapping.clear();
    const btn = document.getElementById("watch-ports");
    btn.textContent = "Watch";
    btn.classList.remove("watching");
    appendLog(`--- Port watcher ${reason} ---\n`);
}

// ---------------------------------------------------------------------------
// Live tab
// ---------------------------------------------------------------------------

// "Online only" — a view filter over the same device list, persisted with the rest of the
// state so the choice survives a reload. It hides nothing permanently: the records stay in
// moondeck.json and reappear the moment the box is unticked (or the device answers again).
const onlineOnlyBox = document.getElementById("online-only");
if (onlineOnlyBox) {
    onlineOnlyBox.addEventListener("change", () => {
        state.onlineOnly = onlineOnlyBox.checked;
        saveState();
        renderDevices();
    });
}

// Scan — the Live tab's single action. A subnet scan both finds new devices and settles
// online/offline for every device already registered, so it is a superset of the old
// separate "refresh known devices" step (which is why that button is gone: pressing both
// was the only way to get one complete picture). The /api/refresh endpoint stays for
// scripts and for a device on a subnet this machine isn't scanning.
document.getElementById("discover-btn").addEventListener("click", async () => {
    appendLog("\n--- Scanning for devices ---\n");
    switchPane("log");
    const resp = await fetch("/api/discover", { method: "POST" });
    // Server attributes found devices to the matching network (or creates a
    // new one) and returns the whole updated state — the JS just adopts it.
    // Client-side merging used to live here; moving it server-side keeps the
    // network-membership rule in one place.
    state = await resp.json();
    // Report which subnet was actually scanned + how many answered, so a
    // wrong/unset-network scan (finds nothing on a subnet with no devices)
    // reads differently from "no devices online". The scan uses the machine's
    // auto-detected /24 — see the server's _scanned_subnet.
    const scanned = state._scanned_subnet;
    const found = state._found_count ?? 0;
    if (scanned) appendLog(`Scanned ${scanned}.1-254 — ${found} device(s) answered\n`);
    if (scanned && found === 0)
        appendLog(`  (nothing on ${scanned}.x — check this machine is on the devices' subnet, no VPN)\n`);
    renderNetworkBar();
    renderDevices();
    refreshPorts();
    // A scan can re-attribute last_port (it consumes the flash breadcrumb and can strip a
    // stale link), so the "last flashed: X" hint under the port dropdown is restated too.
    updatePortDeviceHint(getActiveNetwork()?.port || "");
    const active = getActiveNetwork();
    const count = (active && active.devices) ? active.devices.length : 0;
    const onCount = active ? active.devices.filter(d => d.online).length : 0;
    appendLog(`Active network "${state.active_network}": ${onCount}/${count} online\n`);
});

// Network bar — rebuilds the dropdown options from state.networks and
// syncs the WiFi panel to the active network's credentials. Called from
// init and after any state mutation that touches networks (add, rename,
// discover, refresh).
function renderNetworkBar() {
    const select = document.getElementById("network-select");
    if (!select) return;
    select.innerHTML = "";
    const networks = state.networks || [];
    if (networks.length === 0) {
        const opt = document.createElement("option");
        opt.value = ""; opt.textContent = "(no network — click Add)";
        select.appendChild(opt);
    } else {
        for (const n of networks) {
            const opt = document.createElement("option");
            opt.value = n.name;
            opt.textContent = `${n.name} — ${n.subnet || "no subnet"}`;
            if (n.name === state.active_network) opt.selected = true;
            select.appendChild(opt);
        }
    }
    // Sync the WiFi panel inputs to the active network.
    const active = getActiveNetwork();
    const wifi = (active && active.wifi) || { ssid: "", password: "" };
    const ssidInput = document.getElementById("network-wifi-ssid");
    const pwInput = document.getElementById("network-wifi-password");
    if (ssidInput) ssidInput.value = wifi.ssid || "";
    if (pwInput) pwInput.value = wifi.password || "";
    // Hide the WiFi panel when there's no network selected (nothing to bind to).
    const panel = document.getElementById("network-wifi");
    if (panel) panel.style.display = active ? "" : "none";
}

// One-shot wire-up for the network bar's interactive controls. Idempotent —
// safe to call once at startup; subsequent renders just refresh the option
// list via renderNetworkBar().
function setupNetworkBar() {
    const select = document.getElementById("network-select");
    const rename = document.getElementById("network-rename");
    const add = document.getElementById("network-add");
    const ssidInput = document.getElementById("network-wifi-ssid");
    const pwInput = document.getElementById("network-wifi-password");

    if (select) select.addEventListener("change", async () => {
        state.active_network = select.value;
        state.active_network_user_pinned = true;  // user override sticks
        await saveState();
        renderNetworkBar();
        renderDevices();
        refreshPorts();
    });

    if (rename) rename.addEventListener("click", async () => {
        const active = getActiveNetwork();
        if (!active) { appendLog("No active network to rename.\n"); return; }
        const next = prompt("Rename network", active.name);
        if (!next || next === active.name) return;
        if ((state.networks || []).some(n => n.name === next)) {
            appendLog(`A network named "${next}" already exists.\n`);
            return;
        }
        active.name = next;
        state.active_network = next;
        await saveState();
        renderNetworkBar();
    });

    if (add) add.addEventListener("click", async () => {
        const name = prompt("Network name", "Network " + ((state.networks || []).length + 1));
        if (!name) return;
        if ((state.networks || []).some(n => n.name === name)) {
            appendLog(`A network named "${name}" already exists.\n`);
            return;
        }
        // We don't know the new network's subnet up front — the user is
        // probably about to move to it, or they're adding it speculatively.
        // Discover/refresh will populate the subnet from the actual scan.
        state.networks = state.networks || [];
        state.networks.push({
            name, subnet: "", wifi: { ssid: "", password: "" },
            port: "", devices: [],
        });
        state.active_network = name;
        state.active_network_user_pinned = true;
        await saveState();
        renderNetworkBar();
        renderDevices();
        refreshPorts();
    });

    // WiFi credentials persist on blur — common-case interaction is "paste,
    // tab out" which fires blur. Avoid input-event saves to keep the JSON
    // diff minimal (one save per field edit, not per keystroke).
    function saveWifi() {
        const active = getActiveNetwork();
        if (!active) return;
        active.wifi = active.wifi || {};
        if (ssidInput) active.wifi.ssid = ssidInput.value;
        if (pwInput) active.wifi.password = pwInput.value;
        saveState();
    }
    if (ssidInput) ssidInput.addEventListener("blur", saveWifi);
    if (pwInput) pwInput.addEventListener("blur", saveWifi);
}

function renderDevices() {
    const el = document.getElementById("device-list");
    const active = getActiveNetwork();
    const allDevices = (active && active.devices) || [];
    if (allDevices.length === 0) {
        el.textContent = "No devices discovered yet.";
        return;
    }
    // "Online only" hides what didn't answer the last probe. `online !== false` is the same
    // test the status dot uses: a device discovered but never probed has no `online` field,
    // and treating that as offline would hide a board that may well be up.
    const devices = state.onlineOnly
        ? allDevices.filter(d => d.online !== false)
        : allDevices;
    const hidden = allDevices.length - devices.length;
    el.innerHTML = "";
    if (devices.length === 0) {
        // Say why the list is empty. Without this, filtering everything out looks identical
        // to "discovery found nothing" — and the fix (untick the box) isn't discoverable.
        el.textContent = `No online devices (${hidden} offline hidden).`;
        return;
    }
    for (const device of devices) {
        // A plain <div>, NOT a <label>: a <label> wrapper toggles its checkbox on a click
        // ANYWHERE inside it (native behaviour), so clicking the IP/name to open the device
        // UI also flipped the selection. The checkbox has its own change handler, so it
        // toggles only when clicked directly.
        const label = document.createElement("div");
        label.className = "device-item";

        const dot = document.createElement("span");
        dot.className = "device-status " + (device.online !== false ? "online" : "offline");
        dot.title = device.online !== false ? "online" : "offline";

        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = !!device.selected;
        cb.addEventListener("change", () => {
            device.selected = cb.checked;
            saveState();
        });

        // Two-row layout: row 1 carries identity at-a-glance (dot, checkbox,
        // deviceName). Row 2 carries secondary info (IP + fw:<firmware>) plus
        // the device picker + remove button. The deviceName is the prominent
        // line — it's how a human recognises the device ("the S31") — with the
        // IP demoted to the info row (still clickable to open the device UI).
        // A device with no name yet falls back to showing its IP as the primary.
        // A <button> (not a clickable <span>) so it's keyboard-focusable and Enter/Space activate it
        // — the .link-button class strips the button chrome to read as inline text.
        const nameText = document.createElement("button");
        nameText.type = "button";
        nameText.className = "device-name link-button";
        nameText.textContent = device.deviceName || device.ip;
        const tooltipLines = ["Open this device's UI in the view pane", device.ip];
        if (device.last_port) tooltipLines.push(`last flashed via ${device.last_port}`);
        nameText.title = tooltipLines.join("\n");
        nameText.addEventListener("click", () => showInView("http://" + device.ip));

        // Info row: clickable IP · fw. The IP is its own button so activating it opens the
        // device UI in the view pane (same as the name), while the fw text stays inert.
        const infoText = document.createElement("span");
        infoText.className = "device-info";
        const ipLink = document.createElement("button");
        ipLink.type = "button";
        ipLink.className = "device-ip-link link-button";
        ipLink.textContent = device.ip;
        ipLink.title = "Open this device's UI in the view pane";
        ipLink.addEventListener("click", () => showInView("http://" + device.ip));
        infoText.appendChild(ipLink);

        // ↗ — the same device UI in a real browser tab, next to (not instead of) the view
        // pane. The pane keeps MoonDeck's context; a tab gives the device its own window,
        // devtools, and a bookmarkable URL. An <a target="_blank"> rather than a button
        // calling window.open, so the browser's own affordances work: middle-click,
        // cmd-click, "Open in new window", copy-link-address. rel="noopener" because a
        // target="_blank" link otherwise hands the opened page a window.opener handle
        // back into MoonDeck.
        const ipNewTab = document.createElement("a");
        ipNewTab.className = "device-ip-newtab";
        ipNewTab.href = "http://" + device.ip;
        ipNewTab.target = "_blank";
        ipNewTab.rel = "noopener noreferrer";
        ipNewTab.textContent = "↗";
        ipNewTab.title = "Open this device's UI in a new browser tab";
        infoText.appendChild(ipNewTab);
        if (device.firmware) {
            infoText.appendChild(document.createTextNode(` · fw:${device.firmware}`));
        }

        // last_port — the serial port this exact device (by MAC) was last flashed via. Click it to
        // set it as the active network's flash port, so "flash the SE16" is one click → run Flash.
        // The port lives per-device (keyed by MAC), the flash uses the network-level port; this
        // chip bridges them. Absent until the device has been flashed once through MoonDeck.
        if (device.last_port) {
            const portChip = document.createElement("button");
            portChip.className = "device-port-chip";
            portChip.textContent = "⚡ " + device.last_port.replace("/dev/cu.", "").replace("/dev/tty.", "");
            portChip.title = `Select this device model for flashing: set the port` +
                (device.firmware ? `, firmware (${device.firmware})` : "") +
                (device.deviceModel ? ` and deviceModel` : "");
            const active = getActiveNetwork();
            if (active && active.port === device.last_port) portChip.classList.add("active");
            // Selecting a device model for flashing means matching what it actually runs: the port, AND the
            // firmware + deviceModel MoonDeck learned from discovery — so Build/Flash target the right
            // binary for this device model (flashing the wrong firmware would brick it). One click, all in sync.
            portChip.addEventListener("click", async (e) => {
                e.preventDefault();
                const a = getActiveNetwork();
                if (a) a.port = device.last_port;
                // Set the flash target to THIS device's firmware/deviceModel — and clear it when this
                // device doesn't have one, so the next Build/Flash can't reuse the PREVIOUS device's
                // values (flashing the wrong firmware would brick it). The reset is unconditional.
                state.firmware = (device.firmware && firmwares.includes(device.firmware)) ? device.firmware : "";
                state.provisionDeviceModel = device.deviceModel || "";
                await saveState();
                refreshPorts();
                renderFirmwareSelect();
                renderDeviceModelSelect();
                renderDevices();
            });
            infoText.appendChild(document.createTextNode(" "));
            infoText.appendChild(portChip);
        }

        // Device-model picker — options derived from deviceModels.json (loaded via
        // /api/device-models). Auto-deduced for firmwares that map to a single
        // deviceModel (probe sets device.deviceModel); user-set when the firmware can
        // run on multiple device models (e.g. `esp32` on LOLIN D32 vs generic
        // DevKit). A device-reported value that isn't in the catalog gets
        // prepended as <key> (unknown) so the selection survives — without
        // that, <select> would silently snap to "" and the next saveState
        // would clobber the device's stored value with empty.
        const deviceModelPicker = document.createElement("select");
        deviceModelPicker.className = "devicemodel-picker";
        deviceModelPicker.title = "Device model (pick when firmware can't tell us)";
        // Only offer deviceModels that can run this device's firmware — a model whose `firmwares`
        // don't include it is not a valid choice for this unit (same filter the provisioning picker
        // uses). If the firmware is unknown (empty), we can't filter, so show all.
        const fwModels = device.firmware
            ? deviceModels.filter(b => (b.firmwares || []).includes(device.firmware))
            : deviceModels;
        const deviceModelOptions = [
            ["", "(unknown model)"],
            // Each entry is [value, label] for the <select>; with the
            // single-name catalog (no separate key/label), they're identical.
            ...fwModels.map(b => [b.name, b.name]),
        ];
        const deviceModel = device.deviceModel || "";
        if (deviceModel && !deviceModelOptions.some(([k]) => k === deviceModel)) {
            // Keep the current value selectable even if it's filtered out (firmware mismatch) or
            // not in the catalog — marked (unknown) so the selection survives.
            deviceModelOptions.push([deviceModel, `${deviceModel} (unknown)`]);
        }
        for (const [val, lbl] of deviceModelOptions) {
            const opt = document.createElement("option");
            opt.value = val;
            opt.textContent = lbl;
            if (deviceModel === val) opt.selected = true;
            deviceModelPicker.appendChild(opt);
        }
        // Push a deviceModel's full deviceModels.json defaults to the device (POST
        // /api/push-device → _push_device fans out controls.<Module>.<control>).
        // `onDone(ok)` lets the explicit button below show success/failure; the picker
        // change path passes nothing (fire-and-forget, recovered on next refresh).
        const pushDevice = (deviceModel, onDone) => {
            // Success is the device-side result in the JSON body ({"ok": bool} from
            // _push_device) — HTTP 200 alone can wrap a failed push (a device
            // timeout / non-2xx mid-fan-out), so r.ok would falsely report success.
            // 10s AbortSignal timeout so a stalled request can't wedge the button forever.
            fetch("/api/push-device", {
                method: "POST",
                headers: {"Content-Type": "application/json"},
                body: JSON.stringify({ip: device.ip, deviceModel}),
                signal: timeoutSignal(10000),
            }).then(r => r.json()).then(j => onDone && onDone(!!j.ok))
              .catch(() => onDone && onDone(false));
        };
        deviceModelPicker.addEventListener("change", () => {
            device.deviceModel = deviceModelPicker.value;
            saveState();
            // Mirror the change to the device immediately. Without this, the
            // device's SystemModule wouldn't hear about the picker until the
            // next discover/refresh probe — and the user expects the device
            // UI to update right after they pick. Fire-and-forget; failure
            // (timeout / device offline) is recovered on the next refresh
            // when discover/refresh's bulk push catches up.
            pushDevice(deviceModelPicker.value);
        });

        // Explicit "inject" — re-push the SELECTED device model's full config on demand,
        // without having to change the picker. Distinct intent from the implicit on-change
        // push: re-apply after a reflash wiped config, or re-assert defaults a user edited
        // away. Brief inline feedback so a no-op (timeout / offline) isn't silent.
        const injectBtn = document.createElement("button");
        injectBtn.className = "device-inject";
        injectBtn.textContent = "inject";
        injectBtn.title = "Push the selected device-model's deviceModels.json defaults to this device now";
        injectBtn.addEventListener("click", (e) => {
            e.preventDefault();
            const deviceModel = deviceModelPicker.value;
            if (!deviceModel) { injectBtn.textContent = "pick a device model"; setTimeout(() => injectBtn.textContent = "inject", 1500); return; }
            injectBtn.disabled = true;
            injectBtn.textContent = "injecting…";
            pushDevice(deviceModel, (ok) => {
                injectBtn.textContent = ok ? "injected ✓" : "failed ✗";
                setTimeout(() => { injectBtn.textContent = "inject"; injectBtn.disabled = false; }, 1800);
            });
        });

        // "OTA" — wireless flash of the local build for this device's firmware. MoonDeck serves
        // build/esp32-<fw>/projectMM.bin and hands the device its URL (POST /api/ota); the device
        // pulls + flashes over WiFi. No USB. Needs a local build for the device's firmware.
        const otaBtn = document.createElement("button");
        otaBtn.className = "device-inject";       // same compact style as inject
        const otaFw = device.firmware && device.firmware !== "unknown" ? device.firmware : "";
        otaBtn.textContent = "OTA";
        otaBtn.disabled = !otaFw;
        otaBtn.title = otaFw
            ? `Flash the local ${otaFw} build to this device over WiFi (no USB)`
            : "OTA needs the device's firmware known (discover it first)";
        otaBtn.addEventListener("click", async (e) => {
            e.preventDefault();
            if (!otaFw) return;
            otaBtn.disabled = true;
            otaBtn.textContent = "OTA…";
            try {
                const res = await fetch("/api/ota", {
                    method: "POST", headers: {"Content-Type": "application/json"},
                    body: JSON.stringify({ip: device.ip, firmware: otaFw}),
                    signal: timeoutSignal(15000),
                });
                const j = await res.json();
                otaBtn.textContent = j.ok ? "flashing ✓" : "failed ✗";
            } catch (err) {
                otaBtn.textContent = "failed ✗";
            }
            setTimeout(() => { otaBtn.textContent = "OTA"; otaBtn.disabled = !otaFw; }, 2500);
        });

        const removeBtn = document.createElement("button");
        removeBtn.className = "device-remove";
        removeBtn.textContent = "x";
        removeBtn.title = "Remove device";
        removeBtn.addEventListener("click", (e) => {
            e.preventDefault();
            // Remove from the active network's device list; other networks
            // are untouched even if (theoretically) the same IP shows up.
            const a = getActiveNetwork();
            if (a) a.devices = a.devices.filter(d => d.ip !== device.ip);
            saveState();
            renderDevices();
        });

        // Three-row layout, each row a flex-basis:100% wrapper inside
        // .device-item (which is flex-wrap:wrap):
        //   row 1 — dot · checkbox · deviceName · X  (identity + remove)
        //   row 2 — IP · fw:firmware                  (secondary info)
        //   row 3 — [device picker]                   (right-aligned)
        // Splitting like this keeps each row narrow enough to fit the
        // sidebar without truncation, and puts the most-clicked items
        // (checkbox, name, device picker) at predictable y-positions.
        const row1 = document.createElement("div");
        row1.className = "device-row device-row-identity";
        row1.appendChild(dot);
        row1.appendChild(cb);
        row1.appendChild(nameText);
        row1.appendChild(removeBtn);

        const row2 = document.createElement("div");
        row2.className = "device-row device-row-info";
        row2.appendChild(infoText);

        const row3 = document.createElement("div");
        row3.className = "device-row devicemodel-row";
        row3.appendChild(deviceModelPicker);
        row3.appendChild(injectBtn);
        row3.appendChild(otaBtn);

        label.appendChild(row1);
        label.appendChild(row2);
        label.appendChild(row3);
        el.appendChild(label);
    }

    // Account for what the filter removed, so a short list is never mistaken for the
    // whole registry — the count is the reminder that more devices exist.
    if (hidden > 0) {
        const note = document.createElement("div");
        note.className = "device-filter-note";
        note.textContent = `${hidden} offline device${hidden === 1 ? "" : "s"} hidden`;
        el.appendChild(note);
    }
}

// ---------------------------------------------------------------------------
// State persistence
// ---------------------------------------------------------------------------

async function saveState() {
    await fetch("/api/state", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(state),
    });
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

// http(s):// and file:// links anywhere in script output get linkified.
// Everything else stays as plain textContent — script stdout is treated
// as untrusted, so we never set innerHTML on it; instead we split on URLs
// and append text nodes + <a> nodes, both of which encode HTML-special
// chars automatically.
const URL_RE = /\b((?:https?|file):\/\/[^\s<>"]+)/g;

// `MOONDECK_VIEW: <url>` (relative or absolute) — a marker scripts can
// emit to route the URL straight into the View pane after a one-tick
// delay, AND render a clickable "Open in View pane → <url>" link in the
// log so the user knows what just happened. Same-origin routing without
// relying on the user clicking through.
const VIEW_MARKER_RE = /^MOONDECK_VIEW:\s*(\S+)\s*$/;

function appendLog(text) {
    // Marker shortcut: if this chunk is a single MOONDECK_VIEW line, swap
    // it for an explanatory clickable link and auto-open in the View pane.
    // Strip trailing newlines before matching so the marker survives the
    // "+ \n" the SSE stream tacks on per line.
    const stripped = text.replace(/\n+$/, "");
    const markerMatch = VIEW_MARKER_RE.exec(stripped);
    if (markerMatch) {
        let safeUrl = null;
        try {
            // Use document.baseURI as base so relative paths like /api/history-report resolve.
            const parsed = new URL(markerMatch[1], document.baseURI);
            if (parsed.protocol === "http:" || parsed.protocol === "https:") {
                safeUrl = parsed.href;
            }
        } catch (_) { /* invalid URL — skip */ }
        if (safeUrl) {
            const a = document.createElement("a");
            a.href = safeUrl;
            a.textContent = "Open in View pane → " + safeUrl;
            a.addEventListener("click", (ev) => { ev.preventDefault(); showInView(safeUrl); });
            logEl.appendChild(a);
            logEl.appendChild(document.createTextNode("\n"));
            logEl.scrollTop = logEl.scrollHeight;
            // Defer the actual View-pane switch so the log row renders first
            // (otherwise the user can't see what was just produced when they
            // tab back to Log).
            setTimeout(() => showInView(safeUrl), 50);
        } else {
            logEl.appendChild(document.createTextNode(stripped + "\n"));
            logEl.scrollTop = logEl.scrollHeight;
        }
        return;
    }
    // Fast path: no URL in this chunk, plain append.
    if (!URL_RE.test(text)) {
        logEl.appendChild(document.createTextNode(text));
        logEl.scrollTop = logEl.scrollHeight;
        return;
    }
    URL_RE.lastIndex = 0;
    let lastIdx = 0;
    let m;
    while ((m = URL_RE.exec(text)) !== null) {
        if (m.index > lastIdx) {
            logEl.appendChild(document.createTextNode(text.slice(lastIdx, m.index)));
        }
        const url = m[1];
        const a = document.createElement("a");
        a.href = url;
        // Same-origin URLs (file:// AND http://localhost-the-MoonDeck-server)
        // open inside the View pane — that's a "show me the rendered content
        // MoonDeck just produced" gesture, not "leave MoonDeck for an external
        // site." Cross-origin http(s):// opens in a new tab, the normal
        // external-link behaviour.
        const isMoonDeckUrl =
            url.startsWith("file://") ||
            url.startsWith(location.origin + "/") ||
            url.startsWith(location.origin) && url.length === location.origin.length;
        if (isMoonDeckUrl) {
            a.addEventListener("click", (ev) => {
                ev.preventDefault();
                showInView(url);
            });
        } else {
            a.target = "_blank";
            a.rel = "noopener";
        }
        a.textContent = url;
        logEl.appendChild(a);
        lastIdx = m.index + url.length;
    }
    if (lastIdx < text.length) {
        logEl.appendChild(document.createTextNode(text.slice(lastIdx)));
    }
    logEl.scrollTop = logEl.scrollHeight;
}

document.getElementById("clear-log").addEventListener("click", () => {
    logEl.textContent = "";
});

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

init();
