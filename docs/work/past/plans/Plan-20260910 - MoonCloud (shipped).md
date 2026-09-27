# Plan: MoonCloud

## MoonCloud

**MoonCloud** is the family name for everything MoonLight does with a server we run. One Worker, one database, one deploy, and one container module in the UI holding a child per member.

Each member is a separate feature with its own consent, because agreeing to share a chip model says nothing about wanting to publish messages. Each gets its own prompt, its own consent control, and its own paragraph in the privacy policy.

| Member | What it is | Status |
|---|---|---|
| **Stats** | one opt-in report per install or upgrade, and the aggregates back | built |
| **Talk** | a public message board between devices, Meshtastic-style | built |
| **Sync** | device to device: a joint show across houses | designed, see below |

**The installation id is the one thing they share.** It identifies an installation across every member, so a device reads as the same one to Stats and to Talk, and any future pairing has an identity to work with.

### Stats

We want to know what MoonLight runs on, so effort goes where the users are. Today that is guesswork from Discord and issue reports, which over-represent whoever is loudest and miss everyone quietly running a board that works.

One report per install or upgrade, sent only after the user says yes, describing hardware and configuration: chip, flash, PSRAM, SDK, board, and which modules are enabled. The aggregates come back to the card that asked for consent, so contributing earns the answer.

### Talk

A public message board between devices, in the shape Meshtastic's channel chat has. A message is sent because somebody typed one.

Two consents: `consent` allows posting, and `shareName` (off by default) allows the device name to ride along. A device name is the one field here that identifies a person rather than a machine, so sharing it is its own decision. Without it a message shows the first 8 characters of the installation id, which groups one device's messages while leaving the sender unnamed.

Reading sends no identifier, and happens only while consent is on: a device whose owner said no makes no request to our server at all.

### Sync

A joint show across houses: several installations running one synchronized animation together.

**The same deployment.** Cloudflare Workers terminates WebSockets natively, and a Durable Object is a single addressable instance with in-memory state: one per group, which is exactly what a group is. So `mooncloud/worker.js` grows a `/sync/<groupId>` route beside the others and one deploy still carries everything.

**A different transport, which is why it is its own member.** Stats is a single POST per firmware install; a shared show needs persistent connections, presence, groups and time alignment.

Two things shape the design. **Durable Objects need a paid Cloudflare plan** (Stats and Talk run on Workers plus D1, both free), so Sync is the member that starts costing money. And **the hard part is device-side**: round-trip latency to the edge is tens of milliseconds and varies per device, so the shape that works is to sync clocks, agree a start time in the future, then run the animation locally from each device's own clock.

**Talk is the proof of the plumbing**: identity, consent, and a shared stream that devices both write to and read from, on the infrastructure Sync would use.

## The installation id

Every member identifies an installation the same way: **`SHA-256(salt || platform::getMacAddress())`, truncated to 16 bytes, 32 hex characters. One scheme on every target.**

Per target, the seed is whatever that platform means by "this installation": the eFuse MAC on ESP32, and on desktop and in Docker a random locally-administered address generated once and persisted to `<root>/.config/identity`, following systemd's machine-id pattern. The hashing code takes six bytes from the platform layer, which is why it sits in `src/core/` rather than behind the platform seam: a new target inherits a correct id by implementing `getMacAddress` alone.

**The id is stable across a reflash, a factory reset and an image upgrade.** That is what makes an upgrade distinguishable from a new install, and it is the property the whole feature rests on. An identifier that changed on reflash would report every bench board as a stream of fresh installs.

**The salt is MoonCloud-specific and differs from every other salt in the project.** The same MAC produces the MQTT topic prefix and the Home Assistant `unique_id`, both visible on the user's own network, so a distinct salt keeps a report from being tied to a device somebody can observe locally. Measured on the bench: MAC `2A:DA:B7:C6:A0:94` gives id `c26086e8...`, where an unsalted hash gives `26a61cc4...`; a test asserts the report carries neither the address nor the unsalted form.

**It is pseudonymous, and the privacy policy says so.** Two reports carrying one id came from one installation: that is the point. An ESP32 id is reversible in principle by anyone determined, since the salt is in open source and a MAC is short; a desktop id is seeded from `std::random_device` and is not. An install that predates the identity file keeps the historic address, so upgrading leaves a device's name and topics alone, at the cost of those installs sharing one id.

**On a read-only container filesystem the address is valid for that run and retried next start.** An install holding config keeps the historic address every start; a fresh install generates a new one each start and so reports as a new installation each time. Rare, invisible from the server, recorded so an anomaly in the desktop numbers has an explanation.

