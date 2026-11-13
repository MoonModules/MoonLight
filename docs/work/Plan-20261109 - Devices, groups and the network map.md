# Plan: the network map, groups and supersync

Devices becomes a top-level module that shows what is on the network, lets devices form groups, and lets a group act as one: the same controls and the same clock, up to one effect spread over many boards (supersync), and finally groups across the internet (MoonOrbit), which also lets a helper look at another user's device. Each step ships on its own and the map comes first, so every later step is visible the moment it lands. Ideas that come up while building go to the Backlog and are promoted to a step when picked.

## The phases

| Phase | Steps | What it gives | Its demo |
|--|--|--|--|
| **See the network** | 1 to 5 | a map of every device, what each offers and sends, its health, and a blink to find it | the bench drawn as one map |
| **Act as a group** | 6 to 10 | groups, shared controls and presets without typing addresses, group updates, one clock | a line sweeping straight across boards |
| **One effect across the group** | 11 to 15 | supersync, from streaming to every board computing its own part | a ball crossing screens; Pong with two gamepads |
| **Beyond the LAN** | 16 and 17 | the same groups across the internet, and help for another user by looking at their device | a Pong rally between two homes; a user's setup replicated on a helper's desktop |

## The design basis

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

## What exists

- **Discovery** ([DevicesModule.h](../../src/core/system/DevicesModule.h)): passive, from the 44-byte presence packet on multicast and broadcast ([WledPacket.h](../../src/core/system/WledPacket.h)); MoonLight, WLED and Hue bridges, typed by plugins ([DevicePlugin.h](../../src/core/system/DevicePlugin.h)); a host setting naming a board resolves from it ([HostResolver.h](../../src/core/util/HostResolver.h)). A code-wired child of Network today ([main.cpp](../../src/main.cpp), [module_types.cpp](../../src/module_types.cpp)).
- **Shared controls** over OSC ([OscService.h](../../src/core/services/OscService.h)): the controls half of supersync, live ([mooncore.md](../explanation/architecture/mooncore.md#multi-device-runtime)).
- **Audio sync**, following one sender and counting frames per sender ([AudioService.h](../../src/core/services/AudioService.h)).
- **Light between devices** over Art-Net, E1.31 and DDP: NetworkSendDriver and the Network Receive effect.
- **Firmware by URL or upload** on every device: `POST /api/firmware/url`, and `/api/firmware/upload` where the device has no TLS ([HttpServerModule.cpp](../../src/core/system/HttpServerModule.cpp)).
- **Presets** across the whole device ([ControlModule](../../src/core/system/ControlModule.h)).
- **Peer lists in four places**: `hosts` on OscService, AudioService and NetworkSendDriver, and `host` on MidiService.
- **Two caps that break the dynamic rule**: the device list holds 32 (`kMaxDevices` in [DevicesModule.h](../../src/core/system/DevicesModule.h)), and the list seam counts rows in a `uint8_t` (`ListSource` in [Control.h](../../src/core/module/Control.h)), so no list can pass 255. OscService feeds back to at most 8 hosts. Step 1 removes the first two, step 7 the third; all three are in [What it removes](#what-it-removes).

## What the network carries

Looked at before built (product owner, 2026-11-09): a throwaway listener on the Mac, not committed, reported what the bench network carries on the well-known ports, so the data model follows the real traffic rather than a guess. Three minutes, one ArtPoll and one SSDP search every 30 s, presence read from the desktop's own list:

- **Presence:** 11 MoonLight boards and a WLED, the WLED heard every ~30 s.
- **SSDP:** the Hue bridge (.143), its `SERVER` header naming it and its `USN` carrying a stable uuid, found with no HueDriver configured.
- **Art-Net:** two MoonLight boards answered ArtPoll (.156, .204, both running Network Receive), each as `MoonLight` / `MoonLight NetworkReceive`, hard-coded in [NetworkReceiveEffect.h](../../src/light/effects/NetworkReceiveEffect.h), so a poll cannot tell them apart. Their reply carries a MAC.
- **Audio sync:** .221 sending both formats to their multicast groups, 42 packets a second, about 5.5 KB/s.
- **mDNS:** six senders that are no MoonLight board (phones, computers, media devices), unnamed until their records are parsed.
- **Nothing** on sACN, DDP, OSC, RTP-MIDI or WLED's notifier: idle, or unicast between others, which the Mac cannot see. The desktop's DDP stream to .156 was running all along, unseen by the listener and reported by both ends.

What it means for the data model:

- **One device, several sightings.** A node's kind comes from several sources, a MoonLight board by presence and by ArtPoll, so nodes merge by address.
- **Identity.** A device is its MAC. MoonLight's and MoonBase's presence packet appends the base MAC, the same on every interface; WLED's 44 bytes carry none, but WLED, MoonLight and ESPHome name it in their mDNS records; ArtPollReply has one and SSDP a uuid. Discovery keys a row by the MAC once a source names it (built 2026-11-10: the packet, the ESPHome and WLED records; Tasmota's status page and Hue's SSDP id to come).
- **A multicast stream goes to a group, not a device.** The map has a node for a multicast group, its listeners joined to it.
- **Services beside flows.** A device offers things while nothing flows: .101 shares MIDI as an RTP-MIDI session on port 5004, waiting for a desk and a peer. So a node carries its services (an RTP-MIDI session, Art-Net or DDP reception and its universes, OSC in, an audio sync send, its page) as well as its flows, and a session's participants become flows once they join. Foreign RTP-MIDI hosts announce themselves as `_apple-midi._udp` over mDNS.
- **Both ends of a flow report it.** The desktop's DDP to .156 shows in the sender's status (1505 packets/s) and the receiver's (1500, 0 lost, from MM-Desktop), as free text today. Matching the two finds a broken link: a sender reporting a flow its receiver does not.
- **A defect:** the ArtPollReply names become the device's own name (step 1).

## The message design

No new protocol (product owner, 2026-11-09): each kind of message already has its textbook one, and what is designed is the rules on top. The only new formats are documents: the summary and flows JSON, and the OSC addresses of group state.

| Kind | Loss | Protocol |
|--|--|--|
| **Streams**: light, audio, video | acceptable, latest wins | Art-Net, DDP, E1.31, the MoonLight audio sync format, each its own |
| **Group state**: preset, controls, clock, roles | repaired by the next send | OSC over the group's multicast, which already carries shared controls |
| **Requests**: summary, flows, identify, firmware | must arrive | REST over TCP, every peer answering cross-origin (`Access-Control-Allow-Origin: *`) |
| **Discovery** | acceptable | presence, ArtPoll, SSDP, mDNS |

- **Audio sync stays its own packet.** It is a stream at ~42 packets a second, WLED-compatible in one format; carrying must-arrive state in it would tie that state to a lossy stream.
- **State, not commands.** A group's state is published as absolute values ("preset X, apply at T"), on a change at once and then repeated, so a lost packet is repaired by the next repeat and a member that joins or reboots catches up. Applying a value twice changes nothing, so no acknowledgements are needed and a swarm costs one multicast send. Presence already works this way. REST stays for what is not state: an update, identify, reading a peer.
- **Apply at a time.** OSC bundles carry an NTP timetag, OSC 1.0's own "execute at T". [OscPacket.h](../../src/core/services/OscPacket.h) recognizes bundles and rejects them today, "a timed batch we have no use for yet"; step 10 gives them their use.
- **No races, no loops:**
  - **Leaderless: every member may write** (product owner, 2026-11-09). Each value is a last-writer-wins register (Shapiro et al., *Conflict-free Replicated Data Types*, 2011): it carries a timestamp and its writer's device id, a device applies only a newer one, and a tie goes to the higher id. Every member converges on the same value whatever order packets arrive in, so concurrent edits do not race: two people on one slider at once, one edit wins and all agree which. The timestamp is a hybrid logical clock (Kulkarni et al., 2014), which orders edits before step 10 syncs the clocks and by real time after. Duplicates, reordering and stale values are ignored for the same reason. Today's shared controls are peer-to-peer with an echo check ([OscService.h](../../src/core/services/OscService.h)); whether two boards changing one control at once can end apart is a hypothesis, which the registers settle either way.
  - **No forwarding.** A device publishes only what it wrote itself and never re-sends what it received, so no loop can form.
  - **Coalescing.** A slider drag publishes at most ~20 times a second, the latest value winning.
  - **Repeats by Trickle** (RFC 6206, the IETF's for low-power networks, used by RPL): a member stays quiet when it has lately heard others send the same version, its interval doubling while the group agrees and snapping back to fast on any difference. Traffic stays near flat as a group grows, where every member repeating every second would cost each device N packets a second.
  - **Roles, derived, not elected.** A job needing one doer, the clock reference, the member that renders in step 11 and sends keyframes in step 14, the one running a group update, goes to the lowest live device id unless the owner pins one. Every member computes the same answer from the same membership, so no messages are exchanged to agree.
- **MoonOrbit carries the same messages.** One WebSocket per device to the relay carries the same OSC packets and request/response pairs, so a group behaves alike on the LAN and over the internet.

## One effect across the group: the complexity levels

An effect spreads over a group as easily as its output can be computed on each device from what every device shares. The levels, easiest first, with the 83 effects' shape surveyed on 2026-11-09 (18 draw from a `Random8` stream, 14 from `hashInt`; all animate off `elapsed()` in [EffectBase.h](../../src/light/effects/EffectBase.h), so one clock swap reaches all):

| Level | The effect's output depends on | Examples | Each device needs |
|--|--|--|--|
| 1. Position and time | where a light sits and when | the shaders (Truchet, Raymarch), Plasma, Noise, Rainbow, Metaballs | the synced clock, a phase anchored to it, and its region of the group canvas |
| 2. Shared input | level 1, plus an input | AudioSpectrum (a band per device), the audio effects | level 1, plus the same input frames: audio sync from one sender, which exists |
| 3. Randomness, no memory | level 1, plus a random draw per frame | sparkles, twinkles | a draw from (seed, group tick, light), not from a stream |
| 4. Memory of its own past | its previous frames | Fire, Particles, Bouncing Balls, Water Ripple | the same simulation steps at the same times: lockstep |
| 5. Chaos, coupled across the canvas | its previous frames, every cell on its neighbors | Game of Life, ripples crossing devices | lockstep, plus detecting and repairing a divergence |
| 6. Live input on one device | a gamepad or a touch | Space Invaders, Pong | the input as group state, applied at a time |

What breaks each level, which synced clocks and a shared seed alone leave open:

- **Level 1: a phase starts at the device's first frame.** 25 effects animate through `BeatPhase` ([math16.h](../../src/core/util/math16.h)), shaders and Plasma among them. It accumulates `dt × rate` in exact 64-bit integers, so the frame rate cannot make it drift, but it counts from the first frame each device rendered, and a speed change lands at each device's next frame boundary. Two devices are then offset by whatever separated their starts and their frames, for good. The fix is one class: the phase anchored to the effect's start on the synced clock, and a rate change applied at its "apply at T" with the integral split at T exactly, which makes the phase an exact function of synced time on every device. That one change moves all 25 to level 1.
- **Level 1: shaders are level 1 by construction.** A pixel's color is a function of where it is and when, with no state ([power-functions.md](../moonmodules/light/power-functions.md), [shader.h](../../src/light/powerfunctions/shader.h)); Truchet already turns its tiles by `hashInt` on the cell index "so two devices agree on tile (7, 3)". Shaders also gain the most, the heaviest effects per pixel, each device computing only its own share. Three things to watch:
  - **Coordinates from the group canvas.** `uv` centers and scales by the panel's own short side; across a group it takes the canvas's size and the light's position in it, or every device draws the whole pattern on itself.
  - **Floats are harmless here.** A last-bit difference changes one pixel's color by at most a step and never accumulates, since nothing carries over to the next frame; it matters only where state does.
  - **Not every member can run every shader.** Raymarch compiles only on chips with a hardware FPU, so a group checks from the members' summaries that each one has the effect, and streams it to a member that does not.
- **Level 3: a random stream drifts with the frame rate.** A `Random8` stream advances per draw, so a board at 60 fps and one at 45 fps are at different points in it within a second. The textbook answer is a counter-based generator (Salmon et al., *Parallel Random Numbers: As Easy as 1, 2, 3*, 2011): a draw is a hash of (seed, tick, light), which `hashInt` already is, with the tick derived from the synced clock (`tick = synced time / group period`), never the device's frame count.
- **Level 4: a simulation steps per frame.** [FireEffect.h](../../src/light/effects/FireEffect.h) advances once per rendered frame with no time gate, so its state drifts with the frame rate even with the same seed. The textbook answer is a fixed timestep (Fiedler, *Fix Your Timestep*): step n happens at synced time t₀ + n·dt on every device, a late device running the missed steps, and rendering shows the latest step.
- **Level 4: memory that forgets heals itself.** A fade, a blur trail, Fire's cooling and WLED Soap's smoothing each shrink a difference every step (a contraction), so two devices that start apart converge within seconds once their steps and random draws agree. Memory that keeps, a particle's position or a ball's speed, carries a difference forever. So level 4 is two cases: forgetting needs lockstep and level 3 randomness but no keyframe; keeping needs a keyframe too.
- **Levels 4 and 5: floats can differ across chips.** A fused multiply-add on one compiler and not another changes a last bit, and a simulation carries that bit forward, a chaotic one growing it into a different picture. Lockstep effects use integer math, which the coding standards already prefer; Bouncing Balls uses `float` and is checked when it is converted.
- **Level 5: chaos amplifies any difference.** In Game of Life one wrong cell grows into a different board. The textbook answer, from the lockstep netcode of real-time strategy games, is desync detection: each device publishes a checksum of its state every N steps as group state, and a mismatch is repaired by a keyframe, the state copied from the render role holder. Game of Life already fingerprints its board with crc16 ([GameOfLifeEffect.h](../../src/light/effects/GameOfLifeEffect.h)), and its board is bits, about 2 KB at 128×128, a cheap keyframe.
- **Levels 4 and 5: a late joiner has no past.** An effect whose memory keeps, or a level 5 one, joins by keyframe, or by replaying from the effect's start where that is short; one whose memory forgets joins by waiting a moment.
- **Levels 4 and 5: simulating the whole canvas costs memory.** In lockstep each device simulates the whole group canvas and shows its region, the simplest correct shape; halo exchange (each simulating its region and trading border cells per step, the HPC answer) needs a reliable message per step and stalls on WiFi, so it is not used. A device that cannot hold the whole canvas degrades to being streamed.

**Each effect declares its level**, a virtual on EffectBase, and the group picks the strategy from it. The default is the most conservative, streamed, so an effect nobody has looked at is always correct and opts into a cheaper level once checked.

**Pinned by one test for every effect: split equals whole.** On the desktop the effect renders once at full canvas and once as two half-canvas instances at different frame rates on one synced clock; at each checkpoint the halves equal the whole. A declared level that fails it is a defect.

**Prior art: WLED-MM SuperSync** (October 2023, in the local clone `MoonModules/WLED-MM`, commits `37e162472` to `71abd117b`; upstream WLED took the clock half as "Use strip.now instead of millis() in effects AKA supersync", `58eb151ba`):

- **The clock:** `strip.now`, WLED's time with its UDP-synced offset, in place of `millis()`.
- **Levels 1 and 2, done by hand per effect.** Soap, Octopus, Crazy Bees and Polar Lights replaced per-frame accumulation (`noise += mov` each call, `step++`) with a function of `strip.now`: frame count out, time in. MoonLight's effects already animate off `elapsed()`.
- **Level 3, begun:** a random seed set once at the effect's start, fixed (`random16_set_seed(535)`) or from the clock (`random16_set_seed(strip.now)`). It gives the same start where devices start together, and a stream that drifts with the frame rate after it, the gap step 13 closes.
- **The group canvas by configuration:** every node's matrix set to the whole canvas, its panel 0 placed at its offset; a SuperSync checkbox per effect made a node compute its panel only.
- **Fleet consistency:** the Nodes popup fetched each node's info and config, compared version, build, brightness scale, fps, matrix size and panel layout, marked "update needed", and a SuperSync button pushed the LED and light config to each node and rebooted it.
- **Two code paths per effect**, deterministic only while sync was on (`notifyDirect`), kept to compare both modes.

What it teaches this plan: one deterministic path for every effect, never a sync mode beside a normal one; the region from the group canvas, not a per-node matrix configured by hand; the consistency check as group state rather than a config push and a reboot; and level 4's split into memory that forgets and memory that keeps, which Soap's smoothing showed.

## Steps

### See the network

#### 1. Devices on top, drawn as a map ✅

Landed in: the [Devices page](../moonmodules/core/system.md) (its card, the map, growth and what memory refuses, from `DevicesModule.h`); the allocation rule and `platform::fitsWithReserve` in [moonmodule.md](../explanation/architecture/moonmodule.md); the list drawn as a graph, `ListSource::listAsGraph()` in [Control.h](../../src/core/module/Control.h); the ArtPoll reply named after the device in [NetworkReceiveEffect.h](../../src/light/effects/NetworkReceiveEffect.h); the move in [MIGRATING](../reference/MIGRATING.md) ("Discovery moves from Network to Devices").

#### 2. This device's flows and services 🧪 (test: OSC, PanelCard, Hue and MQTT seen on the bench, no flashed board running them yet)

- **Modules report, the socket counts nothing** (product owner, 2026-11-09). Each network module reports its services and its flows, protocol, direction, peer and rate, from the counters it already keeps for its status line, the way every module reports its memory and tick time. Counting at the UDP socket would have caught every module, but a per-peer table there runs on every packet from several tasks, needing a lock or an allocation in the send path; reporting adds nothing per packet. A module that sends without reporting stays off the map.
- The services the device offers, from the modules that offer them: an RTP-MIDI session, Art-Net or DDP reception, the audio sync port, the web page.
- The map draws this device's flows as edges labeled with protocol and rate, a multicast stream to a group node, and its services in its tooltip and detail. A remote address not in the list gets a row of its own, so a sender nobody configured shows up.
- **Verified by:** NetworkSendDriver to a bench board and audio sync from a WLED, both drawn; .101's RTP-MIDI session shown on its node.
- **Built (2026-11-10).** `NetworkUse` ([NetworkUse.h](../../src/core/module/NetworkUse.h)) and `MoonModule::reportNetworkUse`, reported by NetworkSendDriver (frames a second to each receiver, the group or the broadcast address), NetworkReceiveEffect (its ports, the sender it receives), AudioService through AudioSync (its port and followed sender, or each format's group or each host), MidiService (the RTP-MIDI session and its peer) and HttpServerModule (the page's port). Discovery collects them once a second through `Scheduler::instance()`, gives each peer a row, `group` for a multicast group or the broadcast address (`DevType::Group`), and writes each row's edges, this device's outbound flows from its own row and a peer's inbound flow from the peer's row, and this device's services. A flow from a peer, or through a group, is a sighting; one sent to a silent receiver is not, so a dead receiver still ages out. Tests ([unit_DevicesModule_flows.cpp](../../test/unit/core/unit_DevicesModule_flows.cpp)): a peer's row, a group's, the outbound and inbound edges, the services, a disabled module reporting nothing. On the desktop: its row's edge to .156, "DDP 46 fps", and its service "HTTP 8080". Then OscService (its port, the client, where feedback goes), PanelCardDriver (a "ColorLight" service, raw Ethernet having no IP peer), HueDriver (the bridge) and MqttModule (the broker, when set as an address, since the connection resolves a name inside and keeps no address).
- **Built (2026-11-10), at the product owner's ask: Home Assistant on the map.** HttpServerModule reports each client of the WLED API (`/json`, `/win`, `/presets.json`) as an inbound flow, "WLED API 6 req/min" over the last whole minute, from a table that grows as a client first asks and drops one quiet for five minutes. A flow may name its peer (`NetworkUse::flow`'s `peerName`), and the client's User-Agent product token does: `HomeAssistant/` is Home Assistant (RFC 9110 § 10.1.5). The name labels a generic row only, never a MoonLight board's or a WLED's. Tests: the clients, the name, the rate, a quiet client dropped; a named peer listed by its name.

#### 3. Peers' flows: the whole MoonLight map 🧪 (test: .156 and .204 on port 8080 merged, MM-P4-hdmi once, the bench's nodes readable)

- Each device's graph is in its own state; the page reads each MoonLight peer's graph over REST and merges them, so flows between two other MoonLight devices appear.
- Both ends of a flow report it, so a flow its receiver does not report is drawn as a broken link.
- A peer that does not answer keeps its node, marked unreachable. A few requests run at a time, so a swarm loads progressively rather than all at once.
- **Verified by:** the desktop's DDP to .156 drawn on a third board's map; the receiver stopped and the link drawn broken.
- **Built (2026-11-10).** In [app.js](../../src/ui/app.js): while the map is on screen, every node of the same kind as this device's own reads the same list at its `url`, through `GET /api/modules/<name>`, four at a time with a three-second timeout, again five seconds after the round ends. Devices a peer sees and this one does not join the map. In each graph a device's own row holds what it sends and another row what it receives from that row, so a link is drawn once, the sender's label winning, and drawn broken (red, dashed) when its receiver, a peer that answered, reports nothing arriving. A peer that does not answer is drawn unreachable; one answering without the list, older firmware, is drawn as it was. No firmware change: the module route and its list detail serve it.
- **Bench (2026-11-10), seven boards on this branch** (.221, .101, .132, .158, .203, .122, .207) and the desktop: the desktop's "E1.31 44 fps" to MM-NanoPi and .221's "audio sync 23 fps" to 239.0.0.1 and 239.0.0.2 drawn, the other boards as nodes without flows, which is what they run. Three faults seen. MM-NanoPi (.156, port 8080) and MM-Pi5-1 (.204, port 8080) are drawn unreachable, the port gap below. MM-P4-hdmi appears twice, once at a link-local 169.254.x.x address, which no peer can reach (RFC 3927). Unlinked nodes are pushed to the box's edges, where their labels overlap and are clipped, and a node shows its name only.
- **Built (2026-11-10), after the bench.** *The port:* a MoonLight device whose page is not on port 80 appends the port after the 44-byte presence header ([WledPacket.h](../../src/core/system/WledPacket.h)), read from the web server's own HTTP service report; WLED reads 40 bytes or more and ignores the rest, and absent means 80, as in an http URL. mDNS's SRV record was the other carrier, but Discovery never queries mDNS (it starved the ESP32's own advertising) and the desktop and Linux builds advertise none. A row keeps the port and its `url` carries it. *Link-local:* Discovery gives no row to 169.254.0.0/16 (RFC 3927). *Not saved:* the list is runtime state, rebuilt within seconds of boot, so disabling Discovery frees it and saves nothing (pre-merge review). *The layout:* the linked nodes, this device among them, are placed by force; the others sit in a grid beneath, by name, the picture growing with them, each label kept inside. *On each node:* its address, its version, and what it offers, from each MoonLight peer's own row, which now carries its version and firmware. Older firmware does not read a 46-byte presence, so it no longer lists a device on port 8080 ([MIGRATING](../reference/MIGRATING.md)).

#### 4. A peer's summary, and identify ✅

Landed in: the [Devices page](../moonmodules/core/system.md) ("The summary is `/api/system`", a node's popup, its firmware and identify); the summary's fields in [the REST API](../reference/api.md) (`/api/system`); keyboard-reachable nodes and the duplicate `Content-Length` refusal (CodeRabbit, PR #242) in `buildListGraph` and [HttpHead.h](../../src/core/util/HttpHead.h), each pinned by a test.

#### 5. What drives each device's lights ✅

Landed in: the [Devices page](../moonmodules/core/system.md) (the emoji a node's lights show); the `outputs` field in [the REST API](../reference/api.md) (`/api/system`), from `DriverBase::drivesLights` and `windowLength`.

### Before groups

Planned by the product owner (2026-11-10) after step 5 and before step 6, each shipping on its own.

#### Every Hue bridge on the map ⏳

- Not only a bridge a HueDriver points at: Hue's own discovery, mDNS `_hue._tcp`, through a passive listen to mDNS announcements, or SSDP on port 1900. With the rest of the Backlog's More discovery, since they share the mechanism.
- It needs a hardware test: mDNS queries were ended by a pool exhaustion on the ESP32, so the passive listen is a hypothesis until a board runs it.
- **Verified by:** a Hue bridge nobody configured on the bench map.

#### Every desktop updates itself through the Firmware module 🧪 (test: Windows, by CI's `smoke-update` on the next push)

Asked by the product owner (2026-11-12): one way to update any MoonLight device, in two steps, this one and the map using it. A desktop prepares the new version beside the running one and a restart activates it, as a board writes its other slot and reboots into it, through the same routes, the same Firmware card and the same asset naming. The line runs between a native install, which updates itself on every OS, and a container, which its host replaces. Settled with the product owner: the Linux package runs a service enabled on install, rollback by systemd, settings moved by hand, the digest from the browser.

- **The image is the bare program.** The release publishes each desktop's program as `firmware-desktop-<os>-<arch>-v<version>.bin`, the firmware registry's names (`desktop-linux-arm64`), beside the archives and installers it ships. A desktop build carries that name as its firmware, where it said `unknown`, so every path that builds `firmware-<name>-v<version>.bin` reaches a desktop: the Firmware card and its picker, the map, MoonDeck, Home Assistant's update over MQTT.
- **`otaBegin`, `otaWrite`, `otaEnd` on a desktop.** The image goes to `MoonLight.next` beside the running program. `otaEnd` vets it, an executable of this OS and architecture by its header (ELF, Mach-O, PE), as a board vets an image's magic and chip, and checks the SHA-256 where the request names one; it then keeps the running program as `MoonLight.prev` and renames the new one into place (rename(2), atomic; Windows renames the running `.exe` aside, which it allows). The running process keeps its old image. `http_fetch_to_ota` and `/api/firmware/upload` drive it unchanged, libcurl or WinHTTP fetching for the first. The `hasOta` gates become the update mode.
- **The restart activates it.** `platform::reboot()` starts the program again: on macOS and Linux the new program takes over the process in place (`execv`), same terminal window, same process to systemd or a container runtime, so a reboot is never a start their restart counting sees, with every socket marked close-on-exec where it is made; on Windows the new `.exe` starts in the same console and the old one exits. The reboot button shows on every device.
- **The update mode.** `/api/system`'s `update`, and the Firmware card's: `self` for a native install whose program sits in a folder it can write, `host` for a container, `manual` for a program in a folder it cannot write, such as the package's seed under `/usr/lib`.
- **The Linux package runs a service, and the service owns its program.** Bespoke, with its reason: the textbook Linux update is an APT repository, which needs root to update from the map, and the network-facing service never holds root; a self-owned copy beside a package-tracked seed is how Syncthing, a network daemon, replaces its own binary. New for MoonLight and uncommon on Linux, where the package manager is the norm. The `.deb` ships the program as a seed at `/usr/lib/moonlight/MoonLight`, a `moonlight` system user in the `video` group, and `moonlight.service`, which its post-install script enables and starts the way debhelper's snippets do, skipping where no systemd runs. The unit runs as that user with `StateDirectory=moonlight`, settings in `/var/lib/moonlight` (`XDG_DATA_HOME`), and `Restart=always`. The post-install script copies the seed to `/var/lib/moonlight/bin/MoonLight`, owned by the service user, on every install: an `apt install` is the administrator's explicit choice, so it wins over a self-update, older or newer, and drops `.prev`. The menu entry opens `http://localhost:8080/` instead of starting a second copy on the same port. No sudo rule and no Docker socket, so the network-facing service never holds root.
- **Rollback is systemd's, for the service, by boot assessment.** The swap leaves `MoonLight.pending` beside the program, and the new program deletes it once it has stayed up `kBootStableMs`, the minute a board's boot record uses: systemd's Automatic Boot Assessment, for one program. `OnFailure=moonlight-rollback.service`, which systemd starts on every failed start: a pending program that fails, a crash before `main` included, is replaced by `.prev` and the service started again; a confirmed one is left alone, so restarts of a good program never undo it. `StartLimitBurst=3` within `StartLimitIntervalSec=120` stops a program that keeps failing after that. Elsewhere `.prev` stays beside the program, to rename back by hand.
- **The Firmware card on a desktop** installs as a board's does, by URL from its picker or by upload, when the update mode is `self`; otherwise it offers the download. The shared install picker tells a desktop's own image (`firmware-desktop-…bin`) from the installer's download of the same name.
- **A hypothesis to test first:** a macOS bundle whose inner binary was replaced still launches, Terminal running the binary itself rather than the sealed bundle.
- **Built (2026-11-13).** The image writer, the swap and the restart in [platform_desktop_update.cpp](../../src/platform/desktop/platform_desktop_update.cpp), the header vetting in [ProgramImage.h](../../src/core/util/ProgramImage.h), the download through the one HTTP client (`httpDownload`), the firmware name from CMake, the sockets closed on exec, `hasOta` gone, the bare programs staged by `package_desktop.py`, the Firmware card installing on `self` and offering the download otherwise, the picker telling a desktop's program from its download, the live scenario leaving a desktop to restart itself. Tests: `unit_platform_update.cpp`, `unit_ProgramImage.cpp`, `ui-update-asset.test.mjs`, `test_package_deb.py`. CodeRabbit's 15 findings and a CodeQL alert fixed in the PR's second commit, among them the Linux restart, which read `/proc/self/exe` after the swap and found the old program "(deleted)".
- **Removes:** the archive path, the reboot button hidden on desktops, `unknown` as a desktop's firmware name, the Firmware card's desktop download in place of an install, MoonWalk's hand-written unit steps. **Fixed on the way:** the reboot button hidden for a container (`^docker$` never matched `docker-arm64`).
- **Costs** a MIGRATING entry: the hand-written unit goes, the program moves from `/usr/bin` to `/usr/lib/moonlight`, and settings move from the old user's home to `/var/lib/moonlight` with one copy command. The Dockerfile copies the program from the new seed path. Unblocks step 9 for a group that mixes boards and desktops.
- **Tested (2026-11-13), by the product owner and the agent.** This Mac: an upload installs and restarts in place, same PID, `.pending` cleared after a minute; a board image refused as no program; a URL with a wrong digest refused, with the right one installed and restarted. .204, the Raspberry Pi 5, from the PR's CI `.deb`: the service runs as `moonlight` holding `CAP_NET_RAW` alone, `update: self`; an upload restarts it in place, same PID, no start counted; `/bin/false` installed as an update rolls back to `.prev` within a second; an `apt install` puts the package's program back and drops `.prev`; HDMI shows the effect under the service user. .156, the NanoPi: the PR's program in its container image says `update: host`, refuses both install routes with 501 and restarts in place. The old hand-written unit and a source-build drop-in had to go first, which the MIGRATING entry covers for the unit. CI on the PR: every desktop compiles, Linux and Windows included.
- **Verified by:** .204's package updated from its own Firmware card and rolled back from a program made to fail; this Mac updated from its own Firmware card, by URL and by upload; Windows by CI's `smoke-update`, which installs a program through its own upload route and checks it restarts with `.prev` beside it, on each runner that builds a desktop; .156's container saying its host updates it.

#### The map updates a desktop as it updates a board ⏳ (after the step above)

- **The same call as for a board or a Teensy:** the node's popup posts `firmware-<name>-v<version>.bin` to `/api/firmware/url`, with GitHub's digest, and watches `/api/system` until the device answers on the new version.
- **What only the map adds:** the device's `update` read from `/api/system`; a container's node says its host updates it, a program in a read-only folder that it is updated by hand.
- **Removes** the desktop special cases the map and the update badge carry: the badge opening the release page for every desktop, `desktopAssetPrefix`, the desktop branch of the asset choice. **Fixed on the way:** the update badge read the browser's OS, and always x64 on Linux.
- **Built (2026-11-12), on the archive naming:** the popup offering an update to a `self` device and telling a container its host updates it; the asset choice by the device's own model (`ui-update-asset.test.mjs`). The uniform name reduces it to the board's path.
- **Verified by:** .204, this Mac and a Windows PC each updated from another device's map; .156's container node saying its host updates it.

#### A container updated from the map, through Watchtower ⏳

Asked by the product owner (2026-11-12), after the desktops.

- **The container stays without the Docker socket**, so the device never holds root on its host. Watchtower holds it, and its HTTP API (`--http-api-update`, a token) starts an update of the containers it watches, the same on Linux, Docker Desktop for macOS and Windows, and a NAS.
- **The map calls it.** A setting for Watchtower's address and token, kept in the browser; a container node (`update: host`) then gets "Update through Watchtower", and the map watches `/api/system` until the container answers on the new version. Without a Watchtower set, the node says its host updates it, as now. About 30 lines of JS, nothing on the device.
- **Podman's auto-update** has no API and stays the host's.
- **Verified by:** .156's container updated from the map through a Watchtower on the NanoPi.

#### MoonBase and the WiFi coprocessor updated from the map ⏳

Asked by the product owner (2026-11-12), after the desktops.

- **The routes exist.** `POST /api/firmware/moonbase-update-url` takes the release's MoonBase image, and `POST /api/firmware/coprocessor-update-url` with an empty URL takes the image matching the device's own release; the device fetches either itself, as it does the app.
- **What the map needs: when each one is due.** `/api/system` reports a MoonBase that does not match the app (the Firmware card's `moonbaseMismatch`) and an outdated coprocessor (`coprocessorMismatch`), each absent while current.
- **What the map offers.** A node's popup shows "Update MoonBase" and "Update WiFi coprocessor" beside the app's update, only when due, each pressed twice like the app's, posting to its route and watching `/api/system` until the peer reports it current. A board without TLS is sent to its own Firmware card, as for the app.
- **Verified by:** a bench board with an older MoonBase and the P4 with an older coprocessor image, each brought current from another device's map.

#### WLED to MoonLight from the map ✅

Landed in: [Migrating over the air](../reference/migrating-over-the-air.md) (what can move, by chip and firmware, and what can stop it); the [Move a WLED device](../moonwalk.md#migrate-wled) process; the migration image's steps, the bootloader and table per flash size and type, in [migrate_main.cpp](https://github.com/MoonModules/MoonLight/blob/main/moonbase/migrate/main/migrate_main.cpp); one image per family built with its first firmware in [build_esp32.py](https://github.com/MoonModules/MoonLight/blob/main/moondeck/build/build_esp32.py) (`migration_family`, `migration_tables`); a board in MoonBase on the map in [moonbase.md](../explanation/architecture/moonbase.md); the power-cut rounds in [wled_migration_test.py](https://github.com/MoonModules/MoonLight/blob/main/moondeck/build/wled_migration_test.py). Verified from the map: WLED 0.15 on the classic, the C3 and the 16 MB S3, WLED-MM on the P4 (2026-11-11); the 4 and 8 MB S3 are left to users' reports.

#### ESPHome, Tasmota and others to MoonLight from the map ✅

Landed in: [Migrating over the air](../reference/migrating-over-the-air.md) (Tasmota, ESPHome and any other firmware by hand, with the WiFi each carries); the Devices map's guards for other firmwares in [security](../explanation/security.md#the-devices-map); the readers of each firmware's saved WiFi, ESP-IDF's own as the last, in [WledNetworks.h](../../src/core/util/WledNetworks.h); ESPHome's update protocol and the relay's query in [EsphomeOta.h](../../src/core/util/EsphomeOta.h). Verified from the map: Tasmota 16 and ESPHome Web on the classic, ESPHome through the relay on an S3 (2026-11-11); the S3 and C3 of both are left to users' reports.

- **Still open, each its own track:** ESPixelStick (its EFU container), OpenMQTTGateway (pulls the image through `/up?o=`), Shelly Group's relay firmware (its own ZIP package, its table at 0x10000).

#### MoonBase's page installs through the shared picker ✅

Landed in: [moonbase.md](../explanation/architecture/moonbase.md) (its page installs through the picker loaded from the installer's site); the [retry step](../moonwalk.md#migrate-wled) of the migration process. Verified (2026-11-11): the P4 in MoonBase installing MoonLight from its own page.

#### Every neighbor's MAC, by ARP 🚧 (missing: a Linux and a Windows run)

- **What ARP is.** On a local network a packet travels by MAC address, the IP address being the logical name on top. A device about to send to an address asks the network who holds it, the holder answers with its MAC, and the sender keeps the pair in its address table for minutes: the Address Resolution Protocol, [RFC 826](https://www.rfc-editor.org/rfc/rfc826). Every operating system keeps that table, and lwIP does on the ESP32.
- **What it gives the map.** After any exchange with a device on this subnet, its MAC is in this device's own table, so a device known only by its address, Tasmota or anything added by address, gets the identity a MoonLight board sends in its packet, without a firmware-specific API. Tasmota 16 refuses its status page to a request without its own page as Referer, a guard against cross-site requests, so ARP is the textbook source that leaves that guard alone.
- **Built (2026-11-10).** `platform::arpLookup` reads the stack's table: lwIP's under its core lock on the ESP32, `GetIpNetTable2` on Windows, `/proc/net/arp` on Linux, the routing table's link-layer entries on macOS, none on a Teensy, whose QNEthernet keeps its table private. macOS 27 answers that sysctl, the one arp(8) makes, with an empty table unless the binary is signed with an identifier, a behavior network scanners report ([NetScanner](https://github.com/Criseda/NetScanner/pull/51), [NetGuard](https://github.com/Lyosis/NetGuard/pull/46)); so the build signs the desktop binary as `org.moonmodules.moonlight`, the bundle's identifier, and the release signs every macOS binary with it. Local Network permission is not it. The address probe looks the MAC up after its connection and keys the row by it. Tested with a faked entry on the desktop; verified 2026-11-10 on the macOS desktop and the Olimex (ESP32): Tasmota's row shows c8:f0:9e:73:88:4c on both.
- **More uses**, each its own decision:
    - *A fallback for every row without a MAC*, not only added addresses: any device this one has talked to. Receiving a device's broadcasts alone does not always put it in the table.
    - *The vendor*: a MAC's first three bytes name its maker in the IEEE OUI registry, Espressif, Signify (Hue), Raspberry Pi, Apple, so a small table tells an ESP32 from a Pi or a bridge without asking it.
    - *Wake-on-LAN*, which needs the MAC: the map waking a sleeping PC or Pi that runs MoonLight.
    - *An active scan*: ask for every address of the subnet and list what answers, silent devices included, as `arp-scan` does; a burst of broadcast, so on demand.
    - *Changes spotted*: one MAC at a new address is a device that moved; a new MAC at a known address, another device that took it.
- **Limits.** Only the own subnet, a router answering for everything beyond it; entries expire in minutes; a MAC says who and who made it, never what it runs.

#### A security and privacy check of the map ✅

Landed in: [security](../explanation/security.md) (the trust boundary and the `Origin` and `Host` rule, the Devices map, secrets, firmware, what leaves the device); the rule's code in `http::fromBeyondNetwork` ([HttpHead.h](../../src/core/util/HttpHead.h)), used by the app and MoonBase; [MIGRATING](../reference/MIGRATING.md) ("A page from outside the local network is refused"). Verified (2026-11-11) on the S3 bench board, the access point's UI included. Steps 16 and 17 take the map beyond the network and get their own review there.

#### MoonDeck and the web installer reviewed against the map ⏳

The map now finds devices, reads their summary, installs firmware, moves other firmwares to MoonLight and applies a device model's defaults, which MoonDeck and the web installer each do in their own way. A review of both against it, looking first for what two of them do twice (the product owner, 2026-11-11). Candidates, each kept, merged or removed with its reason:

- **Finding devices.** MoonDeck's network bar and its registry in `moondeck.json` against Discovery's list, which any device serves on `/api/list`; MoonDeck could read the list from one device instead of keeping its own.
- **Device models.** MoonDeck's deviceModel push after each discovery (`PATCH /api/state`), the installer's `APPLY_OP` over serial and the map's Apply device defaults: one document, three transports, and whether each still earns its place.
- **Installing.** MoonDeck's network and USB flashing, the Firmware card, the installer and the map's Install all reach a release's firmware; the install picker is shared already, and the rest is checked for a second copy of release lookup, variant choice or progress reporting.
- **Board details.** The map's popup (`/api/system`), MoonDeck's device panels and the installer's device report read the same facts; one reader, or each its own reason.
- **Other firmwares.** The test script's WLED restore, the links to WLED's, Tasmota's and ESPHome's installers, and the map's moves: one place that knows each firmware's layout and update page.
- **Verified by:** the list of overlaps found, each with its decision, and the code and docs that one of each pair leaves behind removed.

#### MoonBase on every device ⏳ (after WLED to MoonLight from the map)

- Every layout carries MoonBase in a factory slot: recovery without a cable on every board, one update path, and on an 8 or 16 MB board the second app slot's flash going to the filesystem. Today the two-slot variants are the larger S3s (N8R8, N16R8) and the P4.
- Waits for the migration step: its power-cut rounds on each chip family are what MoonBase on that chip is trusted on.
- A device already on a two-slot layout cannot change its table through a network update, so it moves with the same migration image, uploaded through MoonLight's own update page: the program that migrates a WLED device migrates a two-slot MoonLight one. A MIGRATING entry.
- **Verified by:** a bench S3 N16R8 and the P4 moved from two slots to MoonBase over the network, then updated through MoonBase and recovered from a cut mid-update.

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

### Act as a group

#### 6. Groups ⏳

- A group module under Devices: a name, its members picked from the discovered list, and a role pinned to a member where the owner wants one; otherwise each role derives from the lowest live device id ([the message design](#the-message-design)). Membership is stored by the MAC-derived identity from step 4, not by name or address, so a rename or a new lease keeps the group ([device identity](../explanation/architecture/mooncore.md#device-name-one-identity-every-network-name-derives-from-it)).
- The map draws each group as a box around its members; a member not heard gets a "missing" mark.
- **Verified by:** two groups on the bench, a member unplugged and shown missing.

#### 7. Modules address a group, and the four peer lists go ⏳

- OscService, AudioService and NetworkSendDriver pick a group in place of a typed `hosts` list; MidiService's `host` is decided in the step. Their `hosts` text fields go, with a [MIGRATING](../reference/MIGRATING.md) entry, and OscService's cap of 8 with them. A group reaches its members by multicast where the network carries it, by unicast to each where it does not.
- Shared controls become group state: last-writer-wins registers, published by the device that wrote them, repeated by Trickle, never forwarded ([the message design](#the-message-design)).
- Who belongs together is then said in one place, and the map shows it.
- **Verified by:** shared controls and audio sync across a group, a member added and receiving without touching OSC or AudioService; two boards changing one control at the same moment end on the same value, and a unit test feeds the registers every arrival order.

#### 8. The group acts without a page open ⏳

- Any member puts its group into a preset or sets a control across it, published as group state over OSC, the state repeated so a member that missed it or rebooted catches up. Reading the members' summaries uses the same REST endpoints the page uses, a few at a time.
- **Verified by:** a preset change followed by every member, a member rebooted and caught up, packets dropped on purpose and the group still converging.

#### 9. Update a group 🚧 (missing: the whole group in waves; a peer without TLS updated from the map)

- From a device's page: update one peer, or the whole group, through each peer's `/api/firmware/url`, or by download and upload for a peer without TLS, the Firmware card's two paths reused.
- In waves: one device first, then a few at a time, stopping at the first failure, the device doing it last. A swarm finishes in minutes while a bad release stops at one device. Done means the peer is heard again with the new version.
- **Verified by:** a bench group updated from one board's page, a Teensy among them.
- **Built (2026-11-10), at the product owner's ask: one peer from its popup.** An "Update to …" button, pressed twice, posts the release image's address to the peer's `/api/firmware/url`, as its own Firmware card does, and watches `/api/system` until the peer answers on a new version; a peer without TLS (501) is sent to its own Firmware card, a desktop to the release page.

#### 10. Clock sync ⏳

- Absorbs the backlog card, with two changes: the reference is the clock reference role, not a configured leader; and a member measures its offset by the NTP exchange (four timestamps, request and reply), not a one-way send, since the round trip is what measures the delay. The same exchange then works over any transport, the LAN and step 16's relay alike. Aim: under a millisecond on the LAN.
- **A change applies at a time, not on arrival**: a preset or control change is stamped "apply at T", a little in the future, in an OSC bundle's timetag, which OscPacket now accepts. Members that receive it at different moments switch together, which is what lets latency, and step 16's internet, stay invisible.
- With step 7's shared controls, a group now shares both its controls and its time: the base every level of the next phase builds on.
- **Measured by:** the offset spread across a bench group; stop rule named before the first run.
- **Verified by:** a vertical line sweeping across boards side by side, the textbook display-sync test (genlock checks on video walls): straight across a seam when the clocks agree, a step when they do not, the step's width over the line's speed being the clock error.

### One effect across the group

#### 11. One effect streamed: correct for every effect ⏳

- The member holding the render role renders the whole group canvas and sends each member its region over DDP; the group sets up NetworkSendDriver and the members' Network Receive. Every level is correct this way, so it ships first and stays the fallback.
- Bounded by bandwidth and the LAN: a large canvas or MoonOrbit needs the levels of the next steps.
- **Verified by:** one effect running across three bench boards as one picture.

#### 12. Group canvas: position, time and shared input (levels 1 and 2) ⏳

- The group has one virtual size and each member holds its region in it; `elapsed()` reads the synced clock; `BeatPhase` is anchored to the effect's synced start, its rate changes applied at their synced time; `uv` takes the canvas. Every member renders its own region, and no pixels travel.
- A member lacking the effect (Raymarch on a chip without an FPU) gets it streamed, as step 11 does.
- The level declaration on EffectBase, the group's choice from it, and the split-equals-whole test; level 1 and 2 effects declared, each passing it.
- **The demo effect: a ball crossing the screens.** An elastic ball in closed form, x a triangle wave over the canvas width, y a parabola repeating each bounce, both functions of synced time: level 1, no state, each board drawing it while it is in its region. Regions placed at the boards' real positions, gaps included, so the ball takes the right time to fly between screens. BouncingBalls loses energy and relaunches, which carries memory; this is its elastic, sideways twin.
- **Verified by:** the ball crossing three bench boards with no jump at a seam; Plasma, Truchet and AudioSpectrum across them as one picture, the spectrum's bands split over the boards; a speed change mid-run keeping the boards in phase; a board started a minute late in phase from its first frame.

#### 13. Randomness from the group tick (level 3) ⏳

- A group tick from the synced clock; effects drawing per-frame randomness move from a `Random8` stream to `hashInt(seed, tick, light)`.
- **Verified by:** the split-equals-whole test for each converted effect; a sparkle effect on boards at different frame rates showing the same sparkles.

#### 14. Lockstep simulation (levels 4 and 5) ⏳

- A fixed timestep from the synced clock, in core, which a simulating effect steps by; integer math; the whole canvas simulated on each device.
- An effect whose memory forgets (Fire's cooling, a fade trail) converges by itself and needs nothing more. One whose memory keeps, and every level 5 one, publishes a state checksum every N steps as group state; a mismatch is repaired by a keyframe from the render role holder, and a late joiner joins by keyframe.
- Fire (forgets), Particles (keeps) and Game of Life (chaos) first, one of each case, the rest by the test.
- **Measured by:** how often a bench group diverges over an hour, and the keyframe's cost; stop rule named before the first run.
- **Verified by:** Game of Life across three boards as one board for an hour, a board rebooted and rejoining by keyframe.

#### 15. Live input across the group (level 6) ⏳

- A gamepad or touch on one member becomes group state, stamped "apply at T" a little ahead, so every member applies it at the same step: the delay-based lockstep of networked games. Where the delay shows, the effect stays streamed.
- **Pong across two boards**, a paddle on each and the ball crossing between them. Its attract mode re-rolls each paddle's aiming error per exchange; drawn as a hash of the exchange number, the rally is deterministic (levels 1 and 3), and a gamepad per board makes it level 6.
- **Verified by:** Pong played with a gamepad on each of two boards, the bench's Xbox Series pad and 8BitDo Zero 2, each paired with the computer showing its board's page ([the gamepad plan](Plan-20261001%20-%20Gamepads%20and%20MIDI%20desks%20(2%20of%207).md) owns how a pad reaches a board); Space Invaders from one board's gamepad across three.

### Beyond the LAN

#### 16. MoonOrbit: groups over the internet, through MoonCloud (stretch) ⏳

Devices in different places, owned by different people, in one group: a show running at several sites, or friends' installations running as one. MoonOrbit is a working name.

- **A relay, not a stream.** Only the clock and the changes travel: the controls, presets and "apply at T" stamps. Each device renders locally, so internet bandwidth stays small and latency hides behind step 10's scheduled changes. Pixels over the internet stay out of scope, so a group across sites runs effects at levels 1 to 6 and step 11's streaming stays within each site.
- **Outbound only.** A device behind a home router cannot accept a connection, so each member opens one outbound WebSocket to MoonCloud, and MoonCloud relays within the group. On Cloudflare that is one Durable Object per group, beside the existing Worker ([MoonCloud](../explanation/architecture/mooncloud.md)).
- **Opt-in and private**, as MoonCloud is today: a group is joined by a secret code the owner shares, the relay keeps no history, and the [privacy policy](../legal/privacy-policy.md) gains what the relay sees. Messages could be signed or encrypted with the group secret so the relay cannot read them; decided in the step.
- **Reach:** ESP32 boards and the desktop, which have TLS; a Teensy joins through an open page until it has TLS.
- **Measured by:** offset spread and change-to-light delay between two sites; stop rule named before the first run.
- **Verified by:** a bench board and a board at another address in lockstep, judged on a shared video call; a Pong rally between the two homes, one pad in each, paired with that home's own computer or phone.

#### 17. Remote support over MoonOrbit ⏳

A user with a problem shares their device, and a helper sees it and replicates the setup on their own desktop: "share your device in MoonOrbit and I will reproduce it". A support session is a group of two over step 16's relay, the helper reading the user's device as a page reads a peer on the LAN.

- **The user starts it.** A "share for support" button on the user's device shows a short code, valid for a limited time and ended by the user at any moment. The helper enters it in their own MoonLight, and the user's device appears on the helper's map.
- **Read-only by default.** The helper reads the summary, the state, the log and the flows through the relay's request/response. A change on the user's device needs the user to allow writing for that session.
- **Replicate on the desktop.** "Copy setup" loads the user's state into the helper's desktop instance, desktop first as every change is; a module needing hardware the desktop lacks (pins, a driver's peripheral) loads and idles, as it would on a board without that hardware. The copy is the same one the Backlog's "copy a device's settings" needs, so both share one export.
- **Secrets never travel.** Password controls (WiFi, MQTT, API keys) are left out of what is shared and copied; the [privacy policy](../legal/privacy-policy.md) names what a session shows.
- **Verified by:** a bench board at another address shared by code, its setup copied onto the desktop and showing the same effect, no password in the copy, and a write refused until the sharer allows it.

## Backlog

Seeing the network:

- **More discovery**: ArtPoll replies (every Art-Net node, any brand, and the card's dead-receiver skip), SSDP on port 1900 (Hue, Sonos, many IoT devices), ESPHome, Tasmota; a passive listen to mDNS announcements, a hypothesis to test against the pool exhaustion that ended mDNS queries.
- **Conflict detection**: duplicate names, two senders on one universe, two audio sync senders, a configured receiver that is silent.
- **Link quality per peer**, from the per-sender frame counts.
- **Devices placed on a floor plan** on the map, instead of the force layout.
- **A map of thousands**: Barnes-Hut in the force layout, O(N log N), once a swarm makes the plain layout slow.
- **The desktop advertising mDNS** (the platform card), so a desktop instance is findable by name.

Acting on devices and groups:

- **Signed releases, every device**: a release signature (minisign or Ed25519, the public key compiled in) checked before any install, board or desktop, as Sparkle and apt check theirs; the SHA-256 proves integrity, the signature would prove origin beyond TLS to GitHub. Asked 2026-11-13, deferred by the product owner.
- **Hue paired once**, the bridge identity moving from each HueDriver into Devices.
- **The command half for foreign devices** (`DevicePlugin::command()`): WLED brightness, Hue, Tasmota.
- **Copy a device's settings to the group**, and a backup of the whole group, on the export step 17 builds.
- **The user's preview in a support session**, the preview stream over the relay at a coarse rung, so the helper also sees the lights.
- **Scheduled group updates** run by a member with no page open.
- **Non-IP transports**: MQTT-only devices, Zigbee and Thread.

Effects and games across a group:

- **MoonLive scripts declare their level by inference**: the compiler sees whether a script keeps globals across frames (memory) or calls a random builtin (level 3), so a script needs no declaration of its own.
- **Turn-based games over MoonOrbit**, delay not mattering and a game's state a few dozen bytes. A game is a list of moves and only the player on turn writes move n, so moves never conflict; the board and move number repeat as group state, so a rebooted board is back in the game within a second. Four in a row first (7×6, rules of ~50 lines, a falling disc; under a name of its own, Connect Four being Hasbro's), then Reversi and chess on 8×8, chess's move checking in the browser rather than the firmware. Beside them a shared pixel canvas, every pixel a last-writer-wins register, the purest demo of the leaderless design. Input to pick a square: a gamepad cursor, a tap on the web preview, touch, or an 8×8 MIDI pad grid.

## Cards this plan absorbs

The interop plugins and their command half live in the Backlog above, and the shared clock is step 10.

Related cards that stay on [the Project](https://github.com/users/MoonModules/projects/5):

- *ArtPoll discovery: know which tubes are alive*, a NetworkSendDriver card, its discovery half named in the Backlog.
- *The desktop build advertises no mDNS*, a platform card, named in the Backlog.
- *Read a vehicle's CAN bus and drive lights from it*: its "MoonSync" convoy note is this plan's groups and clock sync, over MoonOrbit when each car is on its own mobile data.

## What it removes

Devices as a wired child of Network; the device list's 32-row cap, the list seam's 255-row cap and OscService's 8 hosts; PictureService's inline reserve check, moved to core; the four typed peer lists, replaced by a group; typing a peer's address where discovery already knows it; a bespoke group protocol; acknowledgement and retry machinery, which state repeated with versions makes unnecessary; a leader election, which derived roles make unnecessary; and, from the Backlog, the per-driver Hue pairing.
