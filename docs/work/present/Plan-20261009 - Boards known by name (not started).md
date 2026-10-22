# Plan: Boards known by name

A setting that points at another board, such as the MIDI service's `host`, takes an address or a name, and a card that reports a peer shows its address.
An address says nothing about which board it is, and it changes when the network changes: a new router hands out new addresses.
A name survives that, but it resolves through mDNS, which is multicast, the traffic a router can drop.
This plan resolves a board's name from MoonLight's own discovery and shows names where addresses appear, while the address stays what travels on the wire.

## What exists

- The Devices module discovers every MoonLight board on the network, by name and address, from presence packets sent on a multicast group and on the broadcast address. Broadcast reaches every WiFi station, so this list holds even on a router that filters multicast.
- The MIDI card names its peer ("shared with MM-StadBeest") because RTP-MIDI's handshake carries a name. That is the protocol's own name, not a general mechanism.
- Every other peer is an address: the Audio card says "receiving from 192.168.1.125".
- The host settings of MIDI, Audio, OSC and Network Send take names through `HostResolver`: a `.local` name by mDNS, anything else by DNS, in the background.
- On the StadBeest (2026-10-09), one setting holds an address: the legs' `Midi.host` = 192.168.1.101, the bridge. OSC and audio use multicast and need none.

## Steps

### 1. One lookup, in core

The Devices module answers the name for an address and the address for a name, from the list it already keeps.
It is the one home of that mapping; nothing else resolves names.

About 40 lines and two tests: an hour.

### 2. A peer is shown by name

A status that names a peer uses the lookup: "receiving from MM-S31 (192.168.1.125)".
A peer the list does not know, a desk or a computer, keeps its address alone.

About 20 lines across the cards that name a peer (Audio, Network Receive, MIDI), plus a test: an hour.

### 3. An address setting shows the name

A text control that holds an address or a list of addresses says so when it is declared, so the UI knows without guessing from the value.
The UI then shows the known name next to each address, the same way on every card.

A control flag carried in the module's JSON, the Devices list reaching the UI, and one rendering rule in `app.js` with its JS test: about 100 lines, half a day.

### 4. A board's name resolves through discovery

`HostResolver` asks the lookup first and falls back to mDNS and DNS, so a MoonLight board's name resolves from the Devices list, which broadcast keeps current even on a router that drops multicast.
Every host setting gets it at once, since they all resolve through `HostResolver`.

About 30 lines and a test: two hours.

### 5. Tests and docs

- Unit tests: the lookup both ways, a status naming a known and an unknown peer, a board name resolving through the Devices list before mDNS.
- The multi-board how-to gets an addressing section: reservations on the installation's own router, or names in the host settings.

The tests come with each step above; the docs are an hour.

## Estimate

| Step | Size | Work |
|---|---|---|
| 1. Lookup | ~40 lines | 1 hour |
| 2. Peer by name | ~20 lines | 1 hour |
| 3. Name next to an address | ~100 lines | half a day |
| 4. Name through discovery | ~30 lines | 2 hours |
| 5. Docs | one section | 1 hour |
| **Total** | **~190 lines** | **about 1.5 days**, plus an hour of your testing on the StadBeest |

Stopping after step 3 is about 1 day.

## Decisions

- Steps 1 to 3 show names where addresses appear; step 4 makes a name in a host setting hold on a router that drops multicast. Do all four, or 1 and 4 first?
- A control declares that it holds an address (step 3) rather than the UI matching any value that looks like one, since a value that merely looks like an address is not always one.

## What it removes

Nothing in code: the MIDI card keeps the protocol's name, which is authoritative for that link.
For a user it removes the need to keep addresses fixed, once step 4 is in.