**Where the design came from.** A random per-install id was the first draft, and it wrecks the install-versus-upgrade split: a reflash yields a new id, so every reflashed board reports as a fresh install. A hardware-consistent id is the answer, and the scheme above is it. Reporting into an existing usage server was the other route considered, and running our own turned out to fit better, since the fields worth collecting diverge.

## Steps: Stats

The first member, and the one that proves the plumbing every later one uses.

### 1. The report builder, as a pure function (DONE)

`buildMoonStatsReport` in `src/core/system/MoonStatsModule.h`: one function from the live module tree to a JSON string, with no network, consent or persistence involved. It reads what is already in memory: `chip`, `flash`, `psramType`, `sdk` and `deviceModel` from SystemModule, `version` from FirmwareUpdateModule, and the modules a user ADDED, each tagged by its role, from the tree.

**Fields are named one at a time and copied by name**, which is the design rather than an implementation detail: `deviceName`, `mac`, `ssid` and `password` are live controls sitting beside `chip` and `flash`, so a builder that emitted what it found would leak on its first run.

**Its test is the privacy policy made executable.** `unit_MoonStatsReport.cpp` builds a tree stuffed with a device name, a MAC, an SSID, a password and a user's free text, then asserts the report carries none of them, checking values and keys alike. **Control-checked by sabotage**: made the builder emit `deviceName` and watched the test fail on both.

### 2. The installation id (DONE)

`src/core/sha256.{h,cpp}` plus `installationId()` in `src/core/system/MoonCloudModule.h`, implementing the scheme above.

