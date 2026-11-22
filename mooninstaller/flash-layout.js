// The partition layout an install writes, chosen by the flash the installer measures: the pure core, importable in node:test,
// apart from the orchestrator's browser-only imports. The same rule as MoonDeck's serial flash (layout_for in
// moondeck/build/flash_esp32.py): the measured size where the firmware has a layout for it, else the largest that fits,
// none on a board smaller than every layout, and the smallest when the flash chip reports no size.

/**
 * @param {{flashSize: string, bootloader: string, partitionTable: string}[]} layouts - the manifest's layouts, smallest first ("8MB", ...)
 * @param {string|undefined} measured - esptool-js detectFlashSize(), such as "16MB", or undefined when unread
 * @returns {{layout: object|null, note: string}} the layout to write, null when none fits, and a line for the log
 */
export function layoutFor(layouts, measured) {
    const mb = (s) => parseInt(s, 10);
    const sizes = layouts.map((l) => mb(l.flashSize));
    if (!measured || !mb(measured)) {
        return { layout: layouts[0], note: `flash size unread: the smallest layout, ${layouts[0].flashSize}` };
    }
    const have = mb(measured);
    const exact = layouts.find((l) => mb(l.flashSize) === have);
    if (exact) return { layout: exact, note: `${measured} flash: the ${exact.flashSize} layout` };
    const fitting = layouts.filter((l) => mb(l.flashSize) <= have);
    if (!fitting.length) {
        return { layout: null, note: `${measured} flash, and this firmware installs into ${sizes.join(", ")} MB: a 4 MB board takes a -lite firmware` };
    }
    const largest = fitting[fitting.length - 1];
    return { layout: largest, note: `${measured} flash: the largest layout this firmware has, ${largest.flashSize}` };
}
