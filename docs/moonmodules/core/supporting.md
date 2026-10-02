# Core supporting

The shared building blocks under `src/core/util/`: small, domain-neutral pieces every module leans on. Nothing here renders a card or owns a control. Each row links to its generated technical page, built from the header's own `///` comments.

The rule that puts a header here rather than in the light domain is the dependency direction. A light driver's `pins` control and the core pin-ownership map read the same comma-separated list, so the parser lives in core. The dependency then runs domain to core, never the reverse.

A row without a link carries free functions and constants rather than a class, which the generator renders no page for. The header itself is the reference until that is fixed.

## Controls and modules

| Header | What it is |
|---|---|
| [`ControlSurface`](moxygen/ControlSurface.md) | A transport that mirrors the control surface's state in both directions. The module owns the state and a surface is a view of it, which is what lets two attach at once. |
| [`InputMapping`](moxygen/InputMapping.md) | The binding table behind a surface. A row says what an input does to a control: set it, step it, toggle it. A delta steps down as well as up, so one encoder covers a range without a second control to reverse it. |
| [`ModuleFactory`](moxygen/ModuleFactory.md) | The registry mapping a type name to the function that builds one, which is what lets a preset file name a module this build has never instantiated. Storage grows on demand rather than reserving a fixed cap. |
| [`ActiveInstance`](moxygen/ActiveInstance.md) | The one-active-instance election. Several instances of a type may exist, and exactly one is the one a consumer reaches. |

## Reading and writing JSON

| Header | What it is |
|---|---|
| [`JsonUtil`](moxygen/JsonUtil.md) | Two layers: flat helpers that scan for a key over the subset we emit, and a recursive reader for the cases that must descend. Both are off the hot path, so bounded stack use is fine. |
| [`JsonSink`](moxygen/JsonSink.md) | The writing half, formatting straight into a bounded buffer. |

A flat parser returns zero for a missing key, which is indistinguishable from a real zero. A load path pairs it with a presence check before treating the value as authoritative, or an older file that omits a key silently clobbers a non-zero default.

## Memory and threading

| Header | What it is |
|---|---|
| [`ScratchBuffer`](moxygen/ScratchBuffer.md) | One owned allocation with a typed view, so a module sizes its working memory once rather than per frame. The heavy body compiles once rather than per element type. |
| [`SpscRing`](moxygen/SpscRing.md) | The textbook Lamport queue: one thread pushes, one pops, and each index is written by exactly one side. The release and acquire pair is what publishes the element data. |
| [`TryLock`](moxygen/TryLock.md) | A non-blocking latch for a resource two threads reach. It never waits, so the render core is never held by a slower caller. |
| `Sort.h` | In-place insertion sort over a fixed array, for the small bounded collections the system holds. Every module sorts the same way and supplies only its comparator. |

## Wire formats and parsing

| Header | What it is |
|---|---|
| [`ImprovFrame`](moxygen/ImprovFrame.md) | The serial provisioning protocol's framing, with no platform dependencies, so the host tests it without hardware. |
| [`ImprovOpReassembler`](moxygen/ImprovOpReassembler.md) | The state machine that reassembles a chunked operation into one buffer, with the duplicate and out-of-order guard. The platform layer owns the serial side around it. |
| [`FirmwareImage`](moxygen/FirmwareImage.md) | Reads an image's header without the vendor framework. The layout is a fixed on-disk format, so the same bytes parse on a device and in a test. |
| [`PinList`](moxygen/util_PinList.md) · [`HostList`](moxygen/HostList.md) | The two list parsers a board configuration needs: a pin list and a host list of addresses and names, both typed by a human into one text control. |
| [`Addressing`](moxygen/Addressing.md) · [`HostResolver`](moxygen/HostResolver.md) | One rule for sending to many receivers, unicast, multicast or broadcast, and the cache that turns a host name into an address off the render thread. |
| [`sha256`](moxygen/sha256.md) · [`crc`](moxygen/crc.md) | A cryptographic digest for update verification and identity, and a cheap checksum used as a fingerprint of a block of state. |
| [`Sha1`](moxygen/Sha1.md) · [`Base64`](moxygen/Base64.md) | The two codecs the WebSocket handshake needs, the digest that standard mandates and the encoding that carries it. |

## Crossing the domain boundary

| Header | What it is |
|---|---|
| [`LightSummary`](moxygen/LightSummary.md) | A plain-data summary of the light pipeline's output, so the network shims report the real device shape without including anything from the light domain. |
| [`AudioFrame`](moxygen/AudioFrame.md) | One snapshot of analysed audio, produced once per render tick and consumed by audio-reactive effects. |
| [`BinaryBroadcaster`](moxygen/BinaryBroadcaster.md) | The sink a producer holds instead of the concrete server, so a driver sends to every client without depending on the web server. |
| `build_info.h` | The version and build identity, generated from the project manifest rather than edited by hand. |
