# Plan: the network map, groups and supersync

Devices shows what is on the network, lets devices form groups, and lets a group act as one: the same controls and the same clock, up to one effect spread over many boards (supersync), and finally groups across the internet (MoonOrbit). Each step ships on its own, and the map came first, so every later step is visible the moment it lands.

The plan has two parts. **The steps** lead to supersync, numbered 1 to 16. **Beside the steps** are the fleet's other work the map made possible: updating every kind of device, moving other firmwares to MoonLight and back, and the map's own reach. The design the steps rest on follows them, as reference.

## Where it stands

| Phase | Steps | Status | What it gives | Its demo |
|--|--|--|--|--|
| **See the network** | 1 to 5 | ✅ | a map of every device, what each offers and sends, its health, and a blink to find it | the bench drawn as one map |
| **Act as a group** | 6 to 9 | 6 to 8 ✅, 9 🧪 | groups, shared controls and presets, group updates, one clock | a line sweeping straight across boards |
| **One effect across the group** | 10 to 14 | ⏳ | supersync, from streaming to every board computing its own part | a ball crossing screens; Pong with two gamepads |
| **Beyond the LAN** | 15 and 16 | ⏳ | the same groups across the internet, and help for another user by looking at their device | a Pong rally between two homes; a user's setup replicated on a helper's desktop |

✅ done · 🧪 built, its test open · 🚧 in progress · ⏳ not started

## The steps toward supersync

### See the network

#### 1. Devices on top, drawn as a map ✅

