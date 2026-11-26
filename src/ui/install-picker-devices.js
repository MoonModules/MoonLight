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
        return Array.isArray(data) ? flashable(data) : [];
    } catch (_) {
        return [];
    }
}

// The catalog's entries the installer can flash: the catalog also lists the computers MoonLight runs on, whose firmwares are the registry's
// `host-` builds, downloaded rather than written to a board. Every consumer of the catalog in the installer filters through this one rule.
export const flashable = (devices) => devices.filter(d => (d.firmwares || []).some(fw => !String(fw).startsWith("host-")));

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
// Detection gives chip FAMILY only: it can't tell esp32 / esp32-lite /
// esp32-ethonly apart (same silicon, different wiring), so when several devices
// share the family we narrow + let the user pick rather than guessing.
// What a board tells about itself, in the one shape measuredFirmware reads: { revision (major x 100 + minor, or null), flashMB
// (0 when unread), psramMB (0 for none), psramMode ("quad", "octal", "hex" or "") }. Two sources make it.

// From esptool-js: the chip description with its revision ("ESP32-D0WD-V3 (revision v3.1)"), the features, where an in-package
// PSRAM reads "Embedded PSRAM 8MB (AP_3v3)" (an S3's 2 MB part is quad, its 8 and 16 MB parts octal), and the flash size
// ("16MB", undefined when unread). PSRAM outside the package is invisible here.
export function factsFromEsptool({ description, features, flashSize }) {
    const psramMB = embeddedPsramMB(features);
    return { revision: revisionOf(description), flashMB: parseInt(flashSize, 10) || 0, psramMB, psramMode: psramModeOfSize(psramMB) };
}

// From a running device's `hardware` (writeDeviceFacts): the revision ("v3.1"), its flash, and the PSRAM it found at boot, outside
// the package too, with the interface its build drives.
export function factsFromDevice(hw) {
    const h = hw || {};
    return { revision: revisionOf(h.revision), flashMB: Number(h.flashMB) || 0, psramMB: Number(h.psramMB) || 0, psramMode: h.psramMode || "" };
}

// A revision as major x 100 + minor, from "revision v3.1", "v3.1" or "revision 3"; null when the text names none.
function revisionOf(text) {
    const rev = /v?(\d+)(?:\.(\d+))?\s*\)?\s*$/.exec(/revision\s+v?[\d.]+/i.exec(text || "")?.[0] || text || "");
    return rev ? Number(rev[1]) * 100 + Number(rev[2] || 0) : null;
}

// The in-package PSRAM esptool's features name ("Embedded PSRAM 8MB (AP_3v3)"), in MB, 1 where the size is unstated, 0 for none.
function embeddedPsramMB(features) {
    const m = (features || []).map(String).map(f => /Embedded PSRAM(?:\s*(\d+)\s*MB)?/i.exec(f)).find(Boolean);
    return m ? Number(m[1] || 0) || 1 : 0;
}

// An S3's in-package PSRAM interface by its size: the 2 MB part is quad, the 8 and 16 MB parts octal.
function psramModeOfSize(mb) {
    if (!mb) return "";
    return mb >= 8 ? "octal" : "quad";
}

// The firmware each chip family's facts name, one rule per family (docs/reference/hardware/firmware-variants.md).
const FIRMWARE_BY_FAMILY = {
    "ESP32": ({ revision, flashMB, psramMB }) => {
        if (revision !== null && revision < 300 && psramMB) return "esp32-rev1-lite";
        if (!flashMB) return null;   // the 4 MB tier or not: the device's own list stands
        return flashMB > 4 ? "esp32" : "esp32-lite";
    },
    "ESP32-S3": ({ flashMB, psramMode }) => {
        if (!flashMB) return null;
        // Espressif's 32 MB S3 modules are octal flash (WROOM-2 N32R8V, N32R16V); a 16 MB octal part runs esp32s3-opi through ESP-IDF's flash-mode auto-detect.
        if (flashMB >= 32) return "esp32s3-opi-oflash";
        if (psramMode === "octal") return "esp32s3-opi";
        if (psramMode === "quad") return flashMB > 4 ? "esp32s3-qspi" : "esp32s3-qspi-lite";
        return null;   // PSRAM outside the package: an interface this cannot read
    },
    "ESP32-C3": () => "esp32c3",
    "ESP32-S31": () => "esp32s31",
    "ESP32-P4": ({ revision }) => (revision !== null && revision >= 300 ? "esp32p4rev3" : "esp32p4rev1"),
};

// What the facts say, for the note beside the firmware: "revision 3.1, 16 MB flash, octal PSRAM".
function factsNote({ revision, flashMB, psramMB, psramMode }) {
    return [revision !== null ? `revision ${Math.floor(revision / 100)}.${revision % 100}` : "",
            flashMB ? `${flashMB} MB flash` : "flash size unread",
            psramMB ? `${psramMode ? psramMode + " " : ""}PSRAM` : ""].filter(Boolean).join(", ");
}

// The firmware a board names by its facts, for an install with no specific device chosen and for a running device's suggested
// move. Returns { firmware, note }, firmware null when the facts name none.
export function measuredFirmware(family, facts) {
    const said = factsNote(facts);
    const rule = FIRMWARE_BY_FAMILY[family];
    const firmware = rule ? rule(facts) : null;
    if (firmware) return { firmware, note: `${said}: ${firmware}` };
    const why = !rule ? `no firmware rule for ${family}`
              : !facts.flashMB ? "pick the device, which names its firmware"
              : "PSRAM outside the package has an interface this cannot read, so pick the device";
    return { firmware: null, note: `${said}: ${why}` };
}

// Whether `firmware`, a chosen device's, runs on the board whose facts named `measured` (measuredFirmware): the firmware the facts
// name, or one that runs on every board of its chip: the classic's 4 MB tier, Ethernet-only and cache-workaround builds.
// Anything else differs from the board in a fact the note names.
export function firmwareFits(firmware, measured) {
    if (!measured || !measured.firmware || firmware === measured.firmware) return true;
    return /^esp32-/.test(measured.firmware) || measured.firmware === "esp32"
        ? ["esp32-lite", "esp32-ethonly", "esp32-rev1-lite"].includes(firmware)
        : false;
}

// The devices a detected board can be: its chip's, narrowed by what it measured to those with a firmware that runs on it (firmwareFits);
// the chip's whole list when the facts name no firmware or no device fits them.
export function devicesForBoard(devices, chip, facts) {
    const ofChip = devices.filter(b => b.chip === chip);
    const measured = facts ? measuredFirmware(chip, facts) : null;
    if (!measured || !measured.firmware) return ofChip;
    const fitting = ofChip.filter(b => (b.firmwares || []).some(fw => firmwareFits(fw, measured)));
    return fitting.length ? fitting : ofChip;
}

export function applyDetectedChip(state, deviceEl) {
    const matches = devicesForBoard(state.devices, state.detectedChip, state.detectedFacts);
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
