// The migration engine and its data: THE authoritative log of every machine-mappable schema
// break, applied in the BROWSER to a config backup before upload, the device never executes
// migration logic (ADR-0013: its loader ignores unknown keys, defaults absent ones, clamps
// stale values). MIGRATING.md keeps only the breaks no map can express (erase-flash moves,
// behavior changes) and points here for the rest.
//
// Rules of this file:
//  - CUMULATIVE: entries only append, dated newest-first inside each map. Each firmware release
//    embeds the map as of that release, so every release's Restore migrates any older backup
//    exactly TO that release, no version stamps needed.
//  - RETIRED NAMES ARE RESERVED: the maps apply unconditionally, so a retired file, type,
//    control, or option name must never be reintroduced with a new meaning.
//  - Three honesty levels, matching what an entry actually preserves: plain renames map
//    silently-but-reported; deterministic value moves `set` the new value; anything
//    chip-dependent or semantics-changing gets a "review" report entry instead of a guess.
//  - A future non-rename break slots in as a transform stage beside applyMigrations, add the
//    mechanism when the first such break lands, not before.

// Old .config filename → new (the file IS the module's typeName).
export const FILE_RENAMES = {
    "Layers.json": { to: "Effects.json", date: "2026-08-08" },   // Layers container → Effects (L.E.D. naming)
    "LayoutGroup.json": { to: "Layouts.json", date: "2026-05" }, // early container type renames
    "DriverGroup.json": { to: "Drivers.json", date: "2026-05" },
};

// Old module type value (any "type" / "N.type" key) → new type. `set` writes a control the
// merge made explicit when its value is deterministic; `review` flags what a map cannot decide.
export const TYPE_RENAMES = {
    "Layers": { type: "Effects", date: "2026-08-08" },
    // Noise2D folded into Noise, which is Dim::D3 and renders the same field on a panel. `scale`
    // carries; Noise2D's `speed` (a 0..15 divisor) has no equivalent, because Noise takes its rate
    // from `bpm` on the shared beat clock rather than a per-effect divisor.
    "Noise2DEffect": { type: "NoiseEffect", date: "2026-09-05",
                       review: "set bpm: Noise2D's speed (0..15) has no equivalent on the beat clock" },
    // Infrared was rebuilt around learned-code ROWS, so the single-target controls it used to
    // carry (`code on/off`, `code brightness up`, ...) have no equivalent: the codes themselves
    // are gone and the remote has to be re-learned. The module carries over, which is what stops
    // it vanishing from the tree on boot.
    "IrService": { type: "InfraredService", date: "2026-09-02",
                   review: "re-learn the remote: the old per-action code controls became rows" },
    // The three parallel drivers merge into ParallelLedDriver + a `peripheral` Select (values
    // per the peripheral-option rename in CONTROL_VALUE_RENAMES). Parlio and the MoonI80
    // backend map deterministically; the esp_lcd backend's name depends on the chip, review,
    // not a guess.
    "MultiPinLedDriver": { type: "ParallelLedDriver", date: "2026-07-23",
                           review: "peripheral must be re-picked (chip-dependent: I2S-IDF on classic, LCD-IDF on S3/P4)" },
    "MoonLedDriver": { type: "ParallelLedDriver", date: "2026-07-23", set: { "peripheral": "LCD-MM" },
                       review: "peripheral set to LCD-MM (S3/P4 only), re-pick on another chip" },
    "ParlioLedDriver": { type: "ParallelLedDriver", date: "2026-07-23", set: { "peripheral": "Parlio" } },
    // Driver types renamed for a human-readable UI; both later merged (above), so the map
    // jumps straight to the end state.
    "I80LedDriver": { type: "ParallelLedDriver", date: "2026-07-16",
                      review: "peripheral must be re-picked (chip-dependent: I2S-IDF on classic, LCD-IDF on S3/P4)" },
    "MoonI80LedDriver": { type: "ParallelLedDriver", date: "2026-07-16", set: { "peripheral": "LCD-MM" },
                          review: "peripheral set to LCD-MM (S3/P4 only), re-pick on another chip" },
};

