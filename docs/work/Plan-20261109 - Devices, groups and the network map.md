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
- **Identity.** The presence packet has no MAC (its 44 bytes are WLED's layout), while ArtPollReply has one and SSDP a uuid. A MoonLight member's identity comes from its summary (step 4).
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

- **Grown in place**, not beside it (product owner, 2026-11-09): the existing DevicesModule moves and grows. A second module next to it would be a second listener on the presence port and a migration later, two mechanisms for one job.
- Devices moves from a wired child of Network to a top-level container, the discovery list its first child. Its doc moves from [system.md](../moonmodules/core/system.md) to a page of its own.
- A list drawn as a graph: a row is a node keyed by `ip` or else `name`, colored by `type`, ringed when `self`, faded by its detail's age, linked by an `edges` array of `{to, label}`. The discovery list declares it; the UI draws it above the list, this device fixed at the center, a click opening a device's page.
- The device list loses its 32-row cap and the list seam its 255-row one; persistence and the state document stream a long list as they stream a long control set.
- **The allocation rule says what it protects**, the product owner's decision (2026-11-09). [moonmodule.md](../explanation/architecture/moonmodule.md) says "grow on demand from the heap during `setup()`" (dynamic over fixed-size) and "Allocate everything during `setup()` / `prepare()`" (hot path discipline), narrower than the code, which already allocates on an edit. Both become one rule: **allocate on a change, never per frame.** Structures grow from the heap when the structure changes, at setup, at prepare, at an edit, at a new member; by doubling in one contiguous block, so N items cost about log₂ N allocations and fragmentation stays bounded; every growth checks the reserve first and degrades when it does not fit; the frame only reads and writes what is allocated. The device list is its first user.
- The reserve check gets one home in core, a function both the device list and PictureService call, which does the same check inline today ([PictureService.h:399](../../src/core/services/PictureService.h#L399)). The device list's degradation (pinned rows, least recently used replaced, the card's "holding N of M") lands here.
- The ArtPollReply names become the device's own name ([what the network carries](#what-the-network-carries)).
- No edges yet: this step is the map of who is there.
- **Verified by:** the bench boards, a WLED and the desktop on one map; a board unplugged fades and ages out. Unit tests: a thousand synthetic devices all held; with growth refused, a new MoonLight device replaces the oldest foreign one, a pinned row is never replaced, and the card says memory full.
- **Built (2026-11-09): the allocation rule, the caps, the move.** The rule in [moonmodule.md](../explanation/architecture/moonmodule.md), its one home; `platform::fitsWithReserve` in [platform.h](../../src/platform/platform.h), which PictureService calls in place of its own check; `ScratchBuffer::ensure` growing a block by doubling and keeping what it holds ([ScratchBuffer.h](../../src/core/util/ScratchBuffer.h)). `ListRow`, the list seam's row index, from `uint8_t` to `uint16_t`, the streaming cursor's width ([Control.h](../../src/core/module/Control.h)). DevicesModule's rows on the heap, no fixed size, freed while disabled; with growth refused, a newcomer replaces the least recently heard row of the lowest worth (other devices, then MoonLight boards, this device never) and the status says "memory full". The `Devices` container at the top level ([Devices.h](../../src/core/system/Devices.h)), DevicesModule wired into it as Discovery, its page `core/devices.md`, with a [MIGRATING](../reference/MIGRATING.md) entry. Tests: the growth and degradation cases ([unit_DevicesModule_growth.cpp](../../test/unit/core/unit_DevicesModule_growth.cpp)), `ensure` keeping its elements and refused by memory ([unit_ScratchBuffer.cpp](../../test/unit/core/unit_ScratchBuffer.cpp)), and the discovery scenario reading Discovery under Devices; 3145 unit tests pass. Discovery's object went from 1,584 to 368 bytes, its rows now taken only as devices are heard.
- **Built (2026-11-09): the map and the ArtPoll names.** `ListSource::listAsGraph()` ([Control.h](../../src/core/module/Control.h)), sent as `"graph":true`, and Discovery declaring it; `buildListGraph` and `layoutGraph` in [app.js](../../src/ui/app.js), the styles in [style.css](../../src/ui/style.css), drawing the `edges` step 2 adds with no UI change. An ArtPoll reply names the node after the device, read from Discovery, which holds the name SystemModule owns, "MoonLight" while Discovery is off (`NetworkReceiveEffect::nodeName`). Tests: the node name, following a rename and falling back; 3146 unit tests and 368 JS tests pass.

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

#### 4. A peer's summary, and identify 🧪 (test: a bench board found by its blink, picked from the map)

- A compact summary per device, one small JSON document: identity (MAC-derived), version, firmware variant, board, uptime, FPS, free heap, RSSI, the running effect and the effects it has. The textbook health endpoint, cheap to poll where `/api/state` is large.
- Shown when a node or row is selected; a peer behind the release gets an "outdated" badge.
- **Identify**: a button that blinks a device's lights for a few seconds, on every device, pressed remotely through the peer's own `/api/control`. That is the command half for MoonLight peers, with no new protocol.
- **Verified by:** finding a board on the bench by its blink, picked from the map.
- **Built (2026-11-10), at the product owner's ask: the map in place of the list, a node's popup, and its firmware.** The graph replaces the rows; a click on a node opens its detail in a popup ([app.js](../../src/ui/app.js) `openGraphNode`). `/api/system`, the existing health endpoint, carries `version`, `build` and `firmware` first, and the popup of a MoonLight device reads them, from `/api/state` where older firmware lacks them, and says whether a newer release fits it, by the same checks the update badge makes for this device (`stableUpdate`, `devUpdate`). Verified against the bench: 6.1.0-dev on .203 and .101, 7.1.0-dev on .221, the latest release 7.1.0-dev.81. A node shows what it offers as emoji (`GRAPH_PROTOCOL_EMOJI` in [app.js](../../src/ui/app.js)): 🌐 HTTP, 🎛️ OSC, 🎹 RTP-MIDI, 🔊 audio sync, 🎭 Art-Net, 🎚️ E1.31, 📦 DDP, 🧱 ColorLight; a service it does not name shows its protocol, and the tooltip and popup keep the ports.
- **Built (2026-11-10): the summary and identify.** `/api/system` leads, after the firmware, with `mac`, `board` (the deviceModel tooling sets), `chip`, `rssi` (0 off WiFi), `outputs` and `effects` (`writeDeviceSummary` in [HttpServerModule.cpp](../../src/core/system/HttpServerModule.cpp)); uptime, FPS and free heap it already carried. The outputs and effects come through `LightOutput` ([LightOutput.h](../../src/core/util/LightOutput.h)), `writeOutputs` and `writeEffects`, which Drivers implements: each enabled effect on an enabled layer, by name. The effects a device has are its type catalog, `/api/types`, read where needed rather than repeated in the summary. The popup lists board, MAC, health, lights and effects (`peerSummaryLines`). **Identify** is a button on Drivers: five seconds of the lights dark and lit twice a second through the brightness tables, at their own brightness and never brighter, so it browns out nothing and writes into no effect's buffer; the popup presses it through the peer's `/api/control`, shown where the summary carries `outputs`, which older firmware lacks. A black effect blinks invisibly. Tests: the blink's halves and its end, the summary's members with and without a light output.
- **CodeRabbit on PR #242 (2026-11-10).** Graph nodes are buttons, reached by Tab and pressed by Enter or Space, the SVG a group rather than one image; two `Content-Length` lines that disagree refuse the request (RFC 9112 § 6.3); the screenshot script records the nav roots, Devices among them; the flows tests require a row before asserting what it lacks.

#### 5. What drives each device's lights 🧪 (test: the bench boards' outputs on the map, each as its card names it)

- Each device's summary lists the light outputs it runs: a driver's kind (HUB75, HDMI, a PanelCard, parallel LED pins, Art-Net out, a Hue bridge) and how many lights it drives, from the Drivers container it already has.
- The map shows them on each node, a small label under its name, and the popup lists them with their light counts, so a wall is read at a glance: .127 a HUB75 panel, .221 an HDMI screen, .156 a PanelCard.
- **Verified by:** the bench boards' outputs on the map, each as its card names it.
- **Built (2026-11-10).** `writeOutputs` lists each enabled driver that lights something, the preview excepted (`DriverBase::drivesLights`), with the lights its window sends (`windowLength`, the test-only resolver made API). This device's Discovery row carries them as `outputs`, so a peer's map labels its node from the list it already reads; the label sits under the name as the kind of light and its count ("📺 4096  💡 300", `GRAPH_OUTPUT_EMOJI` in [app.js](../../src/ui/app.js), at the product owner's ask), 🎯 for moving heads whatever drives them (`movingHeads`, from the fixture profile's motion), the popup adding each driver's name. The map's cells grow a line to hold it. Edge labels are placed greedily along their edge, from the middle outward, clear of the nodes' labels and of each other, the spot covering least where none is free; the picture has side padding, so a long label at an outer cell ends inside it rather than being cut. MoonLight nodes are purple and WLED orange, the product owner's call. With Home Assistant linked to every board the twelve linked nodes were pinned to the top edge in one row: the force area now grows with its nodes, room for each cell twice over, and node overlap removal pushes apart two cells that still overlap along the axis that overlaps least. Toggles above the map show a node's IP or version, both off by default, and hide the lines' labels, on by default, kept per browser; its lights and services, the emoji, always show. The map draws in real pixels at its box's size, a wider or taller box giving the nodes room rather than larger text, a resized map carrying its nodes along in proportion; its box is the first expandable area of the module panel's grips (`panel.js`, at the product owner's ask, generic for every module: a left edge, a bottom edge and a bottom-left corner, the height shared among a page's expandable areas in proportion to their natural heights). The edges are drawn as a macOS window's, the cursor alone; the panel shrinks too, an area to 80 px, the map zooming out to fit. The map zooms and pans as a map and the preview do: the scroll wheel or a pinch at the pointer, a background drag, a double-click or ⌖ in its bar, the preview's reset glyph, for the whole picture. A line's label shows its protocol as an emoji, from one protocol table with the services (`GRAPH_PROTOCOL_EMOJI`), 🐙 the WLED API Home Assistant polls; rates read "/s" and "/m" at the source; AudioSync names each format's group as it reports it, "MoonLight audio" for 239.0.0.2 and "WLED audio" for 239.0.0.1, the address under the address toggle; each output shows its frames a second, "📡 16384 @ 45/s", its own count where it keeps one (`DriverBase::framesPerSecond`, Network Send's) or the device's frame rate. **A list's detail rides the state patch** (found here: an open page kept the map's lines, services and lights from when it connected until a reload): the patch leaf of a list is its rows and, beside them, their detail, so the hash covers both (`writeListDetail`, one writer for the full state and the patch); a map already drawn then redraws its picture in place each second, its box keeping its height. Also fixed: the AudioService sync test read its second packet once instead of polling, which a loaded run could find empty. Tests: the outputs, the preview and a disabled driver left out, a window counted; the node's lines in order; two labels that met at their middles placed apart.

### Before groups

Planned by the product owner (2026-11-10) after step 5 and before step 6, each shipping on its own.

#### Every Hue bridge on the map ⏳

- Not only a bridge a HueDriver points at: Hue's own discovery, mDNS `_hue._tcp`, through a passive listen to mDNS announcements, or SSDP on port 1900. With the rest of the Backlog's More discovery, since they share the mechanism.
- It needs a hardware test: mDNS queries were ended by a pool exhaustion on the ESP32, so the passive listen is a hypothesis until a board runs it.
- **Verified by:** a Hue bridge nobody configured on the bench map.

#### Update a Linux box from the map ⏳

- One install path that updates itself. The `.deb` stays the install: apt checks the architecture and pulls the libraries, and the package creates a `moonlight` service user and the systemd unit. It ships the binary as a seed copy dpkg tracks, which its post-install script copies to `/var/lib/moonlight/bin`, owned by the service user, when newer than what is there.
- `POST /api/firmware/url` on Linux downloads the release's `MoonLight-linux-<arch>-v….tar.gz`, checks GitHub's SHA-256 digest, keeps the running binary as `MoonLight.prev`, renames the new one in and exits for systemd's `Restart=`; a binary that fails its first boots (the boot record) puts `.prev` back. No sudo rule and no Docker socket, so the network-facing service never holds root.
- A container is its host's to update, through Watchtower or Podman's auto-update, `:latest` following the rolling prerelease; the app detects it runs in one and the map says so instead of offering the button.
- Costs a MIGRATING entry: the Pi's hand-written unit gives way to the packaged one. Unblocks step 9 for a group that mixes boards and Linux boxes.
- **Verified by:** .204's package updated from the map, and rolled back from a binary made to fail; .156's container named as its host's.

#### WLED to MoonLight from the map 🚧 (missing: the map's Update to MoonLight button, the Ethernet warning and mapping, the S3 and P4 images)

- **One migration image per chip family**, the same program built for each chip MoonLight builds for and WLED runs on: the classic ESP32 first, which most WLED devices run on, then the ESP32-S3, the ESP32-C3 (verified today), the ESP32-P4, and any chip added later by the same rule. Never one per firmware: flash size, PSRAM and Ethernet are what the image reads, and MoonBase installs the matching firmware. CI builds the images in the firmware's matrix; the release and the Pages mirror carry them as `shared-migrate-<chip>.bin`.
- **Always landing on MoonBase**, one install path: the image writes MoonBase into the factory slot where the target layout has one, and otherwise into the second app slot, from where MoonBase installs MoonLight into the first and MoonLight's first network update takes the second back. MoonBase gains installing into the other app slot when it runs from one rather than from factory. Funkelfetisch's S3 migration (GPL-3.0, [Funkelfetisch/projectMM](https://github.com/Funkelfetisch/projectMM), credited in the commit and the migration doc) is read for its S3 parts, saving WLED's networks, the table rewrite and resuming after a cut; theirs takes a signed image from a phone over BLE, ours lands on MoonBase over WiFi.
- **The map's button.** On a WLED node the popup reads its `/json/info` for the chip and offers **Update to MoonLight** where an image exists for it ([migrating over the air](../reference/migrating-over-the-air.md)), else names the chip with no image yet. A press asks for a second, the risk spelled out: a power cut in the 50 ms partition-table write needs a cable to recover. The browser takes the image from the Pages mirror, `moonmodules.org/MoonLight/install/releases/<tag>/`, which serves it cross-origin where GitHub's release download does not, and uploads it to WLED's own `/update`; WLED's JSON API and `/update` answer cross-origin. The browser cannot read WLED's reply, so the map confirms by the device coming back as MoonBase, then as MoonLight on its node; still WLED after about 30 seconds, the popup names the likely cause: the release-name check a newer WLED makes until its option to skip it is ticked, an update lock or password.
- **Every WLED network carried.** A WLED device the map sees is on our network, so the network it uses is among the ones WLED saved (up to three in WLED 16, `nw.ins` in its `cfg.json` and `wsec.json`), or it is on Ethernet. The migration copies all of them into MoonLight's list of known networks, which MoonBase tries in order, so the one we are on is always among them; today it copies only the first, and a device on its second network comes back as an `MM-XXXX` access point. A static IP is not carried: MoonBase asks DHCP, and the map finds the device by its presence.
- **Ethernet.** The popup reads from WLED's `/json/info` whether the device is on Ethernet and warns before migrating that it comes back on its saved WiFi, or else as an access point. Then a mapping from WLED's Ethernet board types to MoonLight's Ethernet settings, written by the migration, so MoonBase comes up wired.
- **A board in MoonBase on the map.** MoonBase sends the presence packet MoonLight sends, marked as MoonBase, so a board keeps its node while it updates, after a migration or after a failed app, where today it drops off the map. The node shows a MoonBase badge (🛟) in place of its lights and services, and its popup offers **Install MoonLight**: the release firmware for its chip and layout, sent to MoonBase's `POST /api/firmware/url`. The WLED button's "back as MoonBase" is that badge appearing, and a migration finishes from the map. WLED 16 refuses an upload without its compatibility metadata unless the form carries `skipValidation=1`, its update page's checkbox, which the button sends.
- Each chip's image passes `wled_migration_test.py`'s power-cut rounds before it ships, and extends [What can move](../reference/migrating-over-the-air.md#what-can-move).
- **Verified by:** a WLED classic ESP32 controller and a WLED C3 bulb moved from the map, then a WLED S3 controller; a chip with no image yet named as such.
- **Built (2026-11-10): the classic ESP32 image, every network, MoonBase on the map.** The `esp32` variant builds the migration image (`"migrate": True`), published as `shared-migrate-esp32.bin`; `wled_migration_test.py` knows the classic (WLED 0.15.0's image, its ESP-IDF 3.3 bootloader at 0x1000, 40 MHz flash, 460800 baud for a USB-serial bridge) and passed 7 of 7 power-cut rounds on the Olimex (2026-11-10). The networks are read by `WledNetworks.h`, shared with a desktop test (WLED 0.15's one, WLED 16's three). MoonBase announces itself with the `MB` sentinel (`WledPacket::kMbMarker`), Discovery lists it as kind `MoonBase` (`MbPlugin`), the latest marked packet deciding MoonLight or MoonBase and an unmarked one never downgrading either; MoonBase answers `GET /api/system` and every reply carries `Access-Control-Allow-Origin: *`; the map shows 🛟 and **Install MoonLight**, the newest release's firmware for the chip, the variant the app ran preselected. Tests: the reader, the sentinel, a board turning MoonBase and back, the chip's firmware filter.

#### ESPHome, Tasmota and others to MoonLight from the map ⏳ (after WLED to MoonLight from the map)

Researched 2026-11-10 from each project's source; ranked by how often a MoonLight user meets them: WLED, WLED-MM, ESPHome, Tasmota, ESPixelStick, Shelly, ESPEasy, OpenMQTTGateway.

- **Overlap measured by bytes, not by slot.** The migration image decides whether it can finish, and whether it must move aside, on the running image's own length (`esp_image_get_metadata`) against the bytes it writes (MoonBase at the factory offset, the filesystem, otadata), not on partition entries. That admits ESPHome's second slot at 0x1D0000, Tasmota's single app slot at 0xE0000 and ESPEasy's and ESPixelStick's at 0x1E0000, whose slots overlap MoonLight's regions on paper while the image's bytes do not. A build check keeps MoonBase under 0xD0000 (832 KB), where Tasmota's app slot starts.
- **WLED-MM:** WLED's path as it is, with no release-name check; its 8 and 16 MB tables add their slot offsets.
- **ESPHome.** Found by mDNS: once a minute Discovery sends one browse for `_esphomelib._tcp` from an ordinary port, a legacy unicast query (RFC 6762 § 6.7) the responder answers directly, so nothing binds 5353 where the platform's own responder listens (`Mdns.h`). The reply names it, its version and its board (built 2026-11-10, on a bench board running ESPHome Web 26.9.2). A device with `web_server:` takes the image on `/update` as WLED does, and from ESPHome 2026.11 only from an allowed origin, which the popup asks the user to add. The default device has no HTTP server, only native OTA on TCP 3232 (magic bytes, SHA-256 password, the image's MD5), which a browser cannot speak; MoonLight's device runs that client, for no password or one the user types, and a device requiring `ota: encryption:` is named as moving by cable. ESPHome compiles its WiFi into the firmware, so the user types the network into the popup, which writes it into the image before the upload, its checksum and SHA-256 recomputed; a network the image reads from the old firmware wins.
- **Tasmota.** It announces nothing by default, so a user adds it by its IP (the product owner, 2026-11-10: no subnet scan), in Discovery's `addedIps`. Discovery asks each added address for `/` once a minute, one at a time, a connect and a read moving one step a tick, and reads its `Server:` header, `Tasmota/16.0.0 (ESP32-D0WD-V3)`, for kind and version, and its first `<h2>`, the name the Tasmota was given (built 2026-11-10, on a bench board running Tasmota 16.0.0; the saved list is parsed at setup too, since a restored value takes no change hook). A WLED's version the map reads once from its `/json/info`, served cross-origin, when the version toggle is on. Its referer check refuses every request from another page, so the popup asks the user to enter `SetOption128 1` in Tasmota's console once, Tasmota's documented setting. Then the two steps Tasmota's own page runs: `/u4?u4=fct&api=` until it answers `true` (safeboot holds the board), then the image to `/u2?fsz=<bytes>`, field `u2`. The image reads the WiFi from NVS `main`/`Settings`, the text pool's entries 4 and 6.
- **The rest:** ESPEasy on `/update`, its WiFi in `security.dat`; ESPixelStick with the image wrapped in its EFU container; OpenMQTTGateway pulling the image from MoonLight's device through `/up?o=`. Shelly keeps its partition table at 0x10000 on Gen3 and Gen4 and takes only its own ZIP package, so it is a track of its own; Tuya's ESP32 modules update only from Tuya's cloud.
- **Verified by:** a unit test of the overlap arithmetic on the ESPHome, Tasmota, ESPEasy and WLED tables, one of the espota2 handshake against a recorded session, and one of Tasmota's text-pool walk on a captured settings blob per chip; a bench ESPHome and a bench Tasmota board on USB, each migrated from the map with the power-cut rounds.

#### MoonBase on every device ⏳ (after WLED to MoonLight from the map)

- Every layout carries MoonBase in a factory slot: recovery without a cable on every board, one update path, and on an 8 or 16 MB board the second app slot's flash going to the filesystem. Today the two-slot variants are the larger S3s (N8R8, N16R8) and the P4.
- Waits for the migration step: its power-cut rounds on each chip family are what MoonBase on that chip is trusted on.
- A device already on a two-slot layout cannot change its table through a network update, so it moves with the same migration image, uploaded through MoonLight's own update page: the program that migrates a WLED device migrates a two-slot MoonLight one. A MIGRATING entry.
- **Verified by:** a bench S3 N16R8 and the P4 moved from two slots to MoonBase over the network, then updated through MoonBase and recovered from a cut mid-update.

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
