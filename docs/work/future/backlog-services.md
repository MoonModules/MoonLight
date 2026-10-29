# Backlog: Services

To-build items for Services, grouped by module, each with its fields on one line. Other areas: [the future index](index.md).

## InfraredService

### IR RX live-reconfigure recovery (unconfirmed, park until it recurs)

Kind: Bug · Platform: S3 · Trigger: it recurs

- **IR RX live-reconfigure recovery — unconfirmed, park until it recurs** (bench 2026-07-13, SE16). IR reception on the SE16 (`IrService` pin 5) went dead mid-session and only a **hard reset** brought it back; a warm/API path did not. **Ruled out:** not hardware (hard reset fixed it, receiver+switch+wiring fine), not LED-count (IR survives the full 16384-light / 8 fps load — a received code still toggled a control at max load), not a regression from the i80 commit (`platform_esp32_ir.cpp` untouched, the 1250 ns glitch-filter fix intact). **Prime suspect (unproven):** the session's live pin churn — including transiently setting the i80 `clockPin` to **5, which IS the IR pin** — left GPIO 5 routed to the wrong peripheral, and the RMT-RX channel (a pin-keyed static behind `platform::irStop`/`ensureChannel`) didn't re-acquire cleanly on the next `irRead`; only a full GPIO re-init (hard reset) cleared it. This may be pure test artifact (nothing in a *normal* user flow points two live modules at GPIO 5). **To conclude:** from a fresh hard reset (IR working), in isolation set i80 `clockPin=5` then restore `clockPin=8` and check whether IR dies and whether it self-recovers *without* a hard reset — self-recovers → no bug (test artifact); stays dead → a real live-reconfigure gap in the IR channel re-acquire worth fixing (per *No reboot to apply a configuration change*). Small robustness/repro work; do it only if IR breaks again in real use.

## SdCardService

### An SD card inserted after boot mounts only when its module restarts (2026-10-12)

Kind: Feature

A mount attempt on an empty slot runs the SDMMC card-init sequence to its timeouts, so `SdCardModule` never retries one on the render thread: a card inserted later mounts when the module is switched off and on, on a pin edit, or at boot. A pull is still noticed within a second through a cheap presence probe. Detecting an insertion without that stall needs a card-detect pin where the slot has one, or a mount attempt off the render thread. The timed retry PR #136 carried was dropped for this reason before its merge.

## Across modules

### A Service that calls another device's API (robwomp, 2026-10-11)

Kind: Feature

Suggested on Discord: panels driven by another stack take their pixels from MoonLight over DDP or Art-Net, but their settings stay out of reach.
A user-added Service holds a list of HTTP requests, each a method, a URL and a body, and fires one when a control it watches changes, so a button, a MIDI knob or an OSC message drives any device that answers `curl`.
Inbound control already exists (`/api/control`, MQTT, OSC, MIDI); this is its outbound twin, and the generic complement to the typed command half of DevicesModule below, which knows each ecosystem's API.
Requests leave the render thread, since a slow peer must never stall a frame.

### The Open Stage Control session's labels (2026-08-30)

Kind: Investigation

Found wiring a real control surface to the [OSC module](../../moonmodules/core/services.md).

**The shipped Open Stage Control session renders no labels, and its pad matrix draws nothing.**
[`docs/reference/examples/open-stage-control.json`](../../reference/examples/open-stage-control.json) works for every fader, encoder and switch, but the widget names never appear and the `matrix` of pads is an empty box.
Five attempts at the label property failed (`@{}`, `JS{}`, `#{}` with `unit`, an explicit string, omitting it so `"auto"` applies), and notably a session SAVED by Open Stage Control itself carries no `label` key on a fader at all, so the name is drawn from something else.
The session was written by reading a minified app's bundled HTML docs; the reliable fix is to build one widget of each kind by hand in its editor, save, and copy the shape it produces.
Cosmetic: the control path works, only the labels and the pad grid are missing.

### MoonCloud: share a MoonLive script over MoonTalk (2026-09-10)

Kind: Feature

Sketched 2026-09-10 while Talk was built. **The unique feature nobody else has**: MoonLight ships a scripting language whose programs are about a kilobyte of text, and a message board between devices. Together they mean a script someone wrote on their wall is one tap from running on yours.

**What makes it plausible.** The scripts are tiny: the shipped `.mle` files run 780 to 1907 bytes and the whole library of twenty-odd is 36 KB. A script is self-contained by design, so the text is the artifact. And the device already compiles and runs arbitrary script text safely, which is the hard half and is done.

