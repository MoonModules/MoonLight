// What a booting device's serial log says about where it is: the pure core of the installer's boot-log read, importable in
// node:test apart from the orchestrator's browser-only imports. The device prints `MM_IP=<dotted quad>` and
// `MM_DEVICE=<name>.local` on its tick line; serial reads chunk arbitrarily, so a token counts only once a character that
// cannot belong to it follows, or `MM_IP=192.168.1.2` would be taken from a chunk that ends before the `27`.

/** The device's address in `buf`, or "" until a complete one has arrived. */
export function bootLogIp(buf) {
    const m = buf.match(/MM_IP=(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})(?=[^\d.])/)
           || buf.match(/(?:Eth|WiFi):\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})(?=[^\d.])/);   // the status line of firmware from before MM_IP
    return m ? m[1] : "";
}

/** The device's mDNS name in `buf`, or "" until a complete one has arrived. */
export function bootLogDevice(buf) {
    const m = buf.match(/MM_DEVICE=(\S+\.local)(?=\s)/);
    return m ? m[1] : "";
}
