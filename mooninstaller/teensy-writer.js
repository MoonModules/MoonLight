// The installer's Teensy 4.1 writer: PJRC's HalfKay bootloader protocol over WebHID, as teensy_loader_cli speaks it.
//
// Dependency-free and import-safe in node (browser APIs are touched only inside the async functions), so test/js pins the report layout.
//
// The protocol, from teensy_loader_cli.c:
//   - a running board enters its bootloader when its USB serial port is opened at 134 baud;
//   - the bootloader is a HID device, 16C0:0478, taking 1088-byte output reports: a 64-byte header whose first 3 bytes are the block's offset from the flash base, little-endian, then 1024 image bytes;
//   - block 0 goes first because it erases the chip, so the first writes wait up to 45 s and the rest 0.5 s;
//   - a block of only 0xFF after the first is skipped, since the chip is erased;
//   - a report whose first 3 bytes are 0xFF boots the new program.

export const PJRC_VID = 0x16C0;
export const TEENSY_SERIAL_PID = 0x0483;   // a Teensy running a program built with USB type Serial, as MoonLight and the factory blink are
export const HALFKAY_PID = 0x0478;
export const TEENSY_FAMILY = "Teensy 4.1";

const BLOCK_BYTES = 1024;
const HEADER_BYTES = 64;
export const REPORT_BYTES = HEADER_BYTES + BLOCK_BYTES;
// The program flash HalfKay writes on a Teensy 4.1: 8 MB less the 256 KB the core reserves at the top.
export const TEENSY41_CODE_BYTES = 8126464;
// MoonLight's settings: a 1 MB LittleFS directly below that reserved 256 KB (platform_teensy.cpp, ProgramFlashFs), in 4 KB blocks.
// Zeroing its first two blocks, the superblock pair, leaves nothing to mount, so the firmware formats it at boot: an erase of
// every setting that leaves the program alone, which the bootloader cannot do otherwise, since it erases the program area only.
export const TEENSY41_SETTINGS_OFFSET = TEENSY41_CODE_BYTES - 1024 * 1024;
const SETTINGS_SUPERBLOCK_BYTES = 2 * 4096;
const ERASE_WRITES = 5;              // teensy_loader_cli's `block_count <= 4`: the writes the chip erase can stall
const ERASE_TIMEOUT_MS = 45000;
const BLOCK_TIMEOUT_MS = 500;
const BOOTLOADER_BAUD = 134;

export function isTeensySerial(info) {
    return !!info && info.usbVendorId === PJRC_VID && info.usbProductId === TEENSY_SERIAL_PID;
}

// Why an image cannot go to a Teensy 4.1, or "" when it can: the flat image starts with the FlexSPI configuration block, "FCFB".
export function imageProblem(image) {
    if (!(image instanceof Uint8Array) || image.length === 0) return "the image is empty";
    if (image[0] === 0x3A) return "that is a HEX file, the installer writes the .bin";
    if (image.length < 4 || String.fromCharCode(...image.subarray(0, 4)) !== "FCFB") return "not a Teensy 4 image";
    if (image.length > TEENSY41_CODE_BYTES) return `the image is ${image.length} bytes, more than the ${TEENSY41_CODE_BYTES} a Teensy 4.1 holds`;
    return "";
}

// The reports that write an image, in order: block 0 always, every later block unless it is all 0xFF.
export function halfkayReports(image) {
    const reports = [];
    for (let offset = 0; offset < image.length; offset += BLOCK_BYTES) {
        const block = image.subarray(offset, offset + BLOCK_BYTES);
        if (offset > 0 && block.every(b => b === 0xFF)) continue;
        const report = new Uint8Array(REPORT_BYTES).fill(0);
        report[0] = offset & 0xFF;
        report[1] = (offset >> 8) & 0xFF;
        report[2] = (offset >> 16) & 0xFF;
        report.fill(0xFF, HEADER_BYTES);   // a short last block reads as erased flash beyond the image
        report.set(block, HEADER_BYTES);
        reports.push({ offset, report });
    }
    return reports;
}

// The reports that zero the settings' superblock pair.
export function eraseSettingsReports() {
    const reports = [];
    for (let offset = TEENSY41_SETTINGS_OFFSET; offset < TEENSY41_SETTINGS_OFFSET + SETTINGS_SUPERBLOCK_BYTES; offset += BLOCK_BYTES) {
        const report = new Uint8Array(REPORT_BYTES).fill(0);
        report[0] = offset & 0xFF;
        report[1] = (offset >> 8) & 0xFF;
        report[2] = (offset >> 16) & 0xFF;
        reports.push({ offset, report });
    }
    return reports;
}

