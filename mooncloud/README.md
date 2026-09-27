# MoonCloud

The server behind the MoonCloud card, and the only one MoonLight talks to. **Stats** takes a report and hands back the aggregates; **Talk** is a public message board between devices. One Worker, one database, one deploy. Published here so that "the deployed code is the published code" is checkable rather than taken on trust.

What it stores, and what it deliberately does not, is in [privacy-policy.md](../docs/legal/privacy-policy.md).

| File | |
|---|---|
| `worker.js` | both endpoints |
| `schema.sql` | the three tables |
| `wrangler.toml` | deployment config |
| `seed.sql` | sample rows for a local run, never deployed |

## Endpoints

**`POST /api/report`** takes one JSON report. Fields outside the allowlist are dropped rather than rejected, so an old device sending an old shape still counts. Returns `{"ok": true}`.

**`GET /api/stats`** returns aggregates only, never a row: totals plus counts by version, chip, device model, country, and by role for what a user added (drivers, services, layouts, effects, modifiers).

Any dimension narrows the whole answer, which is what makes a chart slice clickable: `?version=`, `?chip=`, `?deviceModel=`, `?flash=`, `?psram=`, `?sdk=`, `?country=`, `?event=`, `?driver=`, `?service=`, `?layout=`, `?effect=`, `?modifier=`, and `?dev=0|1`. Memory and light counts are stored as raw numbers and bucketed into ranges on read, so they filter by bounds instead: `?lightCountMin=1&lightCountMax=64`, likewise `totalHeap` and `freeHeap`. The answer carries `filtered: true` when any of them applied.

Column names come from an allowlist in the Worker, never from the query string, and values are bound.

**`POST /api/talk`** posts one message, bounded at 280 characters, carrying a device name only when that device's consent said to share it.

**`GET /api/talk`** returns the newest 50, `?since=<id>` for what a caller has not seen. The full sender id is never published: a message shows the shared name, else the first 8 characters of the id.

## Running it locally

```sh
uv run moondeck/run/run_mooncloud.py --seed     # --seed only the first time
```

`wrangler dev` runs `worker.js` in workerd, the runtime Cloudflare uses, against a local D1 under `.wrangler/`. What you verify here is what deploys. It binds `0.0.0.0` because wrangler's default answers only the machine itself and refuses every board on the network.

**A device cannot be pointed at this.** `MoonCloudModule::post` formats `https://` unconditionally, and `wrangler dev` serves plain HTTP, so changing `kHost` and rebuilding produces a TLS handshake against an HTTP port. Reporting is verified against the deployed Worker instead, which is what the firmware talks to anyway.

The BROWSER half can be pointed here: change `kMoonCloudUrl` in `src/ui/app.js` and rebuild, and the card reads its aggregates from the local server while the device keeps reporting to the real one.

## Deploying

Needs the owner's Cloudflare login. Stats and Talk run on Workers plus D1, both free tier.

```sh
npx wrangler login
npx wrangler d1 create mooncloud-stats                              # put the id in wrangler.toml
npx wrangler d1 execute mooncloud-stats --file=schema.sql --remote
npx wrangler deploy
```

**The firmware talks to the `workers.dev` address, and only that one.** A Custom Domain is optional, for people who want to read the aggregates without the app:

```toml
# Top level, BEFORE any table header: declaring a route disables workers.dev by default, and that is
# the address the firmware compiles in. Inside [[routes]] this would be a key of the route instead.
workers_dev = true

[[routes]]
pattern = "stats.example.org"
custom_domain = true
```

**Do not point the firmware at a Custom Domain without checking the issuer.** Cloudflare picks the CA, and the one it picked was absent from IDF's default `esp_crt_bundle`: every ESP32 handshake failed with "No matching trusted root certificate found" while desktop's system trust store accepted it, so devices went silent with nothing on screen to say why. The `workers.dev` certificate is one the bundle carries. Enabling `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` would cover any issuer, at roughly 30 KB of flash on a board already at 81% of its app slot.

Point the firmware at a new address only once that address answers, and only after a real device has completed a TLS handshake with it: a failed report is never retried, so every device reporting into an unreachable name loses that report.

## Changing the schema

`wrangler d1 execute` does all of it, `--remote` for the deployed database:

```sh
npx wrangler d1 execute mooncloud-stats --remote --command="ALTER TABLE reports ADD COLUMN cpu TEXT NOT NULL DEFAULT ''"
```

**Adding a field needs three edits in order**: the column, then `ALLOWED` in `worker.js` so it is no longer dropped on arrival, then the report builder in `MoonStatsModule.h`. Reversed, devices send a field the server discards.

`schema.sql` uses `CREATE TABLE IF NOT EXISTS`, so re-running it changes nothing; a new column needs `ALTER TABLE` and an edit to the file so a fresh deployment gets it too.

**A field that was never collected cannot be filled in later.** That is why the `events` table and the `dev` flag exist now rather than when they are needed.

Deleting rows has its own script, because it is the one operation waiting cannot undo: `moondeck/run/purge_mooncloud.py` asks for the row count first.

## Moving to another host

The data is plain SQLite and comes out in one command, worth running before any schema change regardless:

```sh
npx wrangler d1 export mooncloud-stats --remote --output=mooncloud-backup.sql
```

`worker.js` is the only thing to rewrite, and three parts of it are Workers-specific: `env.DB.prepare(...)` (any SQL library replaces it), the `fetch(request, env)` entry point (every serverless runtime has one), and `request.cf.country`. That last one is genuinely lost: elsewhere the country comes from a GeoIP lookup on the IP address, which means handling an address the privacy policy promises not to store.

Keep both addresses answering until the fleet has moved, since a report is sent once and never retried.

## Two things that are not obvious

**The country never comes from an address we store.** Cloudflare resolves it at the edge as `request.cf.country`, so no code here sees an IP. The privacy policy's promise is structural rather than a discipline someone maintains in a log config.

**The primary key is `(installationId, version)`.** A device re-reporting the same upgrade overwrites its row, so a row count counts installations rather than retries.

## State outside this repository

`git revert` removes the code and leaves all of this exactly as it is.

- **A Cloudflare account** (moonmodules@icloud.com), Free plan, no payment method: free Workers stop at their limits rather than billing. Undo: delete the Worker and database in the dashboard.
- **A D1 database** `mooncloud-stats`, id `7966e8d7-1fdd-4231-b28d-9dfc08ea94b1`, region WEUR. Undo: `npx wrangler d1 delete mooncloud-stats`.
- **A deployed Worker**. Undo: `npx wrangler delete mooncloud-stats`.
- **A `workers.dev` subdomain**, `moonmodules`, claimed on first deploy and never releasable, only left unused.
- **The nameservers for moonmodules.org**, moved at Esmero to `imani`/`pablo.ns.cloudflare.com`. **The only change that can take the docs site offline.** Undo: put `ns3`/`ns4.esmero.nl` back; Esmero keeps its zone file, so it is a form submission and a wait. The zone is ten records, all DNS-only rather than proxied because GitHub Pages serves its own certificate and proxying causes redirect loops with it: four A and four AAAA at GitHub Pages, `www` CNAME to `moonmodules.github.io`, and a `_discord` TXT.

## Cost on a device

+12,832 bytes of flash (+0.77%) and about 800 bytes of static RAM on a classic ESP32, measured by building the same tree with and without MoonCloud on one toolchain. The TLS stack was already linked for OTA, so `httpsPost` adds call-site code rather than a library.