// Old control name → new, SCOPED by module type: `onTypes` lists the (post-rename) types the
// rename applies to, exact names, or "*Suffix" matching any type ending in Suffix. Scoping is
// mandatory: a bare-name rename is a landmine for every other module using that name (found on
// the bench: a blanket fps → targetFps corrupted NetworkSendDriver's own `fps`). `review` marks
// a value-semantics change: the name maps, the value needs the user's eye.
export const CONTROL_RENAMES = {
    // One name for one thing: the service is AudioService, the frame is AudioFrame, so the control
    // that makes a sprite effect follow the music is audioReactive. Scoped to the seven effects that
    // declare it, per the rule above.
    "soundReactive": {
        name: "audioReactive", date: "2026-09-05",
        onTypes: ["FishTankEffect", "FlyingToastersEffect", "PacmanEffect", "PongEffect",
                  "SpaceInvadersEffect", "SpriteFountainEffect", "MovingHeadEffect"],
    },
    // ControlModule's encoders spell the word out: the interface uses the industry term, the UI
    // abbreviates it to `enc` for the strip. Scoped, because `enc1` is a plausible name anywhere.
    "enc1": { name: "encoder1", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc2": { name: "encoder2", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc3": { name: "encoder3", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc4": { name: "encoder4", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc5": { name: "encoder5", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc6": { name: "encoder6", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc7": { name: "encoder7", date: "2026-08-30", onTypes: ["ControlModule"] },
    "enc8": { name: "encoder8", date: "2026-08-30", onTypes: ["ControlModule"] },
    // PreviewDriver's fps → targetFps (now trades resolution for rate). Other drivers keep `fps`.
    "fps": { name: "targetFps", date: "2026-08-25", onTypes: ["PreviewDriver"] },
    // The correction Select every LED driver inherits from DriverBase.
    "preset": { name: "lightPreset", date: "2026-07-23", onTypes: ["*Driver"] },
    // AudioService's sync collapses into mode + a separate send-audio switch.
    "sync": { name: "mode", date: "2026-07-22", onTypes: ["AudioService"],
              review: "was off/send/receive; now mode (local/receive/simulate) plus a separate 'send audio' switch: review both" },
    // forceRing's 3-option select becomes the useRing switch.
    "forceRing": { name: "useRing", date: "2026-07-17", onTypes: ["ParallelLedDriver"],
                   review: "was a 3-option select (auto/ring/wholeFrame), now a switch: review the value" },
    // Plain-language control names.
    "shiftRegister": { name: "pinExpander", date: "2026-07-16", onTypes: ["ParallelLedDriver"] },
    "asyncTransmit": { name: "doubleBuffer", date: "2026-07-16", onTypes: ["ParallelLedDriver"] },
};

// Old control VALUE → new, keyed by the control's (post-rename) name. Same honesty levels:
// `value` when the move is deterministic, `review` when only the user (or the chip) can decide.
export const CONTROL_VALUE_RENAMES = {
    // Audio modes reordered simple-to-advanced, so the default is what works with nothing
    // attached. The indices are persisted, so every saved value moves: 0 was local audio and
    // is now simulate, 2 was simulate and is now local audio. A device without a network has
    // only two of them, where local audio is 1, which this map cannot tell apart from a
    // networked device's receive: hence the review on that one.
    "mode": { onTypes: ["AudioService"], values: {
        0: { value: 2, date: "2026-09-23" },
        2: { value: 0, date: "2026-09-23" },
        1: { date: "2026-09-23",
             review: "audio mode 1 was 'receive network' on a networked device and 'simulate' on one without: re-pick it" },
    } },
    // Peripheral options renamed to name the silicon block, not the bus protocol.
    "peripheral": { onTypes: ["ParallelLedDriver"], values: {
        "MoonI80": { value: "LCD-MM", date: "2026-07-30" },
        "i80": { date: "2026-07-30",
                 review: "peripheral 'i80' must be re-picked (chip-dependent: I2S-IDF on classic, LCD-IDF on S3/P4)" },
    } },
};

// Value renames inside preset files (/.config/presets/*): the captured container and its role.
// Applied as a quoted-token replace over the whole file, so a USER value that exactly equals a
// token (a captured Text control holding "Layers") is rewritten too; accepted, the report names
// every replacement so a false positive is visible, and parsing preset JSON here would duplicate
// the device's reader for two tokens.
export const PRESET_VALUE_RENAMES = {
    // Alongside the Layers → Effects container rename.
    "Layers": { to: "Effects", date: "2026-08-08" },
    "layer": { to: "effects", date: "2026-08-08" },
};

// Does `type` fall under an entry's onTypes scope? Exact name, or "*Suffix" wildcard.
function typeInScope(onTypes, type) {
    if (type === undefined) return false;   // orphan chain: leave the key untouched
    return onTypes.some(t => t.startsWith("*") ? type.endsWith(t.slice(1)) : type === t);
}

// Rewrite one parsed .config object in place-ish (returns a new object), collecting report
// entries as {kind: "renamed"|"review", where, detail}.
function renameKeys(obj, file, report) {
    const out = {};
    // Pre-pass: each prefix chain's module type, POST type-rename (key order in the file is
    // arbitrary, so this cannot ride the main loop). The top-level type is the filename stem.
    const stem = file.slice(file.lastIndexOf("/") + 1, -".json".length);
    const nodeTypes = { "": TYPE_RENAMES[stem] ? TYPE_RENAMES[stem].type : stem };
    for (const [key, value] of Object.entries(obj)) {
        if (key.endsWith(".type") && typeof value === "string") {
            nodeTypes[key.slice(0, -"type".length)] = TYPE_RENAMES[value] ? TYPE_RENAMES[value].type : value;
        }
    }
    for (const [key, value] of Object.entries(obj)) {
        // A key is a chain of child prefixes ("0.1.") plus the control name; "type" keys carry
        // module types as VALUES.
        const dot = key.lastIndexOf(".");
        const prefix = dot >= 0 ? key.slice(0, dot + 1) : "";
        const name = dot >= 0 ? key.slice(dot + 1) : key;
        // A rewritten key landing on one that already exists (a hand-edited bundle carrying
        // both old and new names) is last-write-wins, but never silently: report it.
        const collide = (k) => {
            if (k in out) report.push({ kind: "review", where: `${file} ${k}`,
                detail: "two entries map to this key; the later one won" });
        };
        if (name === "type" && typeof value === "string" && TYPE_RENAMES[value]) {
            const r = TYPE_RENAMES[value];
            out[key] = r.type;
            report.push({ kind: "renamed", where: `${file} ${key}`, detail: `${value} → ${r.type} (${r.date})` });
            if (r.review) report.push({ kind: "review", where: `${file} ${prefix}`, detail: r.review });
            if (r.set) for (const [k, v] of Object.entries(r.set)) { collide(prefix + k); out[prefix + k] = v; }
            continue;
        }
        const nodeType = nodeTypes[prefix];
        const cr0 = CONTROL_RENAMES[name];
        const cr = cr0 && typeInScope(cr0.onTypes, nodeType) ? cr0 : null;
        const newName = cr ? cr.name : name;
        let newValue = value;
        const vmap = CONTROL_VALUE_RENAMES[newName];
        const vr = vmap && typeInScope(vmap.onTypes, nodeType) ? vmap.values[value] : null;
        if (vr) {
            if (vr.value !== undefined) {
                newValue = vr.value;
                report.push({ kind: "renamed", where: `${file} ${prefix}${newName}`, detail: `value ${value} → ${vr.value} (${vr.date})` });
            }
            if (vr.review) report.push({ kind: "review", where: `${file} ${prefix}${newName}`, detail: vr.review });
        }
        if (cr) {
            report.push({ kind: "renamed", where: `${file} ${key}`, detail: `${name} → ${cr.name} (${cr.date})` });
            if (cr.review) report.push({ kind: "review", where: `${file} ${prefix}${cr.name}`, detail: cr.review });
        }
        collide(prefix + newName);   // fires for either order: rename-then-plain or plain-then-rename
        out[prefix + newName] = newValue;
    }
    return out;
}

/// Apply every known rename to a backup bundle's files. Returns {files, report}: a NEW files
/// map (the input is not mutated) and the list of what changed / what needs review. Files that
/// are not .config JSON (scripts, anything unparsable) pass through untouched.
export function applyMigrations(files) {
    const out = {};
    const report = [];
    for (const [path, content] of Object.entries(files)) {
        let newPath = path;
        const base = path.slice(path.lastIndexOf("/") + 1);
        // Depth one only, mirroring the device's own one-level config rule: a PRESET the
        // user happened to name Layers.json is their file, not a container's.
        if (path === "/.config/" + base && FILE_RENAMES[base]) {
            const fr = FILE_RENAMES[base];
            newPath = path.slice(0, path.lastIndexOf("/") + 1) + fr.to;
            report.push({ kind: "renamed", where: path, detail: `file → ${newPath} (${fr.date})` });
        }
        if (newPath.startsWith("/.config/presets/")) {
            // Preset payloads reference the captured container by name; two known value renames.
            let text = content;
            for (const [oldV, pr] of Object.entries(PRESET_VALUE_RENAMES)) {
                const token = JSON.stringify(oldV);
                if (text.includes(token)) {
                    text = text.split(token).join(JSON.stringify(pr.to));
                    report.push({ kind: "renamed", where: newPath, detail: `preset value ${oldV} → ${pr.to} (${pr.date})` });
                }
            }
            out[newPath] = text;
            continue;
        }
        if (newPath.startsWith("/.config/") && newPath.endsWith(".json")) {
            try {
                const parsed = JSON.parse(content);
                out[newPath] = JSON.stringify(renameKeys(parsed, newPath, report));
                continue;
            } catch (_) {
                report.push({ kind: "review", where: newPath, detail: "not valid JSON; restored as-is" });
            }
        }
        out[newPath] = content;
    }
    return { files: out, report };
}

// ---------------------------------------------------------------------------------------------
// The backup/restore engine core: pure, injectable I/O, node-testable. app.js wires it to the
// real /api endpoints and the File Manager toolbar.
// ---------------------------------------------------------------------------------------------

/// Walk the whole device filesystem and build the backup bundle's files map. Returns
/// {files, skipped}. fetchDir(absPath) -> [{name, isDir, size}]; fetchFile(absPath) -> string.
/// Every file's byte length is verified against the listing's size: FEWER bytes than listed
/// (a read stopping short of its promised length) throws rather than archiving a silently
/// incomplete backup; MORE bytes means the file is not text (reading binary as UTF-8 inflates
/// via replacement characters), it is skipped and reported, since the bundle carries text only.
export async function collectFiles(fetchDir, fetchFile, root = "/") {
    const files = {};
    const skipped = [];
    // Transient machine output is not config: the HLS segment dir is rewritten every second
    // by the encoder and would only fill the report with skipped binaries.
    const kTransientDirs = ["/.hls"];
    async function walk(dir) {
        const entries = await fetchDir(dir);
        for (let e of entries) {
            const p = (dir === "/" ? "" : dir) + "/" + e.name;
            if (kTransientDirs.includes(p)) continue;
            if (e.isDir) {
                await walk(p);
            } else {
                let text;
                // A failed read (the file vanished between listing and fetch, or its path
                // exceeds the API's length cap) skips the file visibly instead of aborting
                // the whole backup; a SHORT read still aborts, silent truncation is worse.
                try { text = await fetchFile(p); }
                catch (_) { skipped.push(p); continue; }
                let bytes = new TextEncoder().encode(text).length;
                if (bytes !== e.size) {
                    // A mismatch can also be the device REWRITING the file between listing and
                    // fetch (its debounced config autosave): re-list once and re-read before
                    // judging, so a mid-walk save is not misread as truncation or binary.
                    const relisted = (await fetchDir(dir)).find(x => x.name === e.name);
                    if (relisted) e = relisted;
                    try { text = await fetchFile(p); }
                    catch (_) { skipped.push(p); continue; }
                    bytes = new TextEncoder().encode(text).length;
                }
                if (bytes < e.size) {
                    throw new Error(`${p}: got ${bytes} of ${e.size} bytes (truncated read)`);
                }
                if (bytes > e.size) { skipped.push(p); continue; }   // not text (UTF-8 inflation)
                files[p] = text;
            }
        }
    }
    await walk(root);
    return { files, skipped };
}

/// The directories a restore must create, parents before children (POST /api/dir is
/// non-recursive), deduplicated, root excluded.
export function restoreDirs(files) {
    const dirs = new Set();
    for (const path of Object.keys(files)) {
        const parts = path.split("/").filter(Boolean);
        let acc = "";
        for (let i = 0; i < parts.length - 1; i++) {
            acc += "/" + parts[i];
            dirs.add(acc);
        }
    }
    return [...dirs].sort((a, b) => a.split("/").length - b.split("/").length || (a < b ? -1 : 1));
}

/// The restore report: diff the restored .config module files against the live /api/state
/// module tree plus the /api/types registry. Returns entries {kind, where, detail} in the same
/// shape applyMigrations emits:
///  - "module": the file's module type does not exist on this firmware (re-add by hand)
///  - "control": a key's control name is gone (its value is back at the default)
/// typeNames (the /api/types registry) matters because a restored module only INSTANTIATES at
/// the next boot: a type that is registered but not yet in the live tree is fine, and its
/// control-level check is impossible until then (the robust loader covers it, unknown keys
/// are ignored). ReadOnly/Progress controls are never persisted, so they never appear in
/// config files and need no special casing here; child chains resolve through "N.type" keys.
export function diffRestore(files, stateModules, typeNames = []) {
    const report = [];
    const byType = new Map();
    (function walk(mods) {
        for (const m of mods || []) {
            if (!byType.has(m.type)) byType.set(m.type, m);
            walk(m.children);
        }
    })(stateModules);
    const known = new Set([...typeNames, ...byType.keys()]);

    for (const [path, content] of Object.entries(files)) {
        if (!path.startsWith("/.config/") || !path.endsWith(".json")) continue;
        if (path.startsWith("/.config/presets/")) continue;   // preset payloads, not module files
        const type = path.slice(path.lastIndexOf("/") + 1, -".json".length);
        if (!known.has(type)) {
            report.push({ kind: "module", where: path, detail: `module type ${type} does not exist on this firmware` });
            continue;
        }
        const top = byType.get(type);   // undefined when registered but not yet instantiated
        let cfg;
        try { cfg = JSON.parse(content); } catch (_) { continue; }   // applyMigrations already flagged it
        // Resolve each key's node by its child-prefix chain; "type" announces a child's type.
        const childTypes = {};   // prefix -> declared type
        for (const [key, value] of Object.entries(cfg)) {
            if (key === "type" || key.endsWith(".type")) childTypes[key.slice(0, -"type".length)] = value;
        }
        // Resolve the LIVE node positionally through the child-index chain, not by first
        // instance of the type: two siblings of one type can publish different control sets
        // (a script-defined module's controls depend on its own script). null = the type is
        // unknown (report it); undefined = no live node to check against yet (registered but
        // not instantiated, or the chain is not walkable), the robust loader covers it.
        const resolveNode = (prefix) => {
            let node = top;
            for (const part of prefix.split(".")) {
                if (part === "" || !node) break;
                node = (node.children || [])[Number(part)];
            }
            return node;
        };
        const controlsOf = (type_, prefix_) => {
            const node = resolveNode(prefix_);
            if (node && node.type === type_) return new Set((node.controls || []).map(c => c.name));
            return known.has(type_) ? undefined : null;
        };
        for (const [key] of Object.entries(cfg)) {
            const dot = key.lastIndexOf(".");
            const prefix = dot >= 0 ? key.slice(0, dot + 1) : "";
            const name = dot >= 0 ? key.slice(dot + 1) : key;
            if (name === "type" || name === "enabled") continue;   // structural, always understood
            const nodeType = prefix === "" ? type : childTypes[prefix];
            if (nodeType === undefined) continue;   // an orphan chain: the missing child is the real finding
            const controls = controlsOf(nodeType, prefix);
            if (controls === null) {
                report.push({ kind: "module", where: `${path} ${prefix}`, detail: `child module type ${nodeType} does not exist on this firmware` });
                delete childTypes[prefix];   // report each missing child once
                continue;
            }
            if (controls === undefined) continue;   // instantiates at reboot; loader is robust
            if (!controls.has(name)) {
                report.push({ kind: "control", where: `${path} ${key}`, detail: `control ${name} does not exist on ${nodeType}; its value is back at the default` });
            }
        }
    }
    return report;
}
