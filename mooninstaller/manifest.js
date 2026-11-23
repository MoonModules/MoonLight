/// The installer's pure manifest rules, with no browser or esptool dependency, so the page, the bench script and the tests share one copy:
/// a chip family from esptool's description, a manifest's builds, and a bootloader re-flagged for another flash size.

// The chip family ("ESP32", "ESP32-S3", "ESP32-P4"), the vocabulary deviceModels.json's `chip`, build_esp32's TARGET_TO_FAMILY and the
// ESP Web Tools manifest's `chipFamily` share, from esptool's description of the specific silicon ("ESP32-D0WD-V3",
// "ESP32-S3 (QFN56) (revision v0.2)"). It keeps the family token and drops the package and revision, so a new ESP32-C5 needs no
// change here, and classic silicon (ESP32-D0WD*, ESP32-PICO-*, ESP32-U4WDH: one family token) is plain "ESP32". The token is matched
// anywhere in the text, since esptool-js prefixes "unknown " to silicon newer than it knows ("unknown ESP32-P4 (revision v1.3)").
export function chipFamily(chipName) {
    const s = String(chipName || "").trim();
    const sub = s.match(/ESP32-([A-Z]\d+)\b/);   // a sub-family: S3, P4, C3, C6, H2 and the rest
    if (sub) return "ESP32-" + sub[1];
    if (/ESP32\b/.test(s)) return "ESP32";       // classic silicon
    return s;                                     // unrecognized, passed through so it shows
}

// An ESP Web Tools manifest's flashable builds, one per chip family, each as { chipFamily, parts: [{ url, offset }], layouts },
// its part paths resolved against `base`. Only builds with parts count (ESPHome also lists chips it updates over the air only),
// and a manifest with none errors here, before esptool-js meets undefined data. MoonLight generates one build per firmware
// (moondeck/build/generate_manifest.py); another project's manifest has one per chip, and the install takes the connected chip's.
//   { name, version, builds: [{ chipFamily, parts: [{ path, offset }], layouts }] }
export function parseManifest(m, base) {
    const builds = (m && Array.isArray(m.builds) ? m.builds : []).filter((build) => Array.isArray(build.parts) && build.parts.length);
    if (!builds.length) throw new Error("manifest has no build with parts[]");
    return builds.map((build) => {
        return {
            chipFamily: build.chipFamily,
            parts: build.parts.map(p => ({
                url: new URL(p.path, base).toString(),
                offset: p.offset,
                flashSize: p.flashSize,   // a bootloader to re-flag for this flash size (patchBootloaderFlashSize), absent for every other part
            })),
            // The bootloader and table per flash size (generate_manifest.py), the default parts being the smallest's; absent in a release from before it.
            layouts: (build.layouts || []).map(l => ({ flashSize: l.flashSize, partitionTable: new URL(l.partitionTable, base).toString(),
                                                       bootloader: l.bootloader && new URL(l.bootloader, base).toString() })),
        };
    });
}

// The flash-size code each ESP32-family image header carries in the high nibble of its fourth byte.
const FLASH_SIZE_NIBBLE = { "4MB": 2, "8MB": 3, "16MB": 4, "32MB": 5 };

// Re-flag a bootloader image for another flash size, as esptool's --flash-size does and WLED's web installer does in the
// browser: the size nibble in the 8-byte header, then the SHA-256 digest appended after the segments, which covers the header.
// Layout: 8-byte header and 16-byte extended header (its last byte the append-digest flag), the segments (8-byte header and
// data each), padding to 16 bytes, the checksum byte, then the 32-byte digest.
export async function patchBootloaderFlashSize(buffer, flashSize) {
    const nibble = FLASH_SIZE_NIBBLE[flashSize];
    if (nibble === undefined) throw new Error(`unknown flash size: ${flashSize}`);
    const data = new Uint8Array(buffer.slice(0));
    if (data[0] !== 0xe9) throw new Error("not an ESP image (bad magic byte)");
    const view = new DataView(data.buffer);
    let pos = 24;
    for (let i = 0; i < data[1]; i++) pos += 8 + view.getUint32(pos + 4, true);
    const hashStart = pos + ((16 - ((pos + 1) % 16)) % 16) + 1;
    data[3] = (data[3] & 0x0f) | (nibble << 4);
    if (data[23]) data.set(new Uint8Array(await crypto.subtle.digest("SHA-256", data.slice(0, hashStart))), hashStart);
    return data;
}
