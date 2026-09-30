// The bookmarklet backup (mooninstaller/backup-snippet.js): the zero-install path for a
// device running firmware from before the File Manager's Backup button. Run against a mocked
// old device, it must produce exactly the bundle the new Restore button accepts.
// Run: `node --test test/js/backup-snippet.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";

import { BACKUP_SNIPPET } from "../../mooninstaller/backup-snippet.js";

// A mocked device: fetch/document/alert/location/URL stand-ins the snippet runs against.
function mockDevice({ dirs, files, state, corruptRead }) {
    const captured = { download: null, blob: null, alerts: [] };
    const fetch = async (url) => {
        const u = new URL(url, "http://device.local");
        const path = u.searchParams.get("path");
        if (u.pathname === "/api/dir") return { ok: true, json: async () => dirs[path] };
        if (u.pathname === "/api/file") {
            const text = corruptRead === path ? files[path].slice(0, -1) : files[path];
            return { ok: true, arrayBuffer: async () => new TextEncoder().encode(text).buffer };
        }
        if (u.pathname === "/api/state") return { ok: true, json: async () => state };
        return { ok: false, status: 404 };
    };
    const document = {
        createElement: () => ({ click() {}, remove() {}, style: {}, set download(v) { captured.download = v; }, get download() { return captured.download; } }),
        body: { appendChild(a) { captured.download = a.download; } },
    };
    const env = {
        fetch, document,
        alert: (msg) => captured.alerts.push(msg),
        location: { hostname: "device.local", origin: "http://device.local" },
        URL: { createObjectURL: (blob) => { captured.blob = blob; return "blob:x"; }, revokeObjectURL() {} },
    };
    return { env, captured };
}

async function runSnippet(env) {
    const body = BACKUP_SNIPPET.slice("javascript:".length);
    await new Function(...Object.keys(env), `return ${body}`)(...Object.values(env));
}

const DEVICE = {
    dirs: {
        "/": [{ name: ".config", isDir: true, size: 0 }, { name: "scripts", isDir: true, size: 0 }],
        "/.config": [{ name: "Network.json", isDir: false, size: 15 }, { name: "presets", isDir: true, size: 0 }],
        "/.config/presets": [{ name: "p1.json", isDir: false, size: 8 }],
        "/scripts": [{ name: "a.mle", isDir: false, size: 7 }, { name: "fw.bin", isDir: false, size: 2 }, { name: "bom.mle", isDir: false, size: 10 }],
    },
    files: {
        "/.config/Network.json": '{"enabled":true}'.slice(0, 15),
        "/.config/presets/p1.json": '{"x":1}\n',
        "/scripts/a.mle": "let x=1",
        "/scripts/bom.mle": "\uFEFFlet y=2",   // BOM-prefixed: 3 BOM bytes count in its size
        "/scripts/fw.bin": "\uFFFD\uFFFD",   // 6 bytes for a 2-byte file: binary read as text
    },
    state: { modules: [{ type: "System", controls: [
        { name: "deviceName", value: "schelpje" }, { name: "firmware", value: "esp32-4mb" }, { name: "build", value: "2.1.0-dev" },
    ], children: [] }] },
};

test("the bookmarklet downloads a complete bundle the Restore button accepts", async () => {
    const { env, captured } = mockDevice(DEVICE);
    await runSnippet(env);
    assert.equal(captured.alerts.length, 1);
    assert.match(captured.alerts[0], /4 files/);
    assert.match(captured.alerts[0], /skipped \(not text\): \/scripts\/fw\.bin/);
    assert.match(captured.alerts[0], /private/);
    const bundle = JSON.parse(await captured.blob.text());
    assert.equal(bundle.format, "MoonLight-config-backup");
    assert.equal(bundle.version, 1);
    assert.equal(bundle.device, "schelpje");
    assert.equal(bundle.firmware, "esp32-4mb");
    assert.deepEqual(Object.keys(bundle.files).sort(),
        ["/.config/Network.json", "/.config/presets/p1.json", "/scripts/a.mle", "/scripts/bom.mle"]);
    assert.equal(bundle.files["/scripts/bom.mle"], "\uFEFFlet y=2");   // BOM preserved byte-exact
    assert.equal(bundle.files["/scripts/a.mle"], "let x=1");
    assert.match(captured.download, /^MoonLight-config-schelpje-\d{4}-\d{2}-\d{2}\.json$/);
});

test("a truncated read fails the backup loudly instead of archiving an incomplete file", async () => {
    const { env, captured } = mockDevice({ ...DEVICE, corruptRead: "/scripts/a.mle" });
    await runSnippet(env);
    assert.equal(captured.blob, null);   // no download happened
    assert.match(captured.alerts[0], /Backup failed/);
    assert.match(captured.alerts[0], /truncated/);
});

test("without /api/state the bundle still builds, named by hostname", async () => {
    const { env, captured } = mockDevice({ ...DEVICE, state: undefined });
    env.fetch = (orig => async (url) => url.includes("/api/state") ? { ok: false, status: 404, json: async () => { throw Error("404"); } } : orig(url))(env.fetch);
    await runSnippet(env);
    const bundle = JSON.parse(await captured.blob.text());
    assert.equal(bundle.device, "device.local");
    assert.equal(Object.keys(bundle.files).length, 4);
});
