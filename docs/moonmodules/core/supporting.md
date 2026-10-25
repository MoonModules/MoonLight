# Core supporting

The shared building blocks under `src/core/util/`: small, domain-neutral pieces every module leans on. Nothing here renders a card or owns a control. Each row links to its header, whose `///` comments are the technical reference.

The rule that puts a header here rather than in the light domain is the dependency direction. A light driver's `pins` control and the core pin-ownership map read the same comma-separated list, so the parser lives in core. The dependency then runs domain to core, never the reverse.

A row without a link carries free functions and constants rather than a class, which the generator renders no page for. The header itself is the reference until that is fixed.

## Controls and modules

| Header | What it is |
|---|---|
| [`ControlSurface`](../../../src/core/util/ControlSurface.h) | A transport that mirrors the control surface's state in both directions. The module owns the state and a surface is a view of it, which is what lets two attach at once. |
| [`InputMapping`](../../../src/core/util/InputMapping.h) | The binding table behind a surface. A row says what an input does to a control: set it, step it, toggle it. A delta steps down as well as up, so one encoder covers a range without a second control to reverse it. |
| [`ModuleFactory`](../../../src/core/util/ModuleFactory.h) | The registry mapping a type name to the function that builds one, which is what lets a preset file name a module this build has never instantiated. Storage grows on demand rather than reserving a fixed cap. |
| [`MoonLiveScript`](../../../src/core/moonlive/MoonLiveScript.h) · [`MoonLiveScriptFile`](../../../src/core/moonlive/MoonLiveScriptFile.h) · [`script_catalog`](../../../src/core/moonlive/script_catalog.h) | One scripted module's script: the file it names, the compiled program and the content hash that decides when to recompile, shared by every scripted effect, layout, modifier, palette and service; and the factory scripts a device knows. |
| [`ActiveInstance`](../../../src/core/util/ActiveInstance.h) | The one-active-instance election. Several instances of a type may exist, and exactly one is the one a consumer reaches. |

## Reading and writing JSON

| Header | What it is |
|---|---|
| [`JsonUtil`](../../../src/core/util/JsonUtil.h) | Two layers: flat helpers that scan for a key over the subset we emit, and a recursive reader for the cases that must descend. Both are off the hot path, so bounded stack use is fine. |
| [`JsonSink`](../../../src/core/util/JsonSink.h) | The writing half, formatting straight into a bounded buffer. |

A flat parser returns zero for a missing key, which is indistinguishable from a real zero. A load path pairs it with a presence check before treating the value as authoritative, or an older file that omits a key silently clobbers a non-zero default.

## Memory and threading

| Header | What it is |
|---|---|
| [`ScratchBuffer`](../../../src/core/util/ScratchBuffer.h) | One owned allocation with a typed view, so a module sizes its working memory once rather than per frame. The heavy body compiles once rather than per element type. |
| [`SpscRing`](../../../src/core/util/SpscRing.h) | The textbook Lamport queue: one thread pushes, one pops, and each index is written by exactly one side. The release and acquire pair is what publishes the element data. |
| [`TryLock`](../../../src/core/util/TryLock.h) | A non-blocking latch for a resource two threads reach. It never waits, so the render core is never held by a slower caller. |
| [`ThreadSlot`](../../../src/core/util/ThreadSlot.h) | A small table of slots a thread claims by its id, standing in for `thread_local` where an ESP32 task has no TLS. The script builtins reach their sinks through it. |
| `Sort.h` | In-place insertion sort over a fixed array, for the small bounded collections the system holds. Every module sorts the same way and supplies only its comparator. |

## Wire formats and parsing

| Header | What it is |
|---|---|
| [`ImprovFrame`](../../../src/core/util/ImprovFrame.h) | The serial provisioning protocol's framing, with no platform dependencies, so the host tests it without hardware. |
| [`ImprovOpReassembler`](../../../src/core/util/ImprovOpReassembler.h) | The state machine that reassembles a chunked operation into one buffer, with the duplicate and out-of-order guard. The platform layer owns the serial side around it. |
| [`FirmwareImage`](../../../src/core/util/FirmwareImage.h) | Reads an image's header without the vendor framework. The layout is a fixed on-disk format, so the same bytes parse on a device and in a test. |
| [`PinList`](../../../src/core/util/PinList.h) · [`HostList`](../../../src/core/util/HostList.h) | The two list parsers a board configuration needs: a pin list and a host list of addresses and names, both typed by a human into one text control. |
| [`Addressing`](../../../src/core/util/Addressing.h) · [`HostResolver`](../../../src/core/util/HostResolver.h) | One rule for sending to many receivers, unicast, multicast or broadcast, and the cache that turns a host name into an address without blocking the render thread. |
| [`ConfigScrape`](../../../src/core/util/ConfigScrape.h) · [`Ipv4`](../../../src/core/util/Ipv4.h) | The keys MoonBase reads out of the app's saved config without a parser, and the IPv4 parse and static-address rule both images check by. |
| [`sha256`](../../../src/core/util/sha256.h) · [`crc`](../../../src/core/util/crc.h) | A cryptographic digest for update verification and identity, and a cheap checksum used as a fingerprint of a block of state. |
| [`Sha1`](../../../src/core/system/Sha1.h) · [`Base64`](../../../src/core/system/Base64.h) | The two codecs the WebSocket handshake needs, the digest that standard mandates and the encoding that carries it. |

## Crossing the domain boundary

| Header | What it is |
|---|---|
| [`LightOutput`](../../../src/core/util/LightOutput.h) | The light pipeline as core reads it: the device's shape and its palettes as colors. The light domain implements it, so the network shims and MoonStats include nothing from the light domain. |
| [`AudioFrame`](../../../src/core/util/AudioFrame.h) | One snapshot of analyzed audio, produced once per render tick and consumed by audio-reactive effects. |
| [`BinaryBroadcaster`](../../../src/core/util/BinaryBroadcaster.h) | The sink a producer holds instead of the concrete server, so a driver sends to every client without depending on the web server. |
| `build_info.h` | The version and build identity, defined in `build_info.cpp` from the project manifest, the git hash and the release flags. |