Shipped: the [Discovery card](../moonmodules/core/devices.md#discovery), its map, growth and what memory refuses; the allocation rule in [moonmodule.md](../explanation/architecture/moonmodule.md#lifecycle-propagation-to-children).

#### 2. This device's flows and services ✅ (OSC, MQTT and Hue on the P4, PanelCard's ColorLight service on the NanoPi, 2026-11-17)

Shipped: the map's edges, each module's own flows and services, on the [Discovery card](../moonmodules/core/devices.md#discovery).

#### 3. Peers' flows: the whole MoonLight map ✅ (bench 2026-11-19: a flow between two other devices on a third board's map, drawn broken when its receiver stopped)

Shipped: the peers' maps merged, broken links and unreachable peers, on the [Discovery card](../moonmodules/core/devices.md#discovery).

#### 4. A peer's summary, and identify ✅

Shipped: a node's summary and identify on the [Discovery card](../moonmodules/core/devices.md#discovery); the summary's fields under `/api/system` in [the REST API](../reference/openapi.yaml).

#### 5. What drives each device's lights ✅

Shipped: the `outputs` of `/api/system` in [the REST API](../reference/openapi.yaml), shown on each node of the [Discovery card](../moonmodules/core/devices.md#discovery).

### Act as a group

#### 6. Device groups, shared by their members ✅ (bench 2026-11-19: a five-member group edited from a board, a member missing in MoonBase)

Shipped: the [Device group card](../moonmodules/core/devices.md#devicegroup), and its replication in [mooncore.md](../explanation/architecture/mooncore.md#multi-device-runtime).

#### 7. A group acts as one ✅ (bench 2026-11-19: five members, a change everywhere within 3 s, simultaneous changes converging, a restarted member catching up)

Shipped: shared controls in [mooncore.md](../explanation/architecture/mooncore.md#multi-device-runtime) and on the [Device group card](../moonmodules/core/devices.md#devicegroup); a group's name in `receivers` in [How feedback works](../reference/control-surface.md#how-feedback-works).

#### 8. Update a group ✅ (bench 2026-11-19: three members behind, updated in waves in two minutes)

Shipped: Updating the members, the waves and the skipped, on the [Device group card](../moonmodules/core/devices.md#devicegroup); one node's update on the [Discovery card](../moonmodules/core/devices.md#discovery).

#### 9. Clock sync 🧪 (built 2026-11-19: the fleet clock, apply at T, effects on fleet time; test: the spread under 1 ms and the line sweep across boards)

- Absorbs the backlog card, with two changes: the reference is the clock reference role, not a configured leader; and a member measures its offset by the NTP exchange (four timestamps, request and reply), not a one-way send, since the round trip is what measures the delay. The same exchange then works over any transport, the LAN and step 15's relay alike. Aim: under a millisecond on the LAN.
- **No NTP module** (product owner, 2026-11-19): NTP proper is wall time from an internet server, about 10 ms; a group needs its members to agree with each other, whatever the internet does. Only NTP's on-wire exchange is taken (RFC 5905 § 8).
- **One fleet clock** (product owner, 2026-11-19): every MoonLight device on the network shares one time base, the reference the lowest live device id Discovery hears, so a device in two groups has one clock. Each device asks the reference once a second until synced, then every 16 s, as NTP's poll interval backs off.
- **Where it lives.** The math in `core/util/ClockSync.h`, pure and host-tested: offset and delay from t1 to t4, NTP's clock filter (the last eight samples, the one with the least delay trusted), a small correction slewed and a large one stepped, so the clock never runs backwards under an effect. The exchange on its own socket and a small receive task, as `/moonlight/clock` by unicast to the reference: a timestamp taken at a 20 ms read would be up to 20 ms late, so t2 and t4 are taken as the task wakes on the datagram, and t3 just before the reply goes out. One reading, `FleetTime::nowUs()`, for the hybrid clock's physical part, step 11's `elapsed()` and "apply at T". The group's status names the reference and the spread.
- **Apply at T, a fixed lead first** (product owner, 2026-11-19): a shared control or look goes out in an OSC bundle whose timetag is 100 ms ahead; a member holds it until then, and one not synced applies it on arrival. The lead is derived from the measured round trips once the bench shows WiFi's.
- **A change applies at a time, not on arrival**: a preset or control change is stamped "apply at T", a little in the future, in an OSC bundle's timetag, which OscPacket now accepts. Members that receive it at different moments switch together, which is what lets latency, and step 15's internet, stay invisible.
- With step 7's shared controls, a group now shares both its controls and its time: the base every level of the next phase builds on.
- **If the exchange is not precise enough over WiFi: the access point's own clock.** Every 802.11 access point broadcasts its 64-bit TSF timer (Timing Synchronization Function) in its beacons, and every station on it keeps its copy aligned to within microseconds; the ESP32 reads it with `esp_wifi_get_tsf_time()`. It covers the WiFi devices on one access point, not Ethernet boards, desktops, or devices on other access points or mesh nodes, which keep the exchange. The textbook combination keeps the fleet clock as the one time base and adds TSF as a sharper source in `ClockSync.h` where it applies: the exchange measures the offset between TSF time and the reference, and TSF then holds the WiFi members together far more tightly than WiFi round trips allow. Anjin's "Wifi comes with gifts" (Discord, 2026-11-19) may point here; his weekend work on time sync is to ask about before building it.
- **Measured by:** the offset spread across a bench group; stop rule named before the first run.
- **Built (2026-11-19).** The arithmetic in [ClockSync.h](../../src/core/util/ClockSync.h) and fleet time for every task (`FleetTime`, two published slots, so a reader preempting the writer never waits); the service in [FleetClock.h](../../src/core/system/FleetClock.h), held by the Devices container beside the hybrid clock, its beacon on 239.255.77.80 and its exchange on port 65511, answered to the asking port; the hybrid clock's physical part and effects' `elapsed()` on fleet time, so `LinesEffect` is the line sweep; shared controls and looks sent as OSC bundles applying 100 ms ahead, held by a member on the fleet clock (`DeviceGroupSync`). The measurement is `moondeck/diag/measure_fleet_clock.py`. One open point: the device where a change is made applies it at once, so it leads the members by the lead time.
- **Tested (2026-11-19), by the agent.** The desktop, .122 (classic) and .115 (C3) on one reference, MM-dn2-2, within seconds of booting. First measurement over WiFi, 60 exchanges each: a spread of 5.1 ms with ±4.1 ms measurement error, the boards' best round trips 4 to 8 ms, so neither the sync nor the measurement resolves below a few milliseconds there. Next: TSF; the boards' WiFi power save is already off (`WIFI_PS_NONE` at every station start, [platform_esp32.cpp](../../src/platform/esp32/platform_esp32.cpp)).
- **The line sweep, on the product owner's eyes (2026-11-20).** All nine 3x3 boards on a6f18d2e's build with the review fixes, LinesEffect on axis x: the nine lines move equally. `measure_fleet_clock.py` over 60 rounds: every board answering, the estimates within 1.9 ms of SS-0-0, each bound ±6.5 ms from the Mac's WiFi round trips, so the 1 ms aim stays unshown by measurement.
- **Verified by:** a vertical line sweeping across boards side by side, the textbook display-sync test (genlock checks on video walls): straight across a seam when the clocks agree, a step when they do not, the step's width over the line's speed being the clock error.
- **The bench for it (product owner, 2026-11-19):** nine ESP32 boards on WiFi, each driving one 16x16 panel, laid out 3x3. LinesEffect, axis x, on every board: each sweeps its own panel on fleet time, so in step the nine lines move together and a board behind shows its line trailing. One line across the whole 3x3 is step 11's group canvas.
- **Notes from the 3x3 bench (2026-11-19).** All nine moved from WLED-MM 0.14 over the network and run one build in group SuperSync, on SS-0-0's clock within ±4.5 ms; on/off and brightness reach every member.
    - *Frame-rate independent is not yet in step.* A stateful effect that scales its steps by the frame time runs at one speed at any frame rate, but boards at different rates sample different moments, so their pictures drift apart. In step across boards means a frame that is a function of fleet time (level 1), randomness hashed from the group tick (step 12), or a fixed timestep on fleet time (step 13).
    - *Layouts belong to the group as one canvas, not as copies.* The shared part is one layout of the whole wall, nine 16x16 panels each a contiguous run; each member's driver window (start, count) picks its tile, which is step 11's region. Until then each board's own Grid is set by hand (serpentine).
    - *A preset recall trailed, and a pull does not scale.* Each member fetched the preset from the maker over HTTP at the same moment, a thundering herd on one ESP32: fetches hung past ESP-IDF's per-operation timeout, and a hung one blocked that board's every later recall. A spread, a retry and a deadline made nine boards follow, but the maker still answers N requests, and a look lands after T. Replaced by the push below.
- **A look is pushed to the group, one mechanism for every size (product owner, 2026-11-19).** Trickle-based object dissemination, Deluge's design (Hui and Culler, 2004), NORM's NACK repair (RFC 5740), on the group's own multicast address:
    - *The maker pushes.* A look applied here becomes an object, its name, stamp, size and digest, in numbered chunks of 1 KB with the fleet time it applies at; the maker multicasts every chunk, a few per 20 ms read. A small preset is one chunk.
    - *Members advertise what they hold.* The Trickle repeats carry each member's look stamp, size, digest and the chunks it has, the anti-entropy the values already use.
    - *A missing chunk is asked for.* A member short of chunks multicasts the ones it lacks (a NACK); any member holding them answers at its own offset, and one that hears them sent first stays quiet.
    - *Applied at T.* Complete and its digest matching, the look applies at its fleet time, so every member switches together as a shared value does; one that completes later applies on completion.
    - *Guaranteed:* every member that is up ends with the whole look, and one that was off catches up from any peer when it returns.
    - *Subtraction:* the HTTP pull goes, its fetch, JsonFetch's use here, and the spread, retry and deadline.
    - **Verified by:** unit tests for one chunk, many chunks, a lost chunk repaired, a late member, a digest mismatch refused; the 3x3 bench switching presets on every panel at once.
    - *Any file, not only a look.* The object is a document of up to 32 KB, so the same push carries a MoonLive script to every member: the object names a file to write rather than a document to apply, and the members' modules running that script pick it up as a file change does. Next, after the look.
- **Four kinds of group: two choices, independent (product owner, 2026-11-19).** Whether the members' layouts are identical, and whether each shows the whole picture (mirrored) or its part of one picture (canvas).

    | | Mirrored: each member shows the whole picture | Canvas: each member shows its part of one picture |
    |--|--|--|
    | **Identical layouts** | the 3x3 bench today: the fleet clock, the shared controls and the look | **the 3x3 wall's goal**: one picture over nine equal tiles, each member's region a cell of a regular grid |
    | **Different layouts** | each member draws the whole effect at its own size and shape | regions of any size and position in one canvas, gaps included (step 11's ball crossing the screens) |

    Every kind needs the fleet clock and, for stateful effects, levels 3 to 5. Canvas adds the shared canvas and each member's region and window; identical tiles make that the regular case, the first canvas to build.
    - *A group needs one firmware generation.* Release boards ignore the timed bundles, so a mixed group follows only on its newer members (MIGRATING, timed group changes).

### One effect across the group

#### 10. One effect streamed: correct for every effect ⏳

- **Streaming is not SuperSync** (product owner, 2026-11-19): SuperSync is every device computing its own part of the whole effect, some only their own 16x16, some up to the whole canvas. Streaming stays the fallback for a member that cannot compute its part, beside baking: an effect recorded once for the whole canvas and played by every member as FSEQ on fleet time, which the FSEQ player (PR #148) already reads.
- The member holding the render role renders the whole group canvas and sends each member its region over DDP; the group sets up NetworkSendDriver and the members' Network Receive. Every level is correct this way.
- Bounded by bandwidth and the LAN: a large canvas or MoonOrbit needs the levels of the next steps.
- **Verified by:** one effect running across three bench boards as one picture.

#### 11. Group canvas: position, time and shared input (levels 1 and 2) ⏳

- **Each device keeps its own layout and places it in the wall** (product owner, 2026-11-20). The position is a fact of the installation, so it lives on the device's Layouts (`positionX`, `positionY`, `positionZ`), one home whatever groups the device is in, the scene graph's world placement; its region is its own layout placed there, and a group's canvas is the bounding box of its members' regions, gaps included.
- **Each effect declares its footprint** (product owner, 2026-11-20), the area it computes: mirrored (the default, its own lights as their own picture, as today), region (its own lights at their place in the wall: shaders, Lines, AudioSpectrum's bands, randomness hashed from position), halo r (its region and an r-wide ring), whole (the whole canvas, showing its own tile: Game of Life, Fire, particles). The effect draws in wall coordinates and stays unaware of the group; the core sizes its surface from the footprint. SuperSync is an expert setting (product owner, 2026-11-20).
- **Built in small steps:** 1a the footprint and Lines as region (host); 1b the position and a typed wall size (`wallWidth`, `wallHeight`) on the Layouts, tested on the 3x3 bench; 1c Truchet as region; 2 the group deriving the wall size from its members' positions, the typed wall size removed.
- **Step 2 built (2026-11-20).** Each device with `superSync` on (Layouts, expert) sends its place, MAC, corner and own size, on the groups' OSC channel (`/moonlight/devicegroup/place`) at once on a change and every 5 s; a device keeps its group members' places for a minute unheard and derives the wall as the bounding box (`WallPlace`, on Devices). The typed wall size is gone. The presence packet stays as it is, since peers accept only its two lengths.
- **The beat phase on fleet time** (product owner, 2026-11-20): `BeatPhase` is fleet time times the rate, as WLED's `strip.now * speed`, so a board restarting mid-show is in step from its first frame and a speed change jumps the phase once; all 25 BeatPhase effects follow, 18 goldens re-blessed.
- **One mechanism for effect builders** (product owner, 2026-11-20): an effect draws in wall coordinates, loops over its lights with `eachLight` or draws shapes at wall positions, and declares how much of the wall it needs; outside a wall the wall is its own lights, so the code reads as a plain effect. The translation is per loop or per shape, never per light, so a device in no wall pays nothing measurable. In steps, measured alone on the layer on the desktop at 128x128 and the Waveshare S3 at 64x64: each new mechanism (`eachLight`, the wall-coordinate shapes, the larger canvas) as an A/B on one boot, the old and new code side by side in a temporary build, and each batch of moved effects in one sweep against the baselines taken 2026-11-20, an A/B following only a number beyond the noise, which on an S3 is about 2% on one boot and up to 15% between boots (product owner, 2026-11-20); a row computed once per row in a hand-written loop and once per light in the lambda, should the compiler leave it there, gets an optional row callback in `eachLight`:
    1. `draw::eachLight(cv, fn)`: the 3D loop over this device's lights, `fn(p)` returning the color at wall position `p`; `shader::each` is its 2D case, Plasma moved onto it, Truchet and Raymarch through `shader::each`. Built (2026-11-20): its writes go through a restrict pointer, since a byte store may alias anything and the compiler then reloads a lambda's captures per light; measured on the Waveshare S3 at 64x64 on one boot, Plasma 2,550 µs on its hand-written loop, 2,880 on `eachLight` plain, 2,470 with the restrict; Truchet and Raymarch within 1%; on the desktop at 128x128 all within its noise.
    2. Draw calls take the device's own grid, and a region effect converts its wall positions once each with `cv.local(p)` (product owner, 2026-11-20), as a pipeline applies its viewport transform per vertex and rasterizes in device coordinates. The A/B on the S3 decided it: the translation inside `draw::pixel` doubled a loop-drawn effect's cost in every variant tried, the compiler losing its proof that a loop counter stays in range. Built 2026-11-20 with Lines, Lissajous, Ripples and BouncingBalls (in closed form: kick and phase hashed by column, the cycle a geometric series inverted with one logarithm), Lissajous and BouncingBalls on the product owner's eyes across the 3x3 wall. The batch's A/B on one S3 boot against the committed versions: Ripples equal, SphereMove 2.5% faster, Lines 4%, Lissajous 6% and BouncingBalls 10% slower, the work of being wall-aware (each point converted, the sweep in wall units, the cycle state), accepted by the product owner; BouncingBalls' first closed form was 2.15 times slower until its kick, span, cycle and phase were cached and its bounce stepped on from the last frame.
    3. Halo and Whole, one construct: core gives the effect a larger canvas and copies its own window back, its box plus an r-wide ring for Halo (the ghost cells of halo exchange, computed rather than sent), the wall for Whole. A halo effect returns its radius, the reach of its neighbor reads, from its controls where they set it (Bloom's is its blur), and core sizes the canvas from it at each prepare, so a control change applies on the next frame (product owner, 2026-11-20). The showcases: Bloom, a new effect (sharp dots on closed-form orbits in fleet time, the frame cleared and blurred once with radius r, its halo the blur radius), for Halo; Waterfall for Whole (rows on fleet time from the shared audio frame scrolling down through the boards below, a late board whole again after one wall height of rows); Game of Life after them (respawns hashed from the generation, generations counted on fleet time, restarts at fixed epochs for a late board). Then the rest move one at a time. The default footprint stays mirrored (product owner, 2026-11-20): an effect tiles the wall once it passes the split-equals-whole test and carries 🧩, and every other effect shows its whole picture on each device. SphereMove on `eachLight`, its jitter a hash of position and a 20 ms tick, built 2026-11-20 and on the product owner's eyes across the 3x3 wall: "works great"; BouncingBalls in closed form first (bounces from fleet time, relaunches hashed), as the demo ball below.
- **Region effects:** Lines, Truchet, Plasma and Raymarch, each tagged 🧩, pinned by the split-equals-whole test and by a test that every effect carries the tag exactly when it declares region.
- **1a to 1c on the product owner's eyes (2026-11-20).** The nine boards at SS-r-c = (16c, 16r) in a 48x48 wall: one red line crossing the columns and one green line the rows, once each board's Grid became a Panel layout wired columns first (YX, serpentine), the panels' own orientation; then Truchet as one tiling over the wall, its arcs meeting across every seam: "perfect". An extra device is extra wall, and each device computes only its own lights. The textbook: a multi-monitor desktop, each monitor its own resolution at a position in one virtual desktop, and sort-first parallel rendering (Molnar et al., 1994), each node rendering the global scene through its own viewport. The arrangement belongs to the group, as a monitor arrangement belongs to the desktop. Open: the canvas as the bounding box, which rescales every effect when a member joins, or a size declared once, which keeps the picture's scale; positions typed in the member list first, dragged on the map later, as display settings drag monitors.
- **The 3x3 bench's first slice:** SS-r-c at x = 16c, y = 16r; Truchet (its arcs meeting across a seam prove the tiles line up) and LinesEffect with axis all (one red and one green line over the whole wall) reading canvas coordinates.
- The group has one virtual size and each member holds its region in it; `elapsed()` reads the synced clock; `BeatPhase` is anchored to the effect's synced start, its rate changes applied at their synced time; `uv` takes the canvas. Every member renders its own region, and no pixels travel.
- A member lacking the effect (Raymarch on a chip without an FPU) gets it streamed, as step 10 does.
- The level declaration on EffectBase, the group's choice from it, and the split-equals-whole test; level 1 and 2 effects declared, each passing it.
- **The demo effect: a ball crossing the screens.** An elastic ball in closed form, x a triangle wave over the canvas width, y a parabola repeating each bounce, both functions of synced time: level 1, no state, each board drawing it while it is in its region. Regions placed at the boards' real positions, gaps included, so the ball takes the right time to fly between screens. BouncingBalls loses energy and relaunches, which carries memory; this is its elastic, sideways twin.
- **Verified by:** the ball crossing three bench boards with no jump at a seam; Plasma, Truchet and AudioSpectrum across them as one picture, the spectrum's bands split over the boards; a speed change mid-run keeping the boards in phase; a board started a minute late in phase from its first frame.

#### 12. Randomness from the group tick (level 3) ⏳

- A group tick from the synced clock; effects drawing per-frame randomness move from a `Random8` stream to `hashInt(seed, tick, light)`.
- **Verified by:** the split-equals-whole test for each converted effect; a sparkle effect on boards at different frame rates showing the same sparkles.

#### 13. Lockstep simulation (levels 4 and 5) ⏳

- A fixed timestep from the synced clock, in core, which a simulating effect steps by; integer math; the whole canvas simulated on each device.
- An effect whose memory forgets (Fire's cooling, a fade trail) converges by itself and needs nothing more. One whose memory keeps, and every level 5 one, publishes a state checksum every N steps as group state; a mismatch is repaired by a keyframe from the render role holder, and a late joiner joins by keyframe.
- Fire (forgets), Particles (keeps) and Game of Life (chaos) first, one of each case, the rest by the test.
- **Measured by:** how often a bench group diverges over an hour, and the keyframe's cost; stop rule named before the first run.
- **Verified by:** Game of Life across three boards as one board for an hour, a board rebooted and rejoining by keyframe.

#### 14. Live input across the group (level 6) ⏳

- A gamepad or touch on one member becomes group state, stamped "apply at T" a little ahead, so every member applies it at the same step: the delay-based lockstep of networked games. Where the delay shows, the effect stays streamed.
- **Pong across two boards**, a paddle on each and the ball crossing between them. Its attract mode re-rolls each paddle's aiming error per exchange; drawn as a hash of the exchange number, the rally is deterministic (levels 1 and 3), and a gamepad per board makes it level 6.
- **Verified by:** Pong played with a gamepad on each of two boards, the bench's Xbox Series pad and 8BitDo Zero 2, each paired with the computer showing its board's page ([the gamepad plan](Plan-20261001%20-%20Gamepads%20and%20MIDI%20desks%20(2%20of%207).md) owns how a pad reaches a board); MoonInvaders from one board's gamepad across three.

### Beyond the LAN

#### 15. MoonOrbit: groups over the internet, through MoonCloud (stretch) ⏳

Devices in different places, owned by different people, in one group: a show running at several sites, or friends' installations running as one. MoonOrbit is a working name.

- **A relay, not a stream.** Only the clock and the changes travel: the controls, presets and "apply at T" stamps. Each device renders locally, so internet bandwidth stays small and latency hides behind step 9's scheduled changes. Pixels over the internet stay out of scope, so a group across sites runs effects at levels 1 to 6 and step 10's streaming stays within each site.
- **Outbound only.** A device behind a home router cannot accept a connection, so each member opens one outbound WebSocket to MoonCloud, and MoonCloud relays within the group. On Cloudflare that is one Durable Object per group, beside the existing Worker ([MoonCloud](../explanation/architecture/mooncloud.md)).
- **Opt-in and private**, as MoonCloud is today: a group is joined by a secret code the owner shares, the relay keeps no history, and the [privacy policy](../legal/privacy-policy.md) gains what the relay sees. Messages could be signed or encrypted with the group secret so the relay cannot read them; decided in the step.
- **Reach:** ESP32 boards and the desktop, which have TLS; a Teensy joins through an open page until it has TLS.
- **Measured by:** offset spread and change-to-light delay between two sites; stop rule named before the first run.
- **Verified by:** a bench board and a board at another address in lockstep, judged on a shared video call; a Pong rally between the two homes, one pad in each, paired with that home's own computer or phone.

#### 16. Remote support over MoonOrbit ⏳

A user with a problem shares their device, and a helper sees it and replicates the setup on their own desktop: "share your device in MoonOrbit and I will reproduce it". A support session is a group of two over step 15's relay, the helper reading the user's device as a page reads a peer on the LAN.

- **The user starts it.** A "share for support" button on the user's device shows a short code, valid for a limited time and ended by the user at any moment. The helper enters it in their own MoonLight, and the user's device appears on the helper's map.
- **Read-only by default.** The helper reads the summary, the state, the log and the flows through the relay's request/response. A change on the user's device needs the user to allow writing for that session.
- **Replicate on the desktop.** "Copy setup" loads the user's state into the helper's desktop instance, desktop first as every change is; a module needing hardware the desktop lacks (pins, a driver's peripheral) loads and idles, as it would on a board without that hardware. The copy is the same one the Backlog's "copy a device's settings" needs, so both share one export.
- **Secrets never travel.** Password controls (WiFi, MQTT, API keys) are left out of what is shared and copied; the [privacy policy](../legal/privacy-policy.md) names what a session shows.
- **Verified by:** a bench board at another address shared by code, its setup copied onto the desktop and showing the same effect, no password in the copy, and a write refused until the sharer allows it.

## Beside the steps

Each ships on its own, in any order.

### Updating every kind of device

#### Every desktop updates itself through the Firmware module ✅

Shipped: a host's self-update in [mooncore.md](../explanation/architecture/mooncore.md#platform-abstraction), the Linux service and its rollback in [the service](../reference/installing-on-linux.md#the-service), the app on Android in [Running MoonLight on Android](../reference/installing-on-android.md#what-the-app-does).

#### The map updates a desktop as it updates a board ✅

Shipped: [the service](../reference/installing-on-linux.md#the-service), updated from the map as a board is, and [Docker](../reference/installing-on-linux.md#docker), updated by its host; the `update` field of `/api/system` in [the REST API](../reference/openapi.yaml).

#### A container updated from the map, through Watchtower ⏳

Asked by the product owner (2026-11-12), after the desktops.

- **The container stays without the Docker socket**, so the device never holds root on its host. Watchtower holds it, and its HTTP API (`--http-api-update`, a token) starts an update of the containers it watches, the same on Linux, Docker Desktop for macOS and Windows, and a NAS.
- **The browser cannot call it, so the device does.** Watchtower's API takes its token only as an `Authorization: Bearer` header and sets no CORS header, so a page served by one device cannot post to it from another. The container's MoonLight calls it instead, as it fetches its own image: two Firmware controls, shown only in `host` mode, Watchtower's address and its token, a password control the API obfuscates; `/api/system` saying a host updater is set; and `POST /api/firmware/host-update`, beside the MoonBase and coprocessor routes, which answers 202 and posts from a background task to `/v1/update?image=ghcr.io/moonmodules/moonlight`, so only MoonLight's container updates. Watchtower blocks until it is done and stops this very container mid-request, so a cut connection is the update under way; a 401 or a refused connection goes on the Firmware status. The platform's `httpFetch` gains a header argument.
- **The map's part.** A container node whose summary names a host updater gets "Update to … through Watchtower", pressed twice like a board's, posting to the route and watching `/api/system` as the board path does, the watch loop shared rather than copied. Without a Watchtower set, the node says its host updates it, as now.
- **Adds** a route, two controls and a client argument; the token lives on the device alone, so the browser-side setting the first draft planned does not exist.
- **Podman's auto-update** has no API and stays the host's, and so does a Kubernetes rollout (belrik runs both, Discord 2026-11-13).
- **Verified by:** .156's container updated from another device's map through a Watchtower on the NanoPi, run with the Docker socket, `--http-api-update`, a token and its port mapped beside MoonLight's 8080; the container on `:latest`, since Watchtower follows the tag a container runs, an older image tagged `latest` locally so the test has somewhere to go.

#### MoonBase and the WiFi coprocessor updated from the map 🧪 (test: the coprocessor at the next ESP-Hosted bump, since a device refuses a radio image of another major.minor, so a C6 falls behind only when the app moves on; MoonBase verified on MM-BD3C, 2026-11-17)

Asked by the product owner (2026-11-12), after the desktops.

- **The routes exist.** `POST /api/firmware/moonbase-update-url` takes the release's MoonBase image, and `POST /api/firmware/coprocessor-update-url` with an empty URL takes the image matching the device's own release; the device fetches either itself, as it does the app.
- **What the map needs: when each one is due.** `/api/system` reports a MoonBase that does not match the app (the Firmware card's `moonbaseMismatch`) and an outdated coprocessor (`coprocessorMismatch`), each absent while current.
- **What the map offers.** A node's popup shows "Update MoonBase" and "Update WiFi coprocessor" beside the app's update, only when due, each pressed twice like the app's, posting to its route and watching `/api/system` until the peer reports it current. A board without TLS is sent to its own Firmware card, as for the app.
- **Verified by:** a bench board with an older MoonBase and the P4 with an older coprocessor image, each brought current from another device's map.

#### The map marks a device behind its channel ✅ (a bench board behind, on the host's map, 2026-11-17)

Shipped: a node's mark and its popup's update on the [Discovery card](../moonmodules/core/devices.md#discovery); the channel in [Update MoonLight](../moonwalk.md#update-firmware); `moonbaseMismatch` and `coprocessorMismatch` in [the REST API](../reference/openapi.yaml).

#### MoonBase on every device ⏳ (after WLED to MoonLight from the map)

- Every layout carries MoonBase in a factory slot: recovery without a cable on every board, one update path, and on an 8 or 16 MB board the second app slot's flash going to the filesystem. Today the two-slot variants are the larger S3s (N8R8, N16R8) and the P4.
- Waits for the migration step: its power-cut rounds on each chip family are what MoonBase on that chip is trusted on.
- A device already on a two-slot layout cannot change its table through a network update, so it moves with the same migration image, uploaded through MoonLight's own update page: the program that migrates a WLED device migrates a two-slot MoonLight one. A MIGRATING entry.
- **Verified by:** a bench S3 N16R8 and the P4 moved from two slots to MoonBase over the network, then updated through MoonBase and recovered from a cut mid-update.

### Other firmwares, to MoonLight and back

#### WLED to MoonLight from the map ✅

Shipped: [Migrating over the air](../reference/migrating-over-the-air.md) and the [Move a WLED device](../moonwalk.md#migrate-wled) process; a board in MoonBase on the map in [moonbase.md](../explanation/architecture/moonbase.md#on-the-devices-map).

#### ESPHome, Tasmota and others to MoonLight from the map ✅

Shipped: [What can move](../reference/migrating-over-the-air.md#what-can-move), and each firmware's guards the map keeps in [security](../explanation/security.md#the-devices-map).

- **Still open, each its own track:** ESPixelStick (its EFU container), OpenMQTTGateway (pulls the image through `/up?o=`), Shelly Group's relay firmware (its own ZIP package, its table at 0x10000).

#### MoonBase's page installs through the shared picker ✅

Shipped: [On the Devices map](../explanation/architecture/moonbase.md#on-the-devices-map), and the retry step of [Move a WLED device](../moonwalk.md#migrate-wled).

#### WLED, WLED-MM, Tasmota and ESPHome installed on a device 🚧 (built: the installer's section and the map's links, 2026-11-17; next: the way back through MoonBase for WLED, the MoonDeck action)

Asked by the product owner (2026-11-17): the way back from MoonLight, and a bench that can set up any firmware the map moves.

- **Over USB, or through MoonBase.** A device's own update route refuses an image that is not MoonLight's (`isAppImage`), and a board MoonLight moved carries MoonBase's partition table, which none of the four boots from, so no plain upload installs them.
- **Through MoonBase, asked by the product owner (2026-11-17): the migration image run backwards.** The migration image (`moonbase/migrate/`, 387 lines) already moves a board from WLED's, Tasmota's or ESPHome's layout to MoonBase's, power-cut safe: it copies itself out of the slot it overwrites, writes the bootloader and table it carries, and carries the networks over. The way back is the same steps toward the target's layout: the target's app downloaded from its own release into a slot MoonBase is not running from, the target's table (WLED's 4 and 16 MB tables are in `esp32/partitions/` already, built for the migration image) and its bootloader, then the choice of app.
    - *What it costs per firmware:* its table and bootloader per flash size, its release asset's name, and its networks written its own way (WLED's `wsec.json`, Tasmota's settings, ESPHome's preferences), then a power-cut round on each chip family, as the way in had.
    - *What it loses:* MoonBase itself, since none of the four has a factory slot, so the way back to MoonLight is the migration image again, through the old firmware's update page.
    - *Order:* WLED first, whose tables exist and whose settings the way in already reads; WLED-MM shares its layout; Tasmota's safeboot layout and ESPHome's later.
- **Built (2026-11-22): the installer writes other firmware itself, each from its own project's site.** [other-firmwares.json](../../mooninstaller/other-firmwares.json) lists them: Troy's WLED and ESPHome by the ESP Web Tools manifests on their own Pages, Tasmota by its factory image per chip, WLED and WLED-MM by their latest GitHub release with the boot files their own installers keep ([install.wled.me](https://install.wled.me/), [MoonModules' installer](https://moonmodules.org/WLED-WebInstaller/)), WLED's bootloader re-flagged for the board's flash size and the apps through WLED's CORS mirror. After a Detect the dropdown lists the builds for the board's chip, flash size and PSRAM, named with their versions; the install takes the connected chip's build and gives the WiFi over Improv where the firmware takes it. [other-firmwares.js](../../mooninstaller/other-firmwares.js) and [manifest.js](../../mooninstaller/manifest.js) hold the rules, which [install_other_firmware](../../moondeck/MoonDeck.md#install_other_firmware) runs for agents and the migration test, now on the latest WLED. The map's move confirm links the section.
- **Tested (2026-11-22):** WLED 16.0.1 on a classic ESP32 16 MB, an S3 N16R8 and a C3 4 MB; WLED-MM 14.7.1 on the S3, whose S3 builds speak Improv on UART0, so a board on its native USB opens WLED-AP (the reason asked of the WLED-MM team); Tasmota 16.0.0 and ESPHome 26.9.2 on the ESP32 with the WiFi over Improv, once the frames ended with the newline ESP Web Tools sends; Troy's WLED on the Olimex P4-PC; the migration test's seven rounds against WLED 16 on the ESP32. Open: an S3 with 8 MB flash, WLED-MM on the ESP32 and the C3, the browser's WiFi step for Tasmota and ESPHome, the test's S3 rounds. Tasmota's P4 builds (a choice by chip revision) and WLED's WROOM-2 build, which its own installer does not offer, stay out.

#### WLED's LED outputs and 2D layout carried into MoonLight ⏳

Asked by Anjin (Discord, 2026-11-11) for a WLED strip cut by segments into a 2D display; for the next branch.

- **In the browser, not on the device.** The map's move already reads WLED's `/json/cfg` (its Ethernet board) and `/json/info` (its chip) before the upload. It reads `/json/state` too and turns what maps into one state document, applied with the device defaults as one `PATCH /api/state` once MoonLight answers. No byte in the migration image or MoonBase; the translation loads from the installer's site as the picker does, or costs a few KB of `app.js`.
- **What maps:** WLED's LED outputs (`hw.led.ins`: GPIO, length, LED type, color order) become MoonLight's driver pins and a layout of that length; WLED's own 2D matrix (`hw.led.matrix`: panel size, serpentine, orientation) becomes a Grid layout.
- **What is offered, never applied unasked:** segments that chop one strip into equal ranges, which WLED knows only as start and stop, are suggested as a grid of that many rows, serpentine or not, for the user to confirm.
- **What stays behind:** effects, palettes and presets, which do not map one to one.
- **Verified by:** JS tests on captured `cfg.json` and `/json/state` from WLED 0.15 and 16; a bench WLED with a matrix moved and lit as before; Anjin's device.

#### WLED, ESPHome and Tasmota working with MoonLight ⏳

WLED is the obvious partner; ESPHome and Tasmota run on far more devices in a home, mostly sensors, buttons and relays rather than lights. What each can bring, and what MoonLight would use; a claim marked *to check* is confirmed in that project's source before it is built on.

| Role | WLED | ESPHome | Tasmota |
|--|--|--|--|
| **Shows MoonLight's pixels**: MoonLight renders, their LEDs show it, through NetworkSendDriver | receives DDP, E1.31 and Art-Net (realtime mode) | the `e131` light effect, and the `wled` effect that takes WLED's UDP realtime protocol | Art-Net on its addressable LEDs (*to check*) |
| **Sends its pixels to MoonLight**: their effect on MoonLight's lights, through Network Receive | a network LED output sending DDP, E1.31 or Art-Net | none | none |
| **Shares on, brightness, color**: one switch for every light | WLED's UDP sync (port 21324), which MoonLight could join for power and brightness | through MQTT or Home Assistant | through MQTT or Home Assistant |
| **Sends sound**: an audio source for audio-reactive effects | WLED's audio sync (port 11988), which MoonLight already receives | a microphone's level, shared over its UDP packet transport (*to check*) | none |
| **Gives inputs**: buttons, motion, light level, a remote | buttons and IR, as WLED sync or presets | any sensor or binary sensor, sent by its `http_request` action to MoonLight's REST API, or over MQTT | buttons, PIR, lux sensors, sent by a Rule's `WebSend` to MoonLight's REST API, or over MQTT |
| **Switches and measures power**: the LED supply off while MoonLight is off, its watts on the map | a relay pin | a smart plug or relay with an energy sensor | a smart plug or relay with an energy monitor, the commonest Tasmota device |
| **Found on the map** | ✅ presence packet and mDNS | ✅ mDNS | ✅ an added address |
| **In a group** (step 6 on) | a pixel receiver for a streamed effect (level 1), and a member of WLED sync | a pixel receiver, an input | a pixel receiver (*to check*), an input, the group's power |
| **Moved to MoonLight** | ✅ | ✅ | ✅ |

- **First candidates**, each small and on paths that exist: *send my lights to this WLED* from the map, a NetworkSendDriver aimed at the node; *switch the supply with MoonLight*, a Tasmota or ESPHome plug following MoonLight's on and off; *a sensor drives a control*, a button, motion or lux reading mapped to a MoonLight control or preset over MQTT or the REST API.
- **What stays theirs:** their own guards (Tasmota's Referer check, ESPHome's native API key), never bypassed; MoonLight talks to them through what each publishes, as the map already does.
- **Verified by:** each first candidate on the bench, a WLED, an ESPHome and a Tasmota board each doing its part beside a MoonLight board on the map.

### The map's reach

#### Every Hue bridge on the map ⏳

- Not only a bridge a HueDriver points at: Hue's own discovery, mDNS `_hue._tcp`, through a passive listen to mDNS announcements, or SSDP on port 1900. With the rest of the Backlog's More discovery, since they share the mechanism.
- It needs a hardware test: mDNS queries were ended by a pool exhaustion on the ESP32, so the passive listen is a hypothesis until a board runs it.
- **Verified by:** a Hue bridge nobody configured on the bench map.

#### Every neighbor's MAC, by ARP ✅ (Windows run 2026-11-17: MM-Windows at .245 gave the Hue bridge at .143, an added address, its MAC 00:17:88:41:67:f0)

Shipped: an added address's MAC from ARP on the [Discovery card](../moonmodules/core/devices.md#discovery), and ARP read from this device's own table in [security](../explanation/security.md#the-devices-map).

- **More uses**, each its own decision:
    - *A fallback for every row without a MAC*, not only added addresses: any device this one has talked to. Receiving a device's broadcasts alone does not always put it in the table.
    - *The vendor*: a MAC's first three bytes name its maker in the IEEE OUI registry, Espressif, Signify (Hue), Raspberry Pi, Apple, so a small table tells an ESP32 from a Pi or a bridge without asking it.
    - *Wake-on-LAN*, which needs the MAC: the map waking a sleeping PC or Pi that runs MoonLight.
    - *An active scan*: ask for every address of the subnet and list what answers, silent devices included, as `arp-scan` does; a burst of broadcast, so on demand.
    - *Changes spotted*: one MAC at a new address is a device that moved; a new MAC at a known address, another device that took it.

#### A security and privacy check of the map ✅

Shipped: [security](../explanation/security.md), the trust boundary, [the Devices map](../explanation/security.md#the-devices-map) and the review a feature beyond the network gets.

#### MoonDeck and the web installer reviewed against the map 🚧 (MoonDeck's half done 2026-11-18; next: the web installer's half, and the replug test)

Where MoonDeck, the web installer and the map each do one job, finding devices, device models, installing, board details and other firmwares, one of each pair stays and the other goes.

- **MoonDeck's half, decided by the product owner (2026-11-18), landed:** the map runs fleet operations and MoonDeck what needs the source tree. MoonDeck's Live tab draws the same map and popup (`src/ui/devicemap.js`), hears the devices itself on UDP 65506 (`moondeck/_presence.py`), adds its USB actions to the popup (`moondeck_ui/popup.js`), and finds a board's port by its MAC when used, never storing one (`moondeck/_usb.py`); its subnet scan, device list, deviceModel push and own update route are gone. How it works: [MoonDeck.md](../../moondeck/MoonDeck.md).
- **The web installer's half, open:** its device models applied over serial (`APPLY_OP`) against the map's Apply device defaults, and its device report against the popup.
- **Later, its own decision:** the map installing over USB through Web Serial, as the web installer does; browsers allow it on https and localhost only, so from the desktop app's page and not from a board's.
- **Verified by:** the list of overlaps found, each with its decision, and the code and docs that one of each pair leaves behind removed. For MoonDeck: its Live tab draws the same map as a board's page; with no MoonLight running it lists a board that announces itself and one on USB; a board replugged to another port is flashed by MAC without a manual pick.

## The design

The principles every step follows, the messages they send, and how an effect spreads over a group.

### The principles

- **One building block.** Devices is a top-level container like Services. Its children are MoonModules: the discovery list, and one module per group, added and removed the way an effect is.
- **The UI stays generic.** The map is a list drawn as a graph, the way a list can already be drawn as pads: a list source declares `listAsGraph()`, each row a node and an `edges` array on a row its adjacency list, the textbook graph representation. `app.js` draws it as SVG with a force-directed layout (Fruchterman-Reingold, hand-written, no library). The rows keep the list's streaming and live updates, the UI learns nothing about Devices, and any list can be drawn this way.
- **Listen always, ask on demand** (product owner, 2026-11-09). Listening costs only the packets that arrive anyway and stays on, since group state and a missing member depend on it. Asking costs traffic and work: ArtPoll and SSDP probes, the REST reads of peers for the map and the summaries. Those run only while something reads them, a Devices card open in a browser or a module that needs them, the pull model the preview and [Demand](../../src/core/module/Demand.h) follow. No scan button.
- **Each device reports only itself.** A board on a switched network sees broadcast, the multicast groups it joins and its own unicast, never traffic between two others. So every MoonLight device reports its own flows, and the page merges what its peers report into one map. A flow between two foreign devices stays unseen.
- **No upper limit**, the architecture's [dynamic over fixed-size](../explanation/architecture/moonmodule.md#lifecycle-propagation-to-children) rule. The device list, the flow table and a group's members grow on demand. They grow at a new sighting, a rare event and not a frame, by doubling, so N devices cost about log₂ N allocations, in PSRAM where the board has it. The hot path only iterates.
- **Graceful degradation when memory is short**, the [allocate-and-degrade rule](../explanation/architecture/moonlight.md#degradation-cascade) the light pipeline already follows. Growth is checked against `platform::HEAP_RESERVE` before allocating, so the fleet never takes memory the lights, WiFi or HTTP need. When growth is refused, it steps down, best to worst:
  1. **Keep what matters, drop the rest.** This device, group members and configured peers are pinned. Among the others, the device heard longest ago is replaced first (least recently used), foreign and unknown devices before MoonLight ones.
  2. **Drop the flows memory refuses.** A flow or service that does not fit is left out of the map that second, and the status says memory full.
  3. **Read, don't hold.** A device reading its group streams the members' summaries through without storing them.
  4. **Say so.** The card's status says "memory full", and the map draws what is held. A count of the devices heard but not held would itself take memory per device, so the status names the state rather than a number.

  When memory returns, the lists grow again by themselves. Never a crash, never the lights' memory.
- **Swarm scale.** Hundreds of devices on one network, so nothing costs O(N²). A device hears N presence packets per period; a change to a group goes out once by multicast, not as N unicasts; the page reads N peers with a bounded number of requests in flight; the map draws a large group as one node with its count, opened on a click.

### The messages

No new protocol (product owner, 2026-11-09): each kind of message already has its textbook one, and what is designed is the rules on top. The only new formats are documents: the summary and flows JSON, and the OSC addresses of group state.

| Kind | Loss | Protocol |
|--|--|--|
| **Streams**: light, audio, video | acceptable, latest wins | Art-Net, DDP, E1.31, the MoonLight audio sync format, each its own |
| **Group state**: preset, controls, clock, roles | repaired by the next send | OSC on the groups' one well-known multicast address: the definitions (step 6), the shared controls and looks (step 7) |
| **Requests**: summary, flows, identify, firmware | must arrive | REST over TCP, every peer answering cross-origin (`Access-Control-Allow-Origin: *`) |
| **Discovery** | acceptable | presence, ArtPoll, SSDP, mDNS |

- **Audio sync stays its own packet.** It is a stream at ~42 packets a second, WLED-compatible in one format; carrying must-arrive state in it would tie that state to a lossy stream.
- **State, not commands.** A group's state is published as absolute values ("preset X, apply at T"), on a change at once and then repeated, so a lost packet is repaired by the next repeat and a member that joins or reboots catches up. Applying a value twice changes nothing, so no acknowledgements are needed and a swarm costs one multicast send. Presence already works this way. REST stays for what is not state: an update, identify, reading a peer.
- **Apply at a time.** OSC bundles carry an NTP timetag, OSC 1.0's own "execute at T". [OscPacket.h](../../src/core/services/OscPacket.h) recognizes bundles and rejects them today, "a timed batch we have no use for yet"; step 9 gives them their use.
- **No races, no loops:**
  - **Leaderless: every member may write** (product owner, 2026-11-09). Each value is a last-writer-wins register (Shapiro et al., *Conflict-free Replicated Data Types*, 2011): it carries a timestamp and its writer's device id, a device applies only a newer one, and a tie goes to the higher id. Every member converges on the same value whatever order packets arrive in, so concurrent edits do not race: two people on one slider at once, one edit wins and all agree which. The timestamp is a hybrid logical clock (Kulkarni et al., 2014), which orders edits before step 9 syncs the clocks and by real time after. Duplicates, reordering and stale values are ignored for the same reason. Today's shared controls are peer-to-peer with an echo check ([OscService.h](../../src/core/services/OscService.h)); whether two boards changing one control at once can end apart is a hypothesis, which the registers settle either way.
  - **No forwarding.** A device publishes only what it wrote itself and never re-sends what it received, so no loop can form.
  - **Coalescing.** A slider drag publishes at most ~20 times a second, the latest value winning.
  - **Repeats by Trickle** (RFC 6206, the IETF's for low-power networks, used by RPL): a member stays quiet when it has lately heard others send the same version, its interval doubling while the group agrees and snapping back to fast on any difference. Traffic stays near flat as a group grows, where every member repeating every second would cost each device N packets a second.
  - **Roles, derived, not elected.** A job needing one doer, the clock reference, the member that renders in step 10 and sends keyframes in step 13, the one running a group update, goes to the lowest live device id unless the owner pins one. Every member computes the same answer from the same membership, so no messages are exchanged to agree.
- **MoonOrbit carries the same messages.** One WebSocket per device to the relay carries the same OSC packets and request/response pairs, so a group behaves alike on the LAN and over the internet.

### One effect across the group: the complexity levels

An effect spreads over a group as easily as its output can be computed on each device from what every device shares. The levels, easiest first, with the 83 effects' shape surveyed on 2026-11-09 (18 draw from a `Random8` stream, 14 from `hashInt`; all animate off `elapsed()` in [EffectBase.h](../../src/light/effects/EffectBase.h), so one clock swap reaches all):

| Level | The effect's output depends on | Examples | Each device needs |
|--|--|--|--|
| 1. Position and time | where a light sits and when | the shaders (Truchet, Raymarch), Plasma, Noise, Rainbow, Metaballs | the synced clock, a phase anchored to it, and its region of the group canvas |
| 2. Shared input | level 1, plus an input | AudioSpectrum (a band per device), the audio effects | level 1, plus the same input frames: audio sync from one sender, which exists |
| 3. Randomness, no memory | level 1, plus a random draw per frame | sparkles, twinkles | a draw from (seed, group tick, light), not from a stream |
| 4. Memory of its own past | its previous frames | Fire, Particles, Bouncing Balls, Water Ripple | the same simulation steps at the same times: lockstep |
| 5. Chaos, coupled across the canvas | its previous frames, every cell on its neighbors | Game of Life, ripples crossing devices | lockstep, plus detecting and repairing a divergence |
| 6. Live input on one device | a gamepad or a touch | MoonInvaders, Pong | the input as group state, applied at a time |

What breaks each level, which synced clocks and a shared seed alone leave open:

- **Level 1: a phase starts at the device's first frame.** 25 effects animate through `BeatPhase` ([math16.h](../../src/core/util/math16.h)), shaders and Plasma among them. It accumulates `dt × rate` in exact 64-bit integers, so the frame rate cannot make it drift, but it counts from the first frame each device rendered, and a speed change lands at each device's next frame boundary. Two devices are then offset by whatever separated their starts and their frames, for good. The fix is one class: the phase anchored to the effect's start on the synced clock, and a rate change applied at its "apply at T" with the integral split at T exactly, which makes the phase an exact function of synced time on every device. That one change moves all 25 to level 1.
- **Level 1: shaders are level 1 by construction.** A pixel's color is a function of where it is and when, with no state ([power-functions.md](../moonmodules/light/power-functions.md), [shader.h](../../src/light/powerfunctions/shader.h)); Truchet already turns its tiles by `hashInt` on the cell index "so two devices agree on tile (7, 3)". Shaders also gain the most, the heaviest effects per pixel, each device computing only its own share. Three things to watch:
  - **Coordinates from the group canvas.** `uv` centers and scales by the panel's own short side; across a group it takes the canvas's size and the light's position in it, or every device draws the whole pattern on itself.
  - **Floats are harmless here.** A last-bit difference changes one pixel's color by at most a step and never accumulates, since nothing carries over to the next frame; it matters only where state does.
  - **Not every member can run every shader.** Raymarch compiles only on chips with a hardware FPU, so a group checks from the members' summaries that each one has the effect, and streams it to a member that does not.
- **Neighbors in one frame: a computed halo.** An effect reading its neighbors within r in the same frame from a stateless picture, a blur over a shader, computes its region plus an r-wide ring itself, with no message, since the ring is a function of position and time too. Messages are needed only where a neighbor's value is state its own device alone holds.
- **Level 3: a random stream drifts with the frame rate.** A `Random8` stream advances per draw, so a board at 60 fps and one at 45 fps are at different points in it within a second. The textbook answer is a counter-based generator (Salmon et al., *Parallel Random Numbers: As Easy as 1, 2, 3*, 2011): a draw is a hash of (seed, tick, light), which `hashInt` already is, with the tick derived from the synced clock (`tick = synced time / group period`), never the device's frame count.
- **Level 4: a simulation steps per frame.** [FireEffect.h](../../src/light/effects/FireEffect.h) advances once per rendered frame with no time gate, so its state drifts with the frame rate even with the same seed. The textbook answer is a fixed timestep (Fiedler, *Fix Your Timestep*): step n happens at synced time t₀ + n·dt on every device, a late device running the missed steps, and rendering shows the latest step.
- **Level 4: memory that forgets heals itself.** A fade, a blur trail, Fire's cooling and WLED Soap's smoothing each shrink a difference every step (a contraction), so two devices that start apart converge within seconds once their steps and random draws agree. Memory that keeps, a particle's position or a ball's speed, carries a difference forever. So level 4 is two cases: forgetting needs lockstep and level 3 randomness but no keyframe; keeping needs a keyframe too.
- **Levels 4 and 5: floats can differ across chips.** A fused multiply-add on one compiler and not another changes a last bit, and a simulation carries that bit forward, a chaotic one growing it into a different picture. Lockstep effects use integer math, which the coding standards already prefer; Bouncing Balls uses `float` and is checked when it is converted.
- **Level 5: chaos amplifies any difference.** In Game of Life one wrong cell grows into a different board. The textbook answer, from the lockstep netcode of real-time strategy games, is desync detection: each device publishes a checksum of its state every N steps as group state, and a mismatch is repaired by a keyframe, the state copied from the render role holder. Game of Life already fingerprints its board with crc16 ([GameOfLifeEffect.h](../../src/light/effects/GameOfLifeEffect.h)), and its board is bits, about 2 KB at 128×128, a cheap keyframe.
- **Levels 4 and 5: a late joiner has no past.** An effect whose memory keeps, or a level 5 one, joins by keyframe, or by replaying from the effect's start where that is short; one whose memory forgets joins by waiting a moment.
- **Levels 4 and 5: simulating the whole canvas costs memory.** In lockstep each device simulates the whole group canvas and shows its region, the simplest correct shape; halo exchange (each simulating its region and trading border cells per step, the HPC answer) needs a reliable message per step and stalls on WiFi, so it is not used until a wall outgrows simulating whole on one device. A device that cannot hold the whole canvas degrades to being streamed.

**Each effect declares its level**, a virtual on EffectBase, and the group picks the strategy from it, the compute area with it: the member's region at levels 1 to 3, the region and a computed halo for a same-frame neighbor read, the whole canvas at levels 4 and 5. The effect is written once, as for the whole canvas. The default is the most conservative, streamed, so an effect nobody has looked at is always correct and opts into a cheaper level once checked.

**Pinned by one test for every effect: split equals whole.** On the desktop the effect renders once at full canvas and once as two half-canvas instances at different frame rates on one synced clock; at each checkpoint the halves equal the whole. A declared level that fails it is a defect.

**Prior art: WLED-MM SuperSync** (October 2023, in the local clone `MoonModules/WLED-MM`, commits `37e162472` to `71abd117b`; upstream WLED took the clock half as "Use strip.now instead of millis() in effects AKA supersync", `58eb151ba`):

- **The clock:** `strip.now`, WLED's time with its UDP-synced offset, in place of `millis()`.
- **Levels 1 and 2, done by hand per effect.** Soap, Octopus, Crazy Bees and Polar Lights replaced per-frame accumulation (`noise += mov` each call, `step++`) with a function of `strip.now`: frame count out, time in. MoonLight's effects already animate off `elapsed()`.
- **Level 3, begun:** a random seed set once at the effect's start, fixed (`random16_set_seed(535)`) or from the clock (`random16_set_seed(strip.now)`). It gives the same start where devices start together, and a stream that drifts with the frame rate after it, the gap step 12 closes.
- **The group canvas by configuration:** every node's matrix set to the whole canvas, its panel 0 placed at its offset; a SuperSync checkbox per effect made a node compute its panel only.
- **Fleet consistency:** the Nodes popup fetched each node's info and config, compared version, build, brightness scale, fps, matrix size and panel layout, marked "update needed", and a SuperSync button pushed the LED and light config to each node and rebooted it.
- **Two code paths per effect**, deterministic only while sync was on (`notifyDirect`), kept to compare both modes.

What it teaches this plan: one deterministic path for every effect, never a sync mode beside a normal one; the region from the group canvas, not a per-node matrix configured by hand; the consistency check as group state rather than a config push and a reboot; and level 4's split into memory that forgets and memory that keeps, which Soap's smoothing showed.

**Prior art: drone light shows** (Skybrush, open source): one show designed for the whole formation and cut into a timeline per drone; each drone holding only its slot; GPS time and an agreed start, each drone playing its part at `now − T0`; nothing streamed during the show, only start, abort and a heartbeat; the music on timecode; a missing drone a gap, with no master to fail. The same rule as the levels: each light's canvas coordinate, fleet time since the effect's start, shared controls and seed, each device evaluating only its own lights. A drone's timeline is closed form, so it never meets the late joiner a stateful effect does.

## Backlog

Seeing the network:

- **More discovery**: ArtPoll replies (every Art-Net node, any brand, and the card's dead-receiver skip), SSDP on port 1900 (Hue, Sonos, many IoT devices), ESPHome, Tasmota; a passive listen to mDNS announcements, a hypothesis to test against the pool exhaustion that ended mDNS queries.
- **Conflict detection**: duplicate names, two senders on one universe, two audio sync senders, a configured receiver that is silent.
- **Link quality per peer**, from the per-sender frame counts.
- **Devices placed on a floor plan** on the map, instead of the force layout.
- **A group drawn as its wall** (product owner, 2026-11-20): on the map, a group's members with a place in the wall (`positionX`, `positionY`, `positionZ` on their Layouts) are drawn at their positions in the group's box, as display settings draw monitors; members without one keep the force layout.
- **A map of thousands**: Barnes-Hut in the force layout, O(N log N), once a swarm makes the plain layout slow.
- **The desktop advertising mDNS** (the platform card), so a desktop instance is findable by name.

Acting on devices and groups:

- **The fleet clock's adjustment in the platform layer** (product owner, 2026-11-19): the published fleet time (`FleetTime`'s one static in [ClockSync.h](../../src/core/util/ClockSync.h)) moves beside `platform::uptimeUs()`, as an operating system's kernel holds the clock adjustment (`adjtime`) its NTP daemon steers; core keeps the arithmetic and the service, and `src/core` holds no mutable static for it (check_code's rule).

- **Group id from a random clock sequence** (CodeRabbit on #256, 2026-11-18): a group's id is the creating MAC and the moment it was made, so one board making a group at the same uptime on two boots repeats an id a peer's deletion marker may still refuse. RFC 4122 version 1 adds a random clock sequence for this, which needs a platform random source first.
- **A retained leave record per device group** (CodeRabbit on #256, 2026-11-18): a device that leaves a group, or is removed from it, drops the module and with it the stamp of its removal, so an older copy from a third member adopts the group again, and a self-removal a peer's copy settles before it went out never reaches the others. The fix is a saved record of the leave (group id and its stamp) beside the deletion markers: re-adopted only on a newer addition, and the self-removal kept sendable.
- **List options carry a value, not a position** (CodeRabbit on #256, 2026-11-18): a list picker sends the index of the option the page showed, which the device resolves against its list at submit time, so a Discovery row aging out between the two adds the wrong device to a group. A core change for every picker: option sets whose entries carry a stable key, the MAC for a group member, resolved by key.
- **An APT repository beside the self-update** (asked by wompslab in [issue 247](https://github.com/MoonModules/MoonLight/issues/247), 2026-11-15, and by belrik on Discord): the textbook Linux update, a signed repository on GitHub Pages with a keyring package, so `apt upgrade` installs the next version as the Firmware card does; flatpak and snap are not on the table. The self-update came first because an update from the map has to work without root; the repository joins it for the administrator who updates every package at once. The post-install script already treats an `apt install` as the administrator's choice over a self-update. About a day in the release workflow. Agreed in principle with the reporter, to pick once the current round settles.
- **Signed releases, every device**: a release signature (minisign or Ed25519, the public key compiled in) checked before any install, board or desktop, as Sparkle and apt check theirs; the SHA-256 proves integrity, the signature would prove origin beyond TLS to GitHub. Asked 2026-11-13, deferred by the product owner.
- **Hue paired once**, the bridge identity moving from each HueDriver into Devices.
- **The command half for foreign devices** (`DevicePlugin::command()`): WLED brightness, Hue, Tasmota.
- **Copy a device's settings to the group**, and a backup of the whole group, on the export step 16 builds.
- **A wall of a thousand devices** (product owner, 2026-11-20): the rendering scales as it is, each device's work set by its own lights alone; the coordination around it does not yet.
    - The wall as an aggregate: a device keeps the bounding box's extremes rather than every member's place, so the cost stays fixed at any size, where the list of at most 64 places derives a wrong wall beyond 64 members.
    - A hierarchical fleet clock, NTP's strata: a few devices sync to the reference and the rest to them, where every device asking one reference every 16 s is about 60 requests a second at a thousand.
    - The network: a thousand boards on wired Ethernet or many access points, one access point serving roughly 30 to 100 clients and multicast on WiFi running at its slowest rate.
- **The user's preview in a support session**, the preview stream over the relay at a coarse rung, so the helper also sees the lights.
- **Scheduled group updates** run by a member with no page open.
- **Non-IP transports**: MQTT-only devices, Zigbee and Thread.

Effects and games across a group:

- **MoonLive scripts declare their level by inference**: the compiler sees whether a script keeps globals across frames (memory) or calls a random builtin (level 3), so a script needs no declaration of its own.
- **Turn-based games over MoonOrbit**, delay not mattering and a game's state a few dozen bytes. A game is a list of moves and only the player on turn writes move n, so moves never conflict; the board and move number repeat as group state, so a rebooted board is back in the game within a second. Four in a row first (7×6, rules of ~50 lines, a falling disc; under a name of its own, Connect Four being Hasbro's), then Reversi and chess on 8×8, chess's move checking in the browser rather than the firmware. Beside them a shared pixel canvas, every pixel a last-writer-wins register, the purest demo of the leaderless design. Input to pick a square: a gamepad cursor, a tap on the web preview, touch, or an 8×8 MIDI pad grid.

## Cards this plan absorbs

The interop plugins and their command half live in the Backlog above, and the shared clock is step 9.

Related cards that stay on [the Project](https://github.com/users/MoonModules/projects/5):

- *ArtPoll discovery: know which tubes are alive*, a NetworkSendDriver card, its discovery half named in the Backlog.
- *The desktop build advertises no mDNS*, a platform card, named in the Backlog.
- *Read a vehicle's CAN bus and drive lights from it*: its "MoonSync" convoy note is this plan's groups and clock sync, over MoonOrbit when each car is on its own mobile data.

## What it removes

Devices as a wired child of Network; the device list's 32-row cap, the list seam's 255-row cap and OscService's 8 hosts; PictureService's inline reserve check, moved to core; the four typed peer lists, replaced by a group; typing a peer's address where discovery already knows it; a bespoke group protocol; acknowledgement and retry machinery, which state repeated with versions makes unnecessary; a leader election, which derived roles make unnecessary; and, from the Backlog, the per-driver Hue pairing.