**The design work is that a script is five to seven times a message.** Talk caps a message at 280 characters, deliberately: a chat message is a sentence, and one caller must not fill the table. Three options:

- **A second endpoint** (`/api/script`) with its own size cap and table, and a message that references it by id. The chat table stays a chat table, a script is fetched when someone wants it, and a board read stays small. Most work, cleanest shape, and the recommendation.
- **An attachment column on the message**, capped separately. One table and one post, at the cost of every board read carrying script text unless the query is careful.
- **Share a URL.** A script already lives in the user's File Manager and a device could serve it over the LAN. Free, and limited to one network, which is the case that makes the feature interesting.

**A shared script is code from a stranger**, in a way chat text is not. Three questions the design answers rather than discovers:

1. **Loading is explicit and reversible.** A script arrives as something to look at, and taking it leaves what the user already has intact. `/moonlive` (user) shadowing `/.moonlive` (factory) is the existing trap.
2. **The device is the sandbox.** MoonLive reaches no filesystem and no network, so the blast radius of a hostile script is the LEDs and the render budget. Worth confirming rather than assuming: a script that never yields is a watchdog reset, which is a denial of service on someone's wall.
3. **Attribution and removal.** Any sender id can be claimed, and a message stays once posted. Fine for chat, weaker for code.

Depends on Talk shipping and on the server being deployed.

### The sensors an installation needs (2026-09-01)

Kind: Feature

**Why this is a commitment rather than a wish list.** One of the intended uses is art installations,
and an installation people can interact with has to sense them: that is stated in
[the architecture](../../explanation/architecture/index.md#the-problem). Sensing is therefore part of the product, not a
convenience, and an input peripheral is first-class alongside an output driver. The scope is still
narrow on purpose: a lighting controller that senses its audience, not a home-automation platform.

Beyond the sensors [the input mapping plan](../present/Plan-20260901%20-%20Input%20mapping%20and%20scripted%20sensors%20(7%20of%2012).md) carries (PIR, distance, IMU, rotary encoder), the sensors worth having next:

- **Touch**: the classic ESP32's capacitive touch pins need no external part, so a conductive
  surface becomes an input. The Dig-2-Go's own button is on a touch-capable pin, though it reads
  fine as a plain digital switch.
- **Light level (LDR or a BH1750)**: an installation that dims itself to the room, and the obvious
  companion to a piece that runs day and night.

**None of these need new architecture**, which is the point of recording them together: each is a
Service under the core `Services` container, reaching the rest of the system by the plan's two paths, events and streams. What they need is a
platform seam per sensing modality (the GPIO seam shipped; I2C exists; a pulse-timing seam does
not), and a module each.

### Read a vehicle's CAN bus and drive lights from it (2026-09-11)

Kind: Feature

A MoonLight device in a car, reading the vehicle's own bus and driving aftermarket lighting from what it sees: underglow that pulses with engine RPM, sweeps with the indicator, flares on the brake. The vehicle to develop against is a VW Transporter (the product owner's).

**Read-only, and that is the design, not a limitation.** Writing to a vehicle's bus reaches systems that are mandated safety equipment (indicators and brake lights are UNECE R48), usually on a segment shared with ABS and airbags, and modifying them is illegal in most jurisdictions. Reading has none of that exposure and gets the interesting half anyway: the car reports its state, MoonLight renders it. Anything the device drives is aftermarket lighting on its own output, untouched by the vehicle.

**A Service, not a driver.** It senses rather than renders, so it belongs under the core `Services` container beside AudioService and ButtonService, and it produces both shapes the input mapping plan describes: an EVENT (indicator on, brake pressed) drives a control through `Scheduler::setControl`, while a continuous VALUE (RPM, speed) is published as a shared frame the way `AudioService::latestFrame()` is. Effects then read it, the input-mapping layer routes it, and a MoonLive script reads it as a system variable, so "underglow sweeps with the indicator" is a script someone writes and shares rather than firmware.

**Two tiers, and only the first is write-once.**

| | Standard | What it yields | How |
|---|---|---|---|
| OBD-II / UDS | Yes, legally mandated (EU: 2001 petrol, 2004 diesel) | RPM, speed, coolant, throttle, engine load | Request/response against ID `0x7DF`, so polling, a few times a second |
| Manufacturer body frames | No, proprietary per model and year | Indicators, brakes, doors, lights | Passive sniffing and reverse engineering |

