// Restore a backup bundle onto a running device through the UI's own restoreBundle, optionally with other clients
// reading the device meanwhile, then prove every value came back. Run by run_restore_live.py, which documents it.
//
//   node restore_backup.mjs <host> <bundle.json> [readers]

import { readFileSync } from "node:fs";
import { applyMigrations, restoreBundle } from "../../src/ui/migrate.js";

const [host, bundlePath, readersArg] = process.argv.slice(2);
const base = "http://" + host;
const readers = Number(readersArg || 0);
const at = (path, opts) => fetch(base + path, opts);

// What a device reports rather than stores, and what MIGRATING renames on the device itself, so a value differs by design: matched by path.
const SKIP = [/^System\.(firmware|Tasks\.tasks|Pins\.pins)$/, /\.Devices\.devices$/,
              /^Drivers\.palette$/, /^Drivers\.[^.]+\.(fixture|fixtureRef)$/, /\.FixtureProfiles\.profiles$/, /\.Ethernet\.ethBoard$/];

const system = async () => (await (await at("/api/system")).json());
const bootReason = async () => {
    const mods = (await (await at("/api/modules/System")).json());
    const find = (o) => o && typeof o === "object" ? (o.name === "bootReason" ? o.value : Object.values(o).map(find).find(v => v !== undefined)) : undefined;
    return find(mods);
};
const flat = (o, p = "", out = {}) => {
    for (const [k, v] of Object.entries(o)) {
        if (v && typeof v === "object" && !Array.isArray(v)) flat(v, p + k + ".", out);
        else if (!SKIP.some(re => re.test(p + k))) out[p + k] = JSON.stringify(v);
    }
    return out;
};

const bundle = JSON.parse(readFileSync(bundlePath, "utf8"));
const before = await system();

// Other clients reading the device for the whole restore, as a second browser or a script does.
let reading = readers > 0, reads = 0, cut = 0;
const reader = async () => {
    const paths = ["/api/state", "/api/file?path=.config%2FEffects.json", "/api/system"];
    for (let i = 0; reading; i++) {
        try { const r = await at(paths[i % paths.length]); await r.text(); reads++; } catch (_) { cut++; }
    }
};
const pool = Array.from({ length: readers }, reader);

const { entries } = await restoreBundle(bundle, at);
reading = false;
await Promise.all(pool);
await new Promise(r => setTimeout(r, 1500));   // a written config file applies on the device's next 20 ms poll

let failures = 0;
const after = await system().catch(() => null);
if (!after || after.uptime < before.uptime) {
    failures++;
    console.log(`FAIL the device restarted during the restore (bootReason ${await bootReason().catch(() => "unknown")})`);
}
for (const e of entries.filter(e => e.kind !== "renamed")) { failures++; console.log(`FAIL report: ${e.where}: ${e.detail}`); }

// Every value the migrated bundle holds is back on the device, as it reads its own config files.
const live = (await (await at("/api/state")).json()).modules || [];
const { files } = applyMigrations(bundle.files, live);
for (const [path, text] of Object.entries(files)) {
    if (!path.startsWith("/.config/") || !path.endsWith(".json") || path.includes("/presets/")) continue;
    const r = await at("/api/file?path=" + encodeURIComponent(path.slice(1)));
    if (!r.ok) { failures++; console.log(`FAIL ${path}: not on the device (${r.status})`); continue; }
    const want = flat(JSON.parse(text)), have = flat(JSON.parse(await r.text()));
    const lost = Object.keys(want).filter(k => have[k] !== want[k]);
    if (lost.length) {
        failures++;
        console.log(`FAIL ${path}: ${lost.length} of ${Object.keys(want).length} values differ`);
        for (const k of lost.slice(0, 8)) console.log(`     ${k}: backup ${want[k]}, device ${have[k] ?? "(absent)"}`);
    } else {
        console.log(`ok   ${path}: every value back (${Object.keys(want).length})`);
    }
}
if (readers) console.log(`     ${readers} reader(s) made ${reads} reads during the restore, ${cut} cut off`);
console.log(failures ? `Restore: ${failures} failure(s).` : "Restore: every value back, the report clean.");
process.exit(failures ? 1 : 0);
