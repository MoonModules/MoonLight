// install-picker-devices.js: the device-catalog + chip-detection half of the
// release picker. WEB-INSTALLER ONLY. It is imported by the GitHub Pages
// installer (mooninstaller/index.html) and passed into installPicker.init() as
// the `deviceSupport` option; the shared install-picker.js never imports it.
//
// Why a separate file: install-picker.js is embedded into the firmware binary
// (src/ui/embed_ui.cmake gzips it verbatim: there is no bundler or tree-shaking
// in this project, so whatever is in that file ships on the device). The device
// catalog and chip detection are only meaningful during a first USB flash from
// the browser; on-device OTA already knows its deviceModel (SystemModule). Keeping this
// code out of install-picker.js keeps it out of every device's flash. The
// injection seam (init({ deviceSupport })) is the standard "host supplies the
// optional capability" pattern: the Pages page wires it in, the device passes
// nothing, so the device code is genuinely absent from the firmware.
//
// Pure DOM + a same-origin fetch: no serial / esptool / Improv (those live in
// install-orchestrator.js and reach this code only via the onDetect callback
// install-picker.js already owns).

// Devices catalog: same-origin mooninstaller/deviceModels.json. ~1 KB, no rate-limit
// concern (CDN serves it on the public site, preview_installer serves it from
// disk locally), so no sessionStorage cache: caching adds invalidation bugs
// without saving bytes. Graceful degradation: any fetch / parse failure returns
// [] and the picker silently omits the device <select>.
export async function loadDevices() {
    try {
        const res = await fetch("./deviceModels.json");
        if (!res.ok) return [];
        const data = await res.json();
        return Array.isArray(data) ? data : [];
    } catch (_) {
        return [];
    }
}

// The modules an entry's `state` document describes, depth first, as {id, type, controls}.
// A root member is a top-level module (its type is not written, so `type` is null); an object member with a `type` is a child module; scalars and arrays are controls.
export function stateModules(entry) {
    const out = [];
    const isObj = (v) => v && typeof v === "object" && !Array.isArray(v);
    const walk = (id, node, type) => {
        const controls = {};
        const kids = [];
        for (const [k, v] of Object.entries(node)) {
            if (k === "type" || k === "$patch") continue;
            if (isObj(v) && typeof v.type === "string") kids.push([k, v]);
            else controls[k] = v;
        }
        out.push({ id, type, controls });
        for (const [k, v] of kids) walk(k, v, v.type);
    };
    const state = entry && entry.state;
    if (isObj(state)) for (const [root, node] of Object.entries(state)) if (isObj(node)) walk(root, node, null);
    return out;
}

// Rebuild a device <select>: a leading pass-through option (label varies by
// context) followed by one option per device. Used by the picker's render()
// (full catalog) and applyDetectedChip() (chip-narrowed list) so the option-
// building shape lives in one place.
export function fillDeviceOptions(deviceEl, devices, passthroughLabel) {
    deviceEl.replaceChildren();
    const any = document.createElement("option");
    any.value = "";
    any.textContent = passthroughLabel;
    deviceEl.appendChild(any);
    for (const b of devices) {
        const opt = document.createElement("option");
        opt.value = b.name;
        opt.textContent = b.name;
        deviceEl.appendChild(opt);
    }
}

// After a successful Detect, narrow the device <select> to ONLY the devices whose
// `chip` matches the detected family: the other family is removed from the list
// entirely (plug in an S3, the classic-ESP32 devices disappear, and vice versa).
// The pass-through is relabeled "Other / generic <chip>" so a user with a device
// not in the catalog can still flash the right firmware for their silicon. The
// returned status string is shown next to the Detect button.
// Detection gives chip FAMILY only: it can't tell esp32 / esp32-eth /
// esp32-eth-wifi apart (same silicon, different wiring), so when several devices
// share the family we narrow + let the user pick rather than guessing.
export function applyDetectedChip(state, deviceEl) {
    const matches = state.devices.filter(b => b.chip === state.detectedChip);
    if (matches.length === 0) {
        // A chip we ship no device for: don't strand the user: leave the full
        // list and report it. (selectedDevice unchanged.) The chip itself is
        // shown in the port dropdown ("Port selected: ESP32-P4"), so the status
        // line carries only the action, not a redundant chip echo.
        return `No matching device for this chip: pick manually`;
    }
    fillDeviceOptions(deviceEl, matches, `Other / generic ${state.detectedChip}`);
    let autoName = "";   // a device we auto-selected (single match, or a generic default)
    if (matches.length === 1) {
        state.selectedDevice = matches[0].name;   // exactly one device → auto-pick
        autoName = matches[0].name;
    } else if (!matches.find(b => b.name === state.selectedDevice)) {
        // Several devices in this family and no current pick in it (fresh detect, or a
        // prior pick from the other family). Prefer the catalog's generic device for
        // this chip if one exists (e.g. "Generic ESP32 Dev"): a sensible no-overrides
        // default; otherwise leave the "Other / generic <chip>" pass-through selected
        // (S3/P4 ship no generic entry) so we don't guess a specific device.
        const generic = matches.find(b => /generic/i.test(b.name));
        state.selectedDevice = generic ? generic.name : "";
        if (generic) autoName = generic.name;
    } else {
        autoName = state.selectedDevice;   // an in-family pick survives the detect
    }
    deviceEl.value = state.selectedDevice || "";
    // The chip is shown in the port dropdown; the status line adds only the
    // outcome: which device was auto-selected, or a prompt to choose among the
    // matches the chip narrowed to.
    return autoName
        ? `Selected ${autoName}`
        : `Pick your device (${matches.length} ${matches.length === 1 ? "match" : "matches"})`;
}
