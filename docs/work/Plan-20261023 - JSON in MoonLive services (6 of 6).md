# Plan: JSON in MoonLive services

Plan of 2026-10-23, a Feature, for approval. A service script fetches JSON from the internet (weather, sports, stocks, news), reads values out of it, and sends JSON to a device, itself included, which is how a script builds its own effects. hpwit's show and the StadBeest autopilot create their layers instead of assuming them, the autopilot on the eyes and the mouth as well, and `jsondemo.mls` shows four live data feeds as scrolling text.

## What exists

- **JSON in core:** `JsonUtil.h` parses into a node tree (`parse`, `member`, `element`, `readInt`, `readString`). Numbers are integers only (`JsonType::Int`, a `long`), so `12.3` cannot be read as it is. `JsonSink` writes JSON.
- **Applying JSON to the device:** `applyStateDocument` applies a state document in-process, the same engine `PATCH /api/state` and the presets run. A document's changes are saved; only a service's control writes are live (the Scheduler's live-write scope), so neither the API nor `StateSource` has a live document yet.
- **HTTP in the platform layer:** `httpRequest` sends cleartext HTTP to a LAN host and returns the body; `httpsPost` posts over HTTPS and returns only the status. ESP32 has `esp_http_client` with the certificate bundle (used by the OTA); the desktop has libcurl or WinHTTP. **There is no HTTPS GET that returns a body**, and every public data API needs one.
- **MoonLive strings:** a `string` is only one of the script's own literals, passed to a builtin; "building, joining or comparing text is out of scope". The script's literals share **128 bytes** (`CompileResult::kStringPool`), and four URLs alone run past that.
- **Services** run `tick20ms` on the render loop and write only control-surface slots (`setControl("fader2", v)`), as integers.
- **TextEffect** shows a `text` control (a text area), reachable from a state document.

## The rule this plan follows

The architecture says a script's vocabulary follows the compiled one: every builtin is a thin wrapper around a function compiled code can call. The request puts the JSON functions in the power functions library. That library is the light domain's drawing and math toolbox, and JSON and HTTP are domain-neutral. So the compiled home is core (`core/util` and a core service), as CLAUDE.md asks of a hard construct, and power-functions.md links to it. A compiled module gets every capability a script gets.

## Design

### The `json` type is a handle

MoonLive has no runtime strings, so a JSON document lives on the host and a script holds a **handle** to it: a 4-byte slot like `int`, typed so it cannot be used in arithmetic. Text never passes through the script: it moves from document to document on the host (`jsonCopy`), and a number moves as an `int` or `fixed`.

### Paths are JSON Pointers (RFC 6901)

`"/current/temperature_2m"`, `"/results/0/title"`: the request's own `/x/y/z/temp`, and the textbook. Setting a value is JSON Patch's `replace` (RFC 6902) at a pointer.

### Fetching never blocks the render loop

A DNS lookup and a TLS handshake take seconds on an ESP32. So `getJson` registers a source and answers a handle at once, and a worker task, unpinned at the lowest priority, fetches it and refreshes it every `seconds`. The script reads the last good document (stale-while-revalidate), and `jsonChanged` says a new one arrived. `pushJson` queues likewise. Pushing to the device itself applies the document at the next frame boundary through `applyStateDocument`, as a **live** source, so a show's writes are never saved, the rule the scoped-prepare plan set. Pushing to another MoonLight device sends `PATCH /api/state?live=1`, which applies the document the same way there; any other REST endpoint gets the document as it is.

A script that builds its look this way is the look's one source: the layers it pushes are gone after a reboot, and the script pushes them again at start.

### The script vocabulary (proposal)

| Builtin | Does |
|---|---|
| `json d = R"({"Effects": {...}})";` | a document from a literal, parsed once at start: assigning a string to a `json` is the parse |
| `json d = getJson("https://…", seconds)` | a fetched document, refreshed in the background |
| `bool jsonChanged(d)` | a new fetch arrived since the last ask |
| `int jsonInt(d, "/ptr")`, `fixed jsonFixed(d, "/ptr")` | a number, 0 when absent |
| `jsonSetInt(d, "/ptr", v)`, `jsonSetFixed(d, "/ptr", v)` | write a number |
| `jsonCopy(dst, "/ptr", src, "/ptr")` | move a value, text included, from one document to another |
| `json r = pushJson(d)`, `json r = pushJson("host", d)` | apply to this device, or send to another one (its `/api/state`) or any REST endpoint by URL; `d` is a document or a literal, and `r` is the reply |

A JSON document is full of `"`, and a MoonLive literal today has no escapes and ends at the line. The literal gains C++'s raw form, `R"( … )"`: it spans lines and keeps quotes as they are, so a REST body pastes in unchanged. A script is then standalone: everything it sets up is in its own text.

### A missing catalog script is the UI's to fetch

A pushed document can name a catalog script the device does not hold yet, as a preset or a restored backup can. Today only the picker downloads one, in the browser (`mlDownloadScript`), so a push would leave the module empty. Instead the device reports the script as **missing** in its state, and any open UI downloads it with the same function and writes it to the device; the module starts when the file lands. That is a reconciliation loop: the state says what is wanted, the device says what is there, and a client closes the gap. It holds on every device, with HTTPS or without, and adds no download code to the firmware, as the firmware update already lets the browser carry the image for a device without TLS. Until a UI is open the module shows that it is missing. A script stays on the device once downloaded, so an installation that started once with a UI open runs offline.

The request's `weather.getValue("/x")` method syntax would need member calls on a value type in the compiler; the free functions above give the same reach without that, and the sugar can follow.

### The data sources (to verify in step 6: key, size, rate limit, HTTPS)

| Feed | Endpoint (no key unless noted) | Pointer |
|---|---|---|
| Weather | Open-Meteo `api.open-meteo.com/v1/forecast?latitude=40.79&longitude=-119.21&current=temperature_2m` | `/current/temperature_2m` |
| Stocks | Yahoo Finance's chart endpoint, unofficial: `query1.finance.yahoo.com/v8/finance/chart/ASML.AS?interval=1d&range=1d` | `/chart/result/0/meta/regularMarketPrice` |
| Sports | TheSportsDB test key `123`: `…/api/v1/json/123/eventslast.php?id=133604` | `/results/0/strEvent`, `/results/0/intHomeScore` |
| News | Spaceflight News `api.spaceflightnewsapi.net/v4/articles/?limit=1` | `/results/0/title` |

Each answer is a few KB or less; a feed that returns hundreds of KB (an ESPN scoreboard) is refused by the size cap rather than parsed.

## Extensions

**Core (compiled, the home):**
- `JsonUtil`: JSON Pointer get, `replace` at a pointer, copy between documents, decimals read as `fixed` (a number type the parser gains), and serializing a document back to text.
- `JsonFetch`, a core service with a worker task, unpinned at the lowest priority: a fixed table of sources (URL, period, last document, generation), a push queue, a response size cap (16 KB with PSRAM, 4 KB without), HTTPS on every ESP32 and the desktop, as OTA and MoonCloud use it today.
- `StateSource::Live`: a document applied under the live-write scope, never saved; `PATCH /api/state?live=1` selects it.

**Platform:** `httpsGet(url, out, cap, timeout)` returning status and body: `esp_http_client` with the certificate bundle on the ESP32, libcurl or WinHTTP on the desktop, unavailable on Teensy (it says so).

**MoonLive:** the `json` keyword as a typed handle (compiler only; the backends see an `int` slot), raw string literals in the lexer, the builtins above in the service table, and a string pool sized to each script's literals instead of the fixed 128 bytes every engine reserves, so a 3 KB template costs 3 KB in the one script that has it.

**UI:** a missing script in the state, from any module, is downloaded by `mlDownloadScript`, the picker's own path, which then has one caller more rather than a second copy.

**Scripts:**
- hpwit-show creates its six layers, windows and effects with one `pushJson` of a literal at start, then drives them as now.
- `stadbeest-autopilot.mls` creates the whole beast at start: on the legs, itself, the three looks (walk, orb, thrusters) and the stadbeest-fire palette; on each eye the frog eye, eye orb and Sauron; on the mouth its one effect. Each device's document is a literal in the script, sent to the eyes and mouth by name, such as `stadbeest-eye-left.local`.
- **One layer per look** on the legs and on each eye, instead of three effects in one layer switched by `enabled`. A scene change crossfades: the autopilot ramps the old look's layer opacity down and the new one's up, as hpwit-show alternates voronoi and mandala. A layer at opacity 0 is skipped, so outside a crossfade the beast renders one look, as now. `switch3` to `switch5` move from `<look>.enabled` to the look layers' opacity, so the eyes still follow over OSC; the StadBeest page's surface table changes with them.
- `jsondemo.mls` fetches the four feeds and pushes a TextEffect layer per feed, re-pushing the text when `jsonChanged`.

## Impact and challenges

- **Memory.** A TLS handshake needs about 40 KB of heap for as long as the fetch lasts, freed after (`MBEDTLS_INTERNAL_MEM_ALLOC`, a 16 KB receive buffer); the worker's stack, 6 to 8 KB, is internal. Today OTA and MoonCloud start a handshake without looking at the heap: a failed allocation fails the request cleanly, but a handshake that succeeds on a near-full heap holds 40 KB for seconds that WiFi and the web server then miss, rare today and every minute with four feeds. So the client setup that `httpsPost` and `httpsGet` share refuses to start while free internal heap is under the floor plus the handshake, and the caller sees a failure it already handles: a feed skips a round and says so, a MoonCloud post waits for the next. The worker fetches one source at a time. Documents live in PSRAM where there is PSRAM, capped per source.
- **Render path.** Nothing network-bound runs on it. A push to the device itself applies at the frame boundary; a new layer is a structural change, so the first push of a show rebuilds the tree once and its text updates afterwards prepare nothing.
- **Security.** A pushed document can reshape the whole device, as the API can: a script is trusted code, the same as today. API keys sit in plain text in `/moonlive`. Fetches go only where the script names.
- **Several devices.** A push to an eye that is off or rebooting fails; the autopilot pushes again when that device answers, so the beast assembles in any boot order.
- **No internet at the venue.** The feeds stop and missing scripts stay missing; a show whose scripts were downloaded before runs as it is.
- **Language.** A new type touches the compiler's type checking and the IR golden table, not the five backends. The string pool change touches every script's memory budget.
- **Testing.** The desktop gets a stand-in fetch (canned documents per URL) so every builtin is pinned without a network; the real feeds are verified on the bench.

## Estimate

| Step | Work |
|---|---|
| 1. JSON Pointer, replace, copy, decimals, serialize, in `JsonUtil`, with tests | 1 day |
| 2. `httpsGet` on ESP32 and desktop, with a desktop stand-in | 1 to 1.5 days |
| 3. `JsonFetch`: worker, sources, refresh, push queue; `StateSource::Live` and `?live=1` | 2 days |
| 4. MoonLive: the `json` handle type, raw string literals, the builtins, the per-script string pool; a missing script in the state, downloaded by the UI | 3 days |
| 5. hpwit-show and the StadBeest autopilot create their effects; `jsondemo.mls` | 2 days, plus bench time |
| 6. Verify the four feeds, document (moonlive.md, core services, power-functions link, the StadBeest page, a how-to), MoonWalk | 1 day |
| **Total** | **about 9.5 to 10 days** |

## Steps

1. ✅ Core JSON: pointer get and replace, copy, `fixed` numbers, serialize; unit tests per RFC 6901's own examples.
2. ✅ Platform `httpsGet` beside `httpsPost`, sharing its client setup, which gains the heap check both get; its desktop stand-in; a test against the stand-in, the real one on the bench.
3. ✅ `JsonFetch`: sources, refresh, size cap, push queue; `StateSource::Live` and `?live=1`; tests through the stand-in.
4. ✅ MoonLive: `json` keyword and type checks, raw string literals, the builtins, the per-script string pool; compile tests, IR golden table. A module naming a script the device lacks reports it missing in its state, and the UI downloads it; a UI test with the file absent.
5. ✅ hpwit-show builds its own layers, measured on shiffy and the Waveshare; the StadBeest autopilot builds the legs, eyes and mouth, a layer per look, and crossfades its scenes, measured on the beast (the frame time during a crossfade, when two looks render); `jsondemo.mls` with the four feeds and TextEffect.
6. ✅ Feeds verified, docs, MoonWalk process "Show live data from the internet".

## Decisions (recommended)

- **The compiled home in core** (`JsonUtil`, `JsonFetch`), which power-functions.md links to. JSON and HTTP serve every domain, and a compiled effect (a weather display) gets the same calls.
- **Free functions now** (`jsonInt(d, "/p")`). They are one builtin each; `d.getValue("/p")` is sugar the compiler can add later as a rewrite to the same call, once scripts show they want it.
- **Documents as raw literals in the script, with a per-script string pool.** A script that sets up its look is standalone, with no data file to ship beside it, and the pool sized to a script's literals costs no byte it does not use.
- **A missing script is downloaded by the UI, not the device.** One download path for the picker, a push, a preset and a backup, on every device, Teensy included.
- **HTTPS on every ESP32**, as OTA and MoonCloud use it today: a standalone device shows internet data with no other machine running. An HTTPS request, MoonCloud's included, waits for heap rather than taking the internal RAM floor.
- **Every push is live, to the device itself and to another one.** The script is the look's one source and builds it again at start, so a show writes no flash and a reboot returns the device to its saved state.

## What it removes

Nothing in code: it adds a type, a service, a state source and a platform call. hpwit-show no longer needs its layers set up by a scenario first, so `scenario_MoonLive_hpwit_show`'s layer steps shrink to staging the scripts. The StadBeest page's per-device setup by hand becomes "install the autopilot on the legs".

## Where it stands (2026-10-24)

Steps 1 to 6 are built, desktop-tested and run on shiffy, the Waveshare and the StadBeest: the autopilot on the legs builds the legs, both eyes and the mouth's two bulbs (`MM-mouth`, `MM-mouth2`), and the eyes and the mouth follow it over OSC.

The StadBeest's frame time, the median of one-second averages over four minutes of 20-second scenes on 2026-10-24:

| Look (legs / eyes) | Legs | Eye1 | Eye2 |
|---|---|---|---|
| walk / frog eye | 10.1 ms | 8.6 ms | 8.4 ms |
| orb / eye orb | 15.4 ms | 10.1 ms | 11.3 ms |
| thrusters / Sauron | 23.9 ms | 20.6 ms | 20.1 ms |
| a crossfade, two looks rendering | 30.9 ms | 23.7 ms | 26.1 ms |

A crossfade costs about the heavier look plus the lighter one, and the slowest, thrusters into walk, holds 32 fps on the legs. One second during a crossfade averaged 293 ms on eye1 and 48 ms on eye2, a stall in that second.

What the build changed from the plan:
- **The rule for a busy target.** A second push to a target with one on its way waits and replaces any that waited before it, so no update is lost; dropping it lost text updates.
- **Text and numbers from services.** Many services quote their numbers (`"price":"252.11"`), so `jsonInt` and `jsonFixed` read a quoted number, and a script joins text with `jsonAppendText` and `jsonAppend`. `\uXXXX` escapes decode to UTF-8. Every call a script has: [the MoonLive language](../moonmodules/light/moonlive.md#json-documents-in-a-service).
- **Certificates.** Every Google Trust Services site (Coinbase, TheSportsDB, Spaceflight News) failed on the ESP32 with "No matching trusted root certificate found": it serves GTS Root R4 cross-signed by GlobalSign. `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY` fixes it, for every board.
- **`.local` names.** `esp_http_client` resolves through DNS alone, so the ESP32's `httpFetch` looks a `.local` host up over mDNS first.
- **TheSportsDB's test key** is `123`; `3` is retired.
- **The StadBeest's looks** fade through `encoder6` to `encoder8`, since a switch writes true or false and an opacity needs 0 to 255; `switch3` to `switch5` are freed.
- **`httpsPost`** is gone: MoonCloud posts through `JsonFetch` on the worker, over `httpFetch`, the one client every request shares, so the heap check covers it too.
- **HTTPS needs PSRAM.** On a classic ESP32 without PSRAM (`MM-Olimex`, 2026-10-24) the request waits for 145 KB of internal RAM, the 100 KB floor plus a 45 KB TLS session, where the board has 55 KB free at idle and 19 KB with `jsondemo.mls`'s four text layers; the bands kept their placeholders. Such a board gets its data pushed from one with PSRAM ([the MoonLive language](../moonmodules/light/moonlive.md#json-documents-in-a-service)); smaller TLS sessions are in the backlog. A plain-HTTP feed runs there: `weather.mls`, Open-Meteo over `http://`, showed "Amsterdam 12.2 C" on `MM-Olimex` with 60 KB still free. Of the demo's other feeds only Open-Meteo answers without TLS; the rest redirect to HTTPS, which a fetch refuses.
- **The share price** is ASML's in Amsterdam, from Yahoo Finance's chart endpoint, keyless and unofficial, where the demo first read bitcoin's.

Seen on hardware (`MM-Olimex`, 2026-10-24): a saved service naming `chase.mls`, which the board lacked, listed it as missing; an open page downloaded it into `/.moonlive`, and after a reboot with no page open the service compiled it from there.