**SHA-256 is vendored, about 150 lines.** ESP-IDF ships mbedtls and the desktop would link something else, so a library means two implementations of one function that must agree byte for byte, where a divergence produces ids that silently differ between platforms. One file is the smaller thing to own, the algorithm is frozen, and `unit_sha256.cpp` pins it against the published FIPS 180-4 vectors (verified independently against Python's `hashlib` before being trusted), including the 55/56/64-byte cases where padding bugs hide.

The id is gated on consent: `Never` yields an empty string, so a user who declined has none.

### 3. Consent, and the one-time trigger (DONE)

`src/core/system/MoonStatsModule.h`: `consent` (a checkbox, off by default) and `reportedVersion`, both persisted like any other control. `reportDue()` is true when consent is on and the running version differs from the recorded one, so a reboot sends nothing and an upgrade sends exactly one report.

Install and upgrade are told apart by `reportedVersion` alone: empty means this install has never reported. **Control-checked by sabotage**: removing the consent gate fails the test.

`MoonCloudModule` is the container, with Stats and Talk as children, and both are marked `markWiredByCode()` so the tree comes from the code rather than from whatever the config file last recorded.

The card explains what consent buys above the checkbox, and `inApMode()` asks the platform directly rather than having NetworkModule push the fact in: a copy nothing updates is worse than no copy.

### 4. The send (DONE)

One POST, fire and forget, off the render thread on `tick1s`. A failure is silent and not retried: a lost report costs one row in an aggregate, where a retry queue is persistence, scheduling and a failure mode for something nobody is waiting for. `markReported()` runs on hand-off, so an unreachable server leaves the device quiet rather than re-sending every boot.

**HTTPS through one seam, `platform::httpsPost`, using the TLS each OS already ships.** libcurl on macOS and Linux, present on both and found with `find_package(CURL)`; WinHTTP on Windows, which is in the SDK and needs nothing acquired; `esp_http_client` with `esp_crt_bundle_attach` on ESP32, the same pair the OTA path uses, so a device adds call-site code rather than a TLS stack. Nothing is vendored and no certificate store is ours to maintain.

`platform::httpRequest` stays what it is: a LAN socket for the Philips Hue v1 API, plain HTTP, host given as a dotted-quad IP. `httpsPost` is the one that resolves names and verifies certificates.

**libcurl is optional.** Without it a macOS or Linux build succeeds, `MM_HAVE_CURL` stays undefined and `httpsPost` returns false, so a build never fails over an opt-in statistic. CMake prints which way it went, and fails a packaging build outright (`MM_PACKAGING`), since a published binary that asks for consent must be able to honor it. Windows is exempt: it never uses libcurl.

Verified against live endpoints: a valid certificate sends, and `expired.badssl.com` is refused, so the verification is real. `VERIFYPEER` and `VERIFYHOST` are set explicitly rather than left to curl's defaults, so a later edit has to say out loud that it is turning them off.

### 5. The server, API only (DONE)

Cloudflare Workers plus D1, EU jurisdiction pinned. Four endpoints: `POST /api/report`, `GET /api/stats`, `POST /api/talk`, `GET /api/talk`.

**Workers rather than a box we own, for one specific reason**: `request.cf.country` resolves the country at the edge, so the promise that the IP address is never stored is structural. On our own server that line would be one config change away from being false.

Its source is public, in this repository, for the same reason the firmware is.

Built as `mooncloud/`: `worker.js`, `schema.sql`, `wrangler.toml`, `seed.sql` for local sample rows. `uv run moondeck/run/run_mooncloud.py --seed` runs it under workerd, the same runtime Cloudflare uses, so the local server is the deployed server with a local database.

Verified end to end: a report carrying `deviceName`, `mac`, `ssid` and `password` stored only the allowlisted fields, and three re-posts of one id and version left the installation count unchanged. Eight contract tests in `test/js/mooncloud-report.test.mjs`, control-checked by adding `deviceName` to the allowlist and watching them fail.

### 6. The aggregates on the device's own card (DONE)

The Stats card fetches `GET /api/stats` and renders what everyone reported as pie charts: version, chip, board and country.

**The card is the UI, which is why the server is an API.** There is one place to build and keep in sync with the schema, contributing earns something visible in return, and a user sees the shape of the data their own report joined. `/api/stats` is readable by anyone, so it carries aggregates that are safe to show the world, which counts of chips and versions are.

Fetched when the card is opened. A failure says so on the card ("Cannot reach the MoonCloud server.") rather than rendering as an empty section: an unreachable server and one with nothing in it lead a reader to different actions.

The address is compiled in (`kHost` in `MoonCloudModule.h`, `kMoonCloudUrl` in `src/ui/app.js`), so one constant drives both the report and the fetch. HTTPS only: there is one MoonCloud, and a device that could be pointed elsewhere could be pointed at nothing, where a failed report is never retried. Moving the server is a release.

## Scope

- **One report per install or upgrade.** The trigger is a version change, so a device that keeps running stays quiet.
- **Hardware and configuration only.** "Which effects are most used" is a genuinely interesting question and a different promise: it would need its own consent and its own policy paragraph.
- **A failed send is forgotten.** See step 4.

## Verification

1. `build_desktop.py --tests` and `test_desktop.py`: the forbidden-fields test is the one that matters.
2. The consent prompt on a desktop run: Never is remembered across a restart, and a declined install opens no connection (verifiable with a local listener that should never be reached).
3. On the bench, ONE board (PO's call): the prompt appears after an upgrade, Yes sends one report, and a second boot stays quiet.
4. End to end: a report from a real device appears in `GET /api/stats`, and its row holds a country and no address.

## Risks

- **The totals are trust-based.** Any sender id can be claimed, so the numbers rest on people having no reason to fabricate them. Accepted: the alternative is authenticating users to defend a statistic. Rate limiting at the edge blunts casual abuse, and the privacy policy says this out loud.
- **A prompt that annoys is worse than no data.** It appears once per install or upgrade, and Never is permanent. If it ever fires more often than that, it is a bug. (The refresh button sends on demand, which is the user asking rather than the device prompting.)
- **`std::random_device` is deterministic on some libstdc++ targets**, which would make two fresh containers share an id. Container-only, and detectable in the data as an implausibly popular id.
- **The server is a standing commitment.** It needs an owner, a domain and an account, and that rather than the code is what kept this in the backlog.

## The server is owned

**ewowi owns the Cloudflare account** (settled 2026-09-10). That was the last open question and the one that kept this feature in the backlog: the code was never the blocker, a person willing to hold the account was. Stats and Talk run on Workers plus D1, both free tier; Sync would need Durable Objects, which are paid, so it is also the point where MoonCloud starts costing money.

What ownership means in practice: the login that deploys, the address that gets the alerts, and the person who notices when it breaks. [mooncloud/README.md](https://github.com/MoonModules/projectMM/blob/main/mooncloud/README.md) is the sequence, and three of its steps need that account rather than any automation: `wrangler login`, the Cloudflare account itself, and the decision to flip the firmware default.

**The firmware default is the last step and the one with no recovery.** `MoonCloudModule.h` ships pointing at the `workers.dev` address. A Custom Domain was tried and reverted: Cloudflare issued its certificate from Google Trust Services, which is not in IDF's default root bundle, so every ESP32 handshake failed while desktop's system trust store accepted it. The address is compiled in and hidden, so a prettier one buys nothing a device can use. Changed before an address answers, every device in the wild sends one report into nothing and never retries, because a failed report is deliberately not queued.
