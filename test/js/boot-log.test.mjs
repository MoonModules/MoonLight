// What the installer reads from a booting device's serial log (mooninstaller/boot-log.js). Run: `node --test test/js/boot-log.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { bootLogIp, bootLogDevice } from "../../mooninstaller/boot-log.js";

test("an address split across two serial chunks is read only once it is complete", () => {
    const first = "tick: 2992us  MM_IP=192.168.1.2";   // the chunk ended inside the address
    assert.equal(bootLogIp(first), "");
    assert.equal(bootLogIp(first + "27  MM_DEVICE=MM-88C8.local  Filesystem:3us"), "192.168.1.227");
});

test("the device name is read once it is complete", () => {
    assert.equal(bootLogDevice("MM_DEVICE=MM-88C8.lo"), "");
    assert.equal(bootLogDevice("MM_DEVICE=MM-88C8.local  Filesystem:3us"), "MM-88C8.local");
});

test("firmware from before MM_IP is read from its status line", () => {
    assert.equal(bootLogIp("Network \u2014 WiFi: 192.168.1.30\n"), "192.168.1.30");
});