J1939 is NOT the answer here despite being the "CAN lighting standard" people reach for: it is the heavy-duty protocol (trucks, agricultural, marine), and a Transporter is passenger-car architecture running VW's own frames. J1939 would only apply if MoonLight targeted commercial vehicle equipment.

So phase 1 is OBD-II only, which genuinely works on any car and needs no per-vehicle data. Phase 2 adds raw-frame sniffing, and there the frame IDs must be CONFIGURATION rather than compiled-in constants: a Transporter owner who discovers theirs shares a config file, and no firmware release is involved. That is what keeps a per-vehicle feature from becoming a per-vehicle maintenance burden.

**What it needs:**

- A `platform::twai*` seam (init, send, receive, stop). TWAI is the ESP32's CAN controller and is present on every chip MoonLight ships: 1 controller on classic and S3, 3 on the P4.
- **An external transceiver**: the ESP32 has the controller but no CAN PHY, and this is the only part that needs buying. An SN65HVD230 board (3.3V native, unlike the 5V TJA1050) plus an OBD-II pigtail to reach pins 6 (CAN-H) and 14 (CAN-L), so nothing on the vehicle is cut. Watch for a termination resistor fitted on the transceiver board: the vehicle bus is already terminated at both ends and a third resistor disturbs it. Power the device from USB while developing rather than OBD pin 16, so a crash cannot load the vehicle supply.
- A `VehicleService` module, and a desktop stub so the logic is testable without a car.

**Phase 1 is already confirmed on the van**: an ELM327 running Torque pulls PIDs from it today, so the OBD port is live, CAN is reachable there, and the standard request/response layer answers. Nothing further needs discovering before building the seam.

**Phase 2 is the open question, and one free test decides it.** Torque and ELM327 adapters speak the FILTERED request/response layer: you ask for a PID and the ECU replies, which says nothing about what else is on the wire. Torque itself can do the test: it has a dedicated `canbus monitor` protocol (selected in the vehicle profile, beside a hybrid mode that polls OBD2 and monitors at once) and imports DBC files in the PID editor to decode signals out of the stream. Traffic beyond one's own polled PIDs means the body frames are reachable on that port; only one's own requests and replies coming back means they sit on another segment behind a gateway, so phase 2 needs a different tap point. **The constraint is the adapter, not the app**: monitor-all (`AT MA`) is in the genuine ELM327 specification and many clones omit it outright, which makes them fine for standard PIDs and useless for reverse engineering. An ELM327 identifier app probes which AT commands a given adapter actually recognizes, so that is the thing to check first. Worth the five minutes before designing anything for tier 2.

**Prior art, checked 2026-09-11.** The building blocks are all open source and the closest match is [can_wizard](https://github.com/okhsunrog/can_wizard) (MIT): an ESP32-C3 plus SN65HVD230 sniffer on ESP-IDF's TWAI driver, with hardware acceptance filters, live bus state, error counters and bus-off recovery, which is close to the seam described above. It uses the legacy `driver/twai.h`, deprecated in IDF 6.x, so the new `esp_twai.h` is what to write against. Also useful: [esp32-slcan](https://github.com/mintynet/esp32-slcan) exposes an ESP32 as a standard slcan device so desktop CAN tooling works against it, and [an Alfa Romeo dashboard project](https://github.com/ClaudeMarais/AlfaRomeoGiulia_DashboardInfo_ESP32-S3) runs an ESP32-S3 on two CAN buses at once in a real car. Driving LEDs from OBD2 RPM is well-trodden ground, which is another way of saying tier 1 carries little risk.

**What is NOT available, and it shapes the design.** [awesome-automotive-can-id](https://github.com/iDoka/awesome-automotive-can-id), the main community collection, carries VW data for the MQB platform, Golf, Polo, Touran and e-Up, but **nothing for the Transporter and no lighting signals for any VW**: its coverage is drivetrain and engine management, not body control. So tier 2 starts from an empty database however much searching precedes it, which is exactly why the frame IDs belong in configuration a user discovers and shares rather than in a compiled-in table. (T6 owner forums do report the reverse signal traveling over CAN, so the body signals exist on a bus; they are simply not publicly documented.)

**Related:** a convoy light show across several cars needs nothing from this. That is MoonSync (planned, not built) synchronizing timebase and effect state between devices, with each car rendering locally through whatever driver it has. The two compose for free and neither blocks the other.
