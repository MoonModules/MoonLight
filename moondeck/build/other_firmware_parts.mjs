// The files and offsets that install another project's firmware on a board, from the installer's own modules, so the bench script
// and the web installer read one copy of the rules: mooninstaller/other-firmwares.json, other-firmwares.js and manifest.js.
//
// Input on stdin, JSON: { firmware, build, description, features, flashSize }, the board as esptool reports it.
// Output on stdout, JSON: { name, version, chipFamily, parts: [{ url, offset, flashSize }] }, or { error, choices }.
//
// Run by moondeck/build/install_other_firmware.py.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { chipFamily, parseManifest } from "../../mooninstaller/manifest.js";
import { resolveOtherFirmwares, otherFirmwareOptions } from "../../mooninstaller/other-firmwares.js";
import { factsFromEsptool } from "../../src/ui/install-picker-devices.js";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const input = JSON.parse(readFileSync(0, "utf8"));
const reply = (value) => { process.stdout.write(JSON.stringify(value)); process.exit(0); };

const catalog = JSON.parse(readFileSync(join(ROOT, "mooninstaller", "other-firmwares.json"), "utf8"));
const wanted = String(input.firmware || "").toLowerCase();
const entry = catalog.find((e) => e.name.toLowerCase() === wanted) || catalog.find((e) => e.name.toLowerCase().startsWith(wanted));
if (!entry) reply({ error: `no firmware named ${input.firmware}`, choices: catalog.map((e) => e.name) });

await resolveOtherFirmwares([entry]);
const facts = factsFromEsptool(input);
const board = { family: chipFamily(input.description), flashMB: facts.flashMB, psramMode: facts.psramMode };
let options = otherFirmwareOptions([entry], board);
if (input.build) options = options.filter((o) => o.name.toLowerCase() === `${entry.name} ${input.build}`.toLowerCase());
if (options.length !== 1) {
    reply({ error: options.length ? `${entry.name} has several builds for this ${board.family}: pick one with --build`
                                  : `${entry.name} has no build${input.build ? ` ${input.build}` : ""} for this ${board.family} (${board.flashMB} MB)`,
            choices: otherFirmwareOptions([entry], board).map((o) => o.name.slice(entry.name.length + 1) || o.name) });
}

const { source, name, version } = options[0];
const manifestUrl = source.manifestUrl;
const manifest = manifestUrl ? await (await fetch(manifestUrl)).json() : source.manifest;
const builds = parseManifest(manifest, manifestUrl ? new URL(".", manifestUrl) : "https://localhost/");
const build = builds.find((b) => b.chipFamily === board.family);
reply({ name, version, chipFamily: board.family, parts: build.parts.map(({ url, offset, flashSize }) => ({ url, offset, flashSize })) });