export function bootReport() {
    const report = new Uint8Array(REPORT_BYTES).fill(0);
    report[0] = report[1] = report[2] = 0xFF;
    return report;
}

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// Asks a running Teensy into its bootloader: PJRC's core reboots when its serial port is set to 134 baud.
export async function rebootToBootloader(port) {
    try {
        await port.open({ baudRate: BOOTLOADER_BAUD });
    } catch (e) {
        // Held open by the installer at another rate: close it and open it again at the bootloader's.
        if (e && e.name === "InvalidStateError") {
            try {
                await port.close();
                await port.open({ baudRate: BOOTLOADER_BAUD });
            } catch (_) { /* held by a reader, or already rebooting: the bootloader step that follows says what to do */ }
        }
    }
    await sleep(100);
    try { await port.close(); } catch (_) { /* the device left the bus */ }
}

const HALFKAY_FILTER = { vendorId: PJRC_VID, productId: HALFKAY_PID };

// The browser's device chooser for the bootloader; it needs a user gesture, so call it inside a click. Null when the user cancels.
// Chrome does not remember the grant (seen on the bench, although the bootloader reports a USB serial number): every install asks.
export async function requestHalfKay() {
    const devices = await navigator.hid.requestDevice({ filters: [HALFKAY_FILTER] });
    return devices && devices.length ? devices[0] : null;
}

// One report, retried while the bootloader is busy (erasing), as teensy_loader_cli's teensy_write retries until its timeout.
async function sendReport(device, report, timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        const remaining = deadline - Date.now();
        try {
            await Promise.race([
                device.sendReport(0, report),
                sleep(Math.max(remaining, 1)).then(() => { throw new Error("timeout"); }),
            ]);
            return;
        } catch (e) {
            if (Date.now() >= deadline) throw new Error(`the Teensy bootloader stopped answering (${e && e.message || e})`);
            await sleep(10);
        }
    }
}

// Writes an image through an opened or unopened HalfKay device, with `eraseSettings` also the settings, and boots it; onProgress gets 0..100.
export async function writeImage(device, image, { onProgress, onLog, eraseSettings = false } = {}) {
    const problem = imageProblem(image);
    if (problem) throw new Error(problem);
    if (!device.opened) await device.open();
    const reports = halfkayReports(image);
    if (eraseSettings) {
        if (onLog) onLog("[teensy] erasing the settings as well");
        reports.push(...eraseSettingsReports());
    }
    if (onLog) onLog(`[teensy] writing ${reports.length} blocks of ${BLOCK_BYTES} bytes`);
    for (let i = 0; i < reports.length; i++) {
        await sendReport(device, reports[i].report, i < ERASE_WRITES ? ERASE_TIMEOUT_MS : BLOCK_TIMEOUT_MS);
        if (onProgress) onProgress(Math.round(((i + 1) * 100) / reports.length));
    }
    if (onLog) onLog("[teensy] booting the new program");
    try {
        await device.sendReport(0, bootReport());
    } catch (_) {
        // The bootloader may leave the bus before it acknowledges the boot report.
    }
    try { await device.close(); } catch (_) { /* already gone */ }
}

// The browser's port chooser narrowed to a running Teensy; it needs a user gesture, so call it inside a click.
export async function requestTeensySerial() {
    return await navigator.serial.requestPort({ filters: [{ usbVendorId: PJRC_VID, usbProductId: TEENSY_SERIAL_PID }] });
}

// The granted Teensy serial ports of every board but the one being installed, noted before the install.
export async function otherTeensyPorts(installing) {
    const ports = await navigator.serial.getPorts();
    return ports.filter(p => p !== installing && isTeensySerial(p.getInfo()));
}

// The installed board's serial port after the reboot, which re-enumerates; null when none appears in `waitMs`.
// A port reports only its USB ids, which every Teensy shares, so the board is the Teensy port that is none of `others`: another granted board is never read or sent defaults.
export async function rebootedSerialPort(waitMs, others = []) {
    const deadline = Date.now() + waitMs;
    for (;;) {
        const ports = await navigator.serial.getPorts();
        const port = ports.find(p => isTeensySerial(p.getInfo()) && !others.includes(p));
        if (port) return port;
        if (Date.now() >= deadline) return null;
        await sleep(500);
    }
}
