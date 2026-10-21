# Plan: streams, every frame from outside, one model

Plan, for the product owner's approval; started as an analysis on 2026-10-13, when three pull requests by Gohnnyman (#138 a backlight effect and VideoService, #139 USB video capture, #140 backlight wiring) arrived the same week TouchMyLight announced an FSEQ player for a 20 × 20 × 20 cube (on #134). Read one by one they look like unrelated features: a TV backlight, a capture card, a show player. Read from MoonLight's architecture they are the same thing: a stream of frames arriving from outside the device. This plan models that once, by the textbook, so cameras, video, files, the network and the browser become combinations of a few parts rather than a feature each.

## The textbook model: three independent axes

Every media framework separates the same three questions: GStreamer, FFmpeg, Media Foundation and Video4Linux all do. Where do the bytes come from, how are they encoded, and what shape is the frame? Each axis varies on its own.

| Axis | Values in MoonLight's world |
|---|---|
| **Transport** | a file on flash or the SD card, the network (UDP, TCP, the browser's WebSocket), USB, a camera port (MIPI-CSI) |
| **Format** | raw RGB, PPM, GIF, MJPEG, FSEQ, the DDP, Art-Net and E1.31 framings; H.264 on the way out |
| **Geometry** | a picture (width × height), a light list (N lights, mapped by the sender), a volume (width × height × depth) |

A source is then a transport plus a decoder, delivering frames with a small descriptor: the geometry, the pixel format and the frame's time. An output is an encoder plus a transport. Everything users ask for is a combination:

| What users ask for | Transport | Format | Geometry |
|---|---|---|---|
| An xLights show (FSEQ) | SD file | FSEQ | light list |
| Network Receive today | UDP | DDP, Art-Net, E1.31 | light list |
| A GIF animation | flash or SD file | GIF | picture |
| An HDMI grabber (#139) | USB | MJPEG | picture |
| A camera on the P4 | MIPI-CSI | raw, through the image processor | picture |
| A screen shared from a browser tab | WebSocket | raw or DDP | picture or light list |
| A volumetric show for a cube | SD file | FSEQ | volume, or a light list placed in 3D |
| HLS and RTSP (shipped) | HTTP, RTSP | H.264 | picture |
| Recording | SD file | FSEQ | light list |

## How it lands on what MoonLight has: two families

The three axes describe any stream, and they do not make every stream one module. The standard is **one module model with typed data:** GStreamer gives every source the same element model and types each output (`video/x-raw` and `audio/x-raw` connect only to their own kind), and TouchDesigner, the most general tool here, separates its operators by data type, CHOPs for channels (DMX In, Art-Net, MIDI In, audio) and TOPs for textures (video, files, capture). MoonLight's module model is that element model: every source is a MoonModule with one lifecycle. On top of it, two families exist in the industry and in MoonLight, each with its own semantics, and they meet in the layer.

| | Channel data, the pixel protocols | Pictures |
|---|---|---|
| Live in | Network Receive: Art-Net, sACN (E1.31), DDP | capture: USB, MIPI-CSI, a browser tab |
| Recorded in | FSEQ playback | GIF, image sequences |
| Out | Network Send, FSEQ recording | HLS and RTSP, HDMI |
| Data | values mapped by the sender | width × height pixels |
| Into a layer by | placement through the layout | sampling per light |

**Channel data is a protocol family, not a decoder.** Art-Net, sACN and DDP carry universes and channels, discovery (ArtPoll), sync packets, priorities and merging between senders, and Network Receive and Network Send are the two halves of the family. An FSEQ file is a recording of exactly that data, which is why Falcon Player plays FSEQ and outputs sACN. So FSEQ joins this family as a sibling of Network Receive, not as a decoder in a generic stream source.

**Transports and files carry the same channel data.** Art-Net, E1.31 and DDP are transports, the data cut into packets to send live; FSEQ is a file format, the same data stored frame by frame with a step time to play later. So the family is one channel-data sink (staging, hold-last-frame, `mapped_by`, the placement, the counters) fed by sources: the network transports, and files from the SD card, a USB stick or flash, where the storage is a detail of the file source. Art-Net "from the SD card" is a recorded stream stored as FSEQ, as Falcon Player records it; FSEQ does not travel as a stream, Falcon Player copies the file to its remotes. Recording is the bridge: a live stream in, an FSEQ file out, written from the drivers' output.

**Pictures are their own family:** a picture source publishing frames, and an effect that samples them per light. `VideoService` from #138 is that source, renamed for what it is.

**What the families share, and only that:**

1. **Light-list placement:** writing values mapped by the sender into the layer through the layout. FSEQ needs it, and so does Network Receive for frames mapped by the sender (below). One helper, two users.
2. **The timing rule:** live (the newest frame wins) or recorded (the clock picks the frame), below.
3. **The layer:** both families end as an effect in a layer, so they compose with effects, blend modes and modifiers.

Two rules keep the picture side small enough for a microcontroller, where GStreamer's generality would not fit:

- **Decoders always deliver RGB888,** so nothing negotiates formats.
- **The picture descriptor stays a handful of fields:** size, frame time, live or recorded, and the tone curve a picture carries so the sampler averages light rather than bytes.

## Mapped by the receiver or by the sender

Network Receive copies channel k into logical cell k of its layer, and the layer's mapping then carries each logical cell to its physical light, so it follows the pipeline like any effect. Whether that is right depends on who mapped the frame, and the industry uses two conventions:

| Sender sends | Examples | Copied into the layer by index |
|---|---|---|
| **Mapped by the receiver:** row by row like a picture (raster order), expecting the receiver to apply its own wiring | image-based tools that leave the wiring to the controller | correct: the layer's mapping applies this board's serpentine or 3D wiring once |
| **Mapped by the sender:** arranged in the order the lights are wired (wiring order), because the sender did the mapping itself | xLights, FSEQ files, Falcon Player, pixel-mapping software with a fixture patch, MoonLight's own Network Send (it sends driver output) | mapped twice: the sender already applied the wiring and the layer applies it again, so a serpentine panel gets every other row reversed |

On a plain grid without modifiers the two conventions coincide, which is why the difference stays invisible on simple setups.

**So Network Receive gains a `mapped_by` setting,** `receiver` or `sender`, naming who maps the frame onto the wiring, `receiver` being the default:

- **Mapped by the receiver** is today's copy, unchanged, so nothing changes for anyone using it now.
- **Mapped by the sender** places channel k through the light-list placement, which is the inverse of the layout's mapping, before the layer's modifiers: composed with the layout, the two cancel, so channel k reaches physical light k, while a mirror or a rotation still transforms the frame and it still blends.
- **FSEQ is always mapped by the sender,** since xLights writes it that way, so the FSEQ effect uses the placement without a setting, through the channel-data sink it shares with Network Receive.

**The hot path stays as fast as today.** Network Receive is on it: at 16,000 lights and 40 frames a second every per-light step counts. The placement follows the rule the layer's mapping already follows, which keeps no table at all when logical order equals physical order (`hasLUT()` false):

- **Orders coincide** (a plain grid, no modifiers): the placement is a `memcpy`, zero cost over today.
- **Orders differ** (serpentine, 3D, a mapped layout): a per-light gather through the table, where the copy by index would have mapped twice.
- **Only on new data,** as today: a frame without new packets costs nothing.

A measurement gates it: `run_live_scenario` on the S3 with Network Receive on a large grid, before and after, in both the identity and the serpentine case; an identity path that moves at all does not merge.

One more option exists for a single feed that must be the absolute fastest: writing straight into the drivers' output, bypassing the layer, which is what WLED's realtime override does. It gives up blending, modifiers and composition, so it is not the default; it is a setting to add only when a user needs it.

## Real time and not real time

Streams split a second way, by who sets the pace, and the split is the textbook one between live and on-demand media.

| | Live (real time) | Recorded (not real time) |
|---|---|---|
| Examples | Network Receive, USB and CSI capture, a browser tab, MIDI-driven content | FSEQ, GIF and image sequences from flash or SD, a baked show |
| Who sets the pace | the sender: a frame arrives when it arrives | the device's clock: it pulls the frame the timeline asks for |
| Late or missing frames | the newest frame wins and an old one is dropped, never queued, since a queued frame is a late frame | none: the next frame is read ahead, one frame of buffer at most |
| Seek, loop, pause | not possible | the frame index comes from the clock, so they are arithmetic |
| Sync across boards | each board shows what it receives | clock sync: every board computes the same frame index from the shared clock |
| What it costs | latency, which the design keeps to one frame | storage bandwidth, which the design keeps to one frame per tick |

Each source states which kind it is, so its effect knows whether to show the newest frame or the frame the clock asks for. The transport decides it: a socket, a USB grabber and a camera are live; a file is recorded. A file can also be played live, at the pace of a sender elsewhere, which is how a recording replays exactly what arrived.

Not real time also names a third case that never reaches the device as a stream: **authoring**. Baking a picture or a video into FSEQ for a layout runs in the browser at whatever speed it takes, and only its result plays in real time.

## Modifiers and the single pass

A small device cannot run a PC pipeline, where each stage hands a full frame to the next. MoonLight never does: geometry is composed into coordinates at prepare time, and each frame does one pass.

- **Modifiers are coordinate folds,** composed into the layer's mapping table at prepare (`Layer::rebuildLUT`). Mirror, rotate, tile and multiply change where a logical cell lands, never the pixels; an animated modifier remaps coordinates per frame (`modifyLive`), still without an intermediate frame.
- **Effects write colors for their own logical cells** into one buffer per layer, and the mapping table carries them to the physical lights.

The textbook name is inverse mapping: compose every transform into one coordinate function, then sample the source once per output pixel. A GPU fragment shader works this way, and so does OpenCV's `remap` with a precomputed table.

**Both families follow that rule, so their consumers are effects, not modifiers.** Sampling is what an effect does: a color for its own coordinates, read from a decoded frame instead of computed from a formula.

- **At prepare** each builds a small table, as the layer builds its mapping: the picture effect maps each logical cell to a source pixel or, for area sampling, a source rectangle; the light-list placement maps light k to the logical cell it lands in, which is the physical-to-logical fold the layer's mapping build already computes.
- **Per frame** each does one gather into the layer buffer, and the existing mapping does the rest.

**Modifiers stay the one home for geometry transforms** and apply to a stream like any effect, as the architecture already says: the same RotateModifier rotates Fire, Noise or a network-received frame. So the picture effect has no rotate, mirror, scroll or offset of its own. It keeps only what belongs to sampling: fitting or stretching the picture onto the grid, point or area averaging, and the edge depth a backlight samples at.

**The picture side keeps the same budget:** decoding runs off the render tick, the effect does one gather per frame, and area sampling is capped per zone so a large source never costs a read per source pixel.

**The one full frame is the decoded picture,** the texture the shader samples, and it is an input rather than a stage: the P4's JPEG hardware decodes whole frames and a GIF composes onto a canvas, so a decoder produces it anyway. Channel data needs none: an FSEQ or DDP frame is written into the layer buffer through the placement table as it is read.

**What it removes.** The backlight stops being an effect of its own and becomes the picture effect's area sampler. Network Receive keeps its protocol family and gains sender mapping as a second convention, at no cost in the identity case. FSEQ reuses Network Receive's placement instead of growing its own. No stage holds a frame of its own, and no geometry option is duplicated inside an effect.

```mermaid
flowchart TB
    subgraph CH["<b>Channel data</b>: mapped by the sender"]
        direction LR
        nr["<b>Network Receive</b><br/>Art-Net, sACN, DDP"]
        fseq["<b>FSEQ playback</b><br/>from /sd<br/><i>TouchMyLight</i>"]
    end
    subgraph PIC["<b>Pictures</b>: width × height"]
        direction LR
        cap["<b>capture</b><br/>USB <i>#139</i>, CSI, browser tab"]
        gif["<b>GIF, sequences</b><br/>flash, /sd"]
        ps["<b>picture source</b><br/>test pattern<br/><i>#138</i>"]
    end
    place["<b>light-list placement</b><br/>through the layout"]
    samp["<b>picture effect</b><br/>sampled per light<br/>backlight <i>#138</i>"]
    mix["<b>Layers</b>: effects, opacity, blend modes, modifiers"]
    map["<b>Layouts</b>: 2D, 3D, backlight.mll <i>#140</i>"]
    subgraph OUT["<b>Out</b>"]
        direction LR
        drv["<b>LED drivers</b><br/>color trim <i>#139</i>"]
        ns["<b>Network Send</b>"]
        rec["<b>FSEQ recording</b>"]
        hls["<b>HLS, RTSP</b>"]
        hdmi["<b>HDMI</b><br/>ESP32-P4-PC"]
    end
    browser["<b>Browser</b>: converts and bakes<br/>at authoring time"]

    nr --> place
    fseq --> place
    cap --> ps
    gif --> ps
    ps --> samp
    place --> mix
    samp --> mix
    mix --> map
    map --> drv
    map --> ns
    map --> rec
    map --> hls
    map --> hdmi
    browser -.-> gif
    browser -.-> fseq

    classDef shipped fill:#1f4d3d,stroke:#5fb89a,color:#fff
    classDef pr fill:#2d3561,stroke:#7b88c9,color:#fff
    classDef planned fill:#4d3d1f,stroke:#c9a95f,color:#fff
    classDef proposal fill:#3d2d61,stroke:#a07bc9,color:#fff
    class nr,mix,ns,hls shipped
    class ps,samp,cap,map,drv pr
    class fseq planned
    class place,gif,rec,hdmi,browser proposal
    style CH fill:none,stroke:#888
    style PIC fill:none,stroke:#888
    style OUT fill:none,stroke:#888
```

Green ships today, blue is an open pull request, gold is planned by a contributor, purple is this plan's proposal. The diagram is configuration, not runtime stages: each family does a single gather into its layer, then the existing mapping. Everything from the layers down already exists.

## Where MoonLight stands among the tools

The tools look different, but they share this pipeline and put their weight in different places.

| Tool | Weight on | Runs on |
|---|---|---|
| Resolume | video clips in layers, pixel mapping to fixtures | PC |
| TouchDesigner | a general dataflow graph, anything to anything | PC |
| MADRIX | LED pixels: generative effects, layers, a 2D and 3D patch, media and capture, many outputs, record and replay on a standalone player | PC, standalone nodes |
| xLights with Falcon Player | sequencing offline, playback standalone | PC, Raspberry Pi |
| Hyperion, HyperHDR | capture to LED zones | Raspberry Pi, PC |
| WLED | effects on segments, a realtime override | ESP32 |

MoonLight is closest to MADRIX: generative effects, layers, 2D and 3D layouts and many outputs. The difference is that it runs on the controller itself. MADRIX's media, capture, and record-for-standalone-playback are what this stream model adds. The position is MADRIX-class pixel control, embedded, which no tool in the list occupies.

## What WLED did, and where it stopped

Checked 2026-10-13 against WLED main and WLED-MM.

- **One picture format, decoded on the device.** The Image effect (Aircoookie, PR #3835, finished by netmindz, shipped in WLED 16.0, May 2026) plays a GIF from the internal file system, up to 320 × 320, with about 24 KB of decoder RAM (bitbank2's AnimatedGIF). It scales by nearest neighbor, decodes one image at a time across all segments, and redraws only changed pixels, which upsets brightness limiting. WLED-MM backported it.
- **Every conversion in the browser.** DedeHai's PixelForge (PR #4982, WLED 16.0) crops, scales and encodes a GIF in the page and uploads it; Video Lab turns video into GIFs, Pixel Paint draws on the matrix, Font Factory makes scrolling-text fonts.
- **No capture on the device.** The only video path is a browser page streaming DDP over the WebSocket (DedeHai, PR #4997). Hyperion does all backlight processing on a computer and drives WLED over DDP.
- **FSEQ: five attempts, none merged** (#2413, #3551, #4570, #5641 open since May 2026). They bolted playback onto the realtime override, which replaces effects rather than composing with them, and each bundled an SD driver, a player, a UI and Falcon Player sync in one pull request. Recording was planned and never built.
- **SD** is a usermod nothing in core reads.

ESPixelStick, the reference small-device FSEQ player, plays uncompressed FSEQ v2 with sparse ranges from SD, floors the step at 25 ms, takes the frame index from a monotonic clock plus a sync offset, and seeks per frame, which keeps looping and sync trivial.

What it teaches: GIF first among picture formats, decoded per source rather than in one global decoder; conversion as browser pages; the layer as the home of every stream, which is exactly where WLED's FSEQ attempts went wrong; FSEQ starting where ESPixelStick stops. New beyond WLED: a stream composing in layers on any layout, 3D included; area sampling in linear light; capture decoded on the device; recording; synchronized playback across boards; the SD card as a first-class file store.

## Browser and device

One rule decides the split, and MoonLight already follows it where the device cannot do a job (the Firmware card's download for a board without TLS, MoonCloud's outbox, scripts fetched from GitHub by the page): **what must run with no browser open belongs on the device; what happens once, with a person at the screen, belongs in the browser.** In the stream model the browser is one more transport, and an authoring tool.

| Tier | Where | Examples |
|---|---|---|
| Authoring: once, a person present | browser | crop, scale and convert images; video to GIF or to FSEQ; fonts; an xLights model imported as a layout |
| Live, a browser as the source | the browser samples, the device displays | a screen (`getDisplayMedia`), a webcam or a video in a tab, streamed over the existing WebSocket |
| Run time: autonomous | device | effects, playback from flash or SD, USB and CSI capture, sync, recording, a show running for days |

**The browser can bake for this layout.** It already knows every light's position, since it draws them in the preview. So it can do the expensive part once, sampling a picture or a video per light with the area filter in linear light, and write the result as an FSEQ file. The device then plays it with the same cheap light-list path as an xLights show, on any layout, with no decoder.

| | Baked in the browser (FSEQ) | Decoded on the device (GIF) |
|---|---|---|
| Device cost | lowest: read and copy | decode and sample every frame |
| Follows a layout change | no: bake again | yes |
| Storage | large: 8000 lights at 40 fps is about 1 MB/s, so SD only | small: fits in flash |

A live browser source costs bandwidth, lights × 3 bytes × fps: about 60 KB/s for a 600-light TV frame at 30 fps, too much for 8000 lights over WiFi. USB and CSI capture run with no browser, so they belong on the device. A TV's picture from a set-top box or console never passes through a computer, so a backlight following it needs the grabber on the board (#139).

## 3D layouts

TouchMyLight's cube is the first user, so 3D works from the start.

- **A light list is mapped by the sender, and carries no positions.** An FSEQ frame holds values in the order xLights numbers the model's lights, the order the outputs drive them. Copied into the layer by index, it is mapped twice wherever the layer's order differs from the wiring, as on a cube (see mapped by the receiver or by the sender, above). The light-list placement puts light k at the coordinate the layout gives light k, and the layer's mapping carries it back to that physical light: correct on 1D, 2D and 3D layouts, and still composing with layers and modifiers. The user-side requirement is the one every FSEQ player has: the MoonLight layout and the xLights model number their lights the same way.
- **A picture reaches a 3D layout by projection:** each light samples the picture at its position seen along one axis, the planar projection media servers use.
- **A volume is the native 3D stream:** a frame per depth slice, copied straight into a 3D layer.

## The P4 and its codecs

The P4's hardware video codec encodes H.264 but does not decode it; its JPEG codec works both ways. So video arrives as MJPEG from a grabber, raw from a camera, or baked in the browser, while video leaves through the H.264 encoder HLS already drives.

**One board carries every piece.** The Olimex ESP32-P4-PC (February 2026, on the PO's bench) has HDMI and MIPI-DSI out, a MIPI-CSI camera connector, four USB 2.0 host ports, a microSD slot, Ethernet with PoE and 32 MB of PSRAM: capture in, HDMI out and FSEQ from the card on one P4. HDMI out is also the textbook way to feed commercial LED walls: Novastar and Colorlight sending cards take HDMI or DVI as their input.

## What each contribution becomes

| Contribution | In the plan |
|---|---|
| `VideoService` (#138) | the picture source, renamed for what it is |
| #138's backlight effect | the picture effect's area sampler, edge depth among its settings |
| USB capture (#139, first commit) | a picture source's USB transport and MJPEG decoder, behind the platform seam, in the P4 images with room |
| Per-die trim (#139, second commit) | a separate idea: output calibration in fixture profiles and correction, judged against a 3×3 correction matrix |
| Backlight wiring (#140) | a layout: Hyperion's classic layout, xLights' frame models |
| FSEQ player (TouchMyLight) | its own thin effect beside Network Receive, both on one channel-data sink (staging, hold-last-frame, `mapped_by`, the placement, the counters); a third user later records a live stream to FSEQ |
| Network Receive (shipped) | kept as the pixel-protocol family, gaining a `mapped_by` setting: `receiver` as today, `sender` through the light-list placement |

## What we do not do, and what we do differently

The work follows the idea, not the pull requests: a contribution lands where it fits the plan, in the shape the plan gives it.

**Not part of the idea:**

| What | From | Why not |
|---|---|---|
| The backlight as an effect of its own | #138 | it is one sampler setting of the picture effect; a card of its own duplicates the picture path |
| PPM as a user format | #138 | chosen only for want of a software JPEG decoder; GIF covers that need with the tools users already own, and PPM stays a test fixture |
| HDR tone curves (PQ, HLG) | #114, partly #138 | no planned source delivers HDR; the descriptor carries sRGB or linear until one does |
| Video decoding beyond MJPEG on the device (H.264, software JPEG) | implied by "video" | the browser bakes video into GIF or FSEQ; the device decodes only what its hardware decodes |
| FSEQ extras up front: zstd, `.xlz` archives, FPP Connect upload, FPP MultiSync | the pattern of WLED's attempts | the bundling that stalled all five of them; uncompressed v2 first, each extra on a measurement or a user who needs it |
| One module for every transport and data type | the first draft of this plan | the standard is one module model with typed data, as GStreamer and TouchDesigner do; MoonModule is that model, and a single module would flatten the pixel protocols' semantics (universes, discovery, sync, priorities) |
| A cap on the current a frame draws, a WiFi TX-power cache | #114 riders | not this idea; already dropped |

**In a different shape:**

| What | As proposed | Instead |
|---|---|---|
| Who builds the groundwork | the contributor shapes `VideoService` and the descriptor | MoonLight builds the groundwork below, since core owns the hard constructs; contributors add sources on top in small pull requests |
| USB capture | linked into every P4 image | only into images with room, measured as the SD card was (#136), and after the browser tab source, which serves every board |
| Smoothing | a backlight setting | a picture effect setting, since temporal smoothing applies to any picture, or dropped |
| Letterbox detection | built in | a later sampler option, when a user asks; every setting names the user who needs it |
| The capture seat, one instance per board | the default for every source | only for transports with exclusive hardware (USB, CSI); files and the network feed several sources at once |
| Multi-board sync | FPP MultiSync, as WLED's attempts did | MoonLight's own clock sync from the core backlog first; FPP compatibility later, as an adapter, if users mix in Falcon Player |

**As proposed:** #140's start corner, direction and offset (the textbook layout model, and small); area sampling in linear light (#138); the capture seam's shape, an opaque handle with a generation counter and formats learned at enumeration (#139); FSEQ playback, starting on the cube.

## The groundwork

What MoonLight builds first, so that every contribution after it is one source in a small pull request:

| # | Piece | Where | What it does |
|---|---|---|---|
| G1 | **Light-list placement** ✅ built | `Layer::placeByLayout`, `ChannelDataSink` | each logical cell takes the light the layout places at its position, from a table built at prepare, only when the frame shares its layer or sits under a modifier; on an identity mapping it is one copy, and alone on its layer G5 sends it to the output as it is; it replaced the mapping's inverse (G1's first form), whose static modifiers cancelled |
| G2 | **Network Receive's `mapped_by`** ✅ built | `NetworkReceiveEffect` | `receiver` as before (the default), `sender` through G1, the status saying "one copy" or "mapped twice"; tests pin a serpentine grid and a 3D layout, which reversed every other row before; on the S3 at 12,288 lights a serpentine layout costs 4.5 ms per frame against 1.1 ms for receiver mapping, an identity layout the same as receiver mapping |
| G3 | **The picture source** ✅ built | a core service, from #138's `VideoService` | the descriptor, publishing in `AudioService`'s pattern, a test pattern as its only source |
| G4 | **The picture effect** ✅ built | a light effect, from #138's sampler | point or area sampling in linear light, fit or stretch, a table at prepare and one gather per frame |
| G5 | **Sender mapping skips the board's mapping** ✅ built | `Layer`, `Drivers` | the existing rule, hottest path when possible, extended to layouts that are not the identity: an identity layout is already one copy at any number of layers, since the drivers do not map; a layer whose only writer is mapped by the sender, with no modifiers and no second layer, is read as it is whatever the layout, so the layout still shapes the preview and the board's own effects; blending or a modifier on such a layout falls back to G1's round trip, with G1 inverting the layout's mapping alone so the modifier transforms the frame; the tests add a rotated and a mirrored layer to the serpentine and 3D cases |
| G6 | **The channel-data sink** ✅ built | `ChannelDataSink`, from Network Receive | staging, hold-last-frame, `mapped_by` and the placement, the counters and the status path, extracted from Network Receive so a file source is a thin effect on it; Network Receive's tests pass unchanged |
| G7 | **A contributor page** ✅ built, [the how-to](../../reference/adding-a-stream-source.md) | docs | how to add a channel-data source and how to add a picture source, with the test pattern as the template |
| G8 | **The browser as a picture source** | `PictureService`, a UI page | a second `source`, `browser`: a page draws a video file, a webcam or a shared screen (`getDisplayMedia`) onto a canvas at the picture's size and sends the RGB frames over the existing WebSocket into the service's pixels, as WLED streams a tab (DedeHai, PR #4997); a `<video>` element first, since it plays every file the browser does, and GIF frame by frame after it through `ImageDecoder`; about 7 KB a frame at 64x36, 200 KB/s at 30 fps; it needs no hardware, so every board tests the picture path with it. Follow-ups: live recording of the drivers' output to FSEQ on SD (the Recording row below), and the browser baking a whole video into FSEQ for this layout ahead of time, so nothing runs in real time and the board plays it standalone; an hour is about 1.3 GB at 64x64 and 30 fps and 5.3 GB at 12,288 lights and 40 fps, so a baked movie travels on the card rather than over WiFi |

G1 and G2 come first: they add the second convention every mapping sender needs, and wait on no naming. G3 and G4 take #138's core part, credited to Gohnnyman. Before code: the double mapping confirmed on the bench (a serpentine grid fed by xLights, expected to show every other row reversed) and the open questions below decided.

## How contributions arrive

Each later piece is a small pull request, one topic, based on current main, with only its own commits:

| Pull request | Who | On |
|---|---|---|
| Backlight wiring, today's #140 resubmitted on main | Gohnnyman | nothing; it can come now |
| Color trim, split from #139 | Gohnnyman | nothing; a separate idea |
| FSEQ playback from SD: uncompressed v2, sparse ranges, the frame index from the clock, verified on a 3D layout. It builds on the channel-data sink (G6) that Network Receive and the FSEQ player both use, each a thin effect with its own settings (decided 2026-10-14) | TouchMyLight | G1, G2, G6; G5 sends a player alone on its layer to the output in one copy on any layout, a 3D one included |
| Border sampling: edge depth, smoothing | Gohnnyman | G4 |
| GIF decoder, PPM as a test format | open | G3 |
| USB capture with its MJPEG decoder, P4, with Teensy stubs | Gohnnyman | G3 |
| Browser pages: converter, baking to FSEQ | open | G1 to G4, G8 |
| Recording, HDMI out on the ESP32-P4-PC, clock sync | open | FSEQ playback |

Pull requests #138 and #139 close with thanks once G3 and G4 land, their good parts credited in the groundwork; a stacked pull request waits for the one below it, as [CLAUDE.md](../../../CLAUDE.md) says.

## Skeletons for the contributions

What each pull request builds, settled before code so it arrives small and on design. The names follow the open naming question below. Each passes `check_specs` with a card, and its tests pass on the desktop before a board sees it.

**FSEQ player (TouchMyLight), on G6.** A thin effect on the channel-data sink, the file being its only source; FSEQ is mapped by the sender, so it sets no `mapped_by` of its own. `FseqPlayerEffect` is registered with these settings on the sink and reads no file yet, its status saying so: the contribution is the file reading in `prepare()` and `tick()`.

```cpp
class FseqPlayerEffect : public EffectBase {   // light/effects/FseqPlayerEffect.h
    char file[64];      // a path on /sd or flash, picked from the file list
    bool loop = true;
    uint8_t speed = 100;   // percent of the file's step time
    // prepare(): open the file, read the v2 header, size the sink to its channel count, refuse compressed files and sparse ranges with a status
    // tick(): the frame index from the clock (start time, speed), read that frame into the sink, which places and holds it
    ChannelDataSink sink_;
};
```

Tests: a v2 header read and refused when compressed; the frame index from the clock at 50 and 200 percent; a written serpentine file placed on a serpentine and a 3D layout reaching each light as sent (`moondeck/run/fseq_play.py` writes the reference files); a missing file degrades to a status, never a crash.

**The picture source (G3) and the picture effect (G4), from #138.** A core service publishing frames in `AudioService`'s pattern, and an effect sampling them per light.

```cpp
struct PictureFrame { const uint8_t* rgb; uint16_t width, height; uint32_t generation; };   // core/util/PictureFrame.h
class PictureService : public MoonModule {     // core/services/PictureService.h
    uint8_t source = 0;   // test pattern first; each later source adds an option
    static const PictureFrame* latestFrame();   // always valid: a blank frame when nothing publishes
};
class PictureEffect : public EffectBase {     // light/effects/PictureEffect.h
    uint8_t fit = 0;       // fit or stretch
    uint8_t sampling = 0;  // point or area, area in linear light
    // prepare(): a table from each light's position to its picture rectangle; tick(): one gather per new generation
};
```

Tests: the test pattern's frame; point and area sampling against hand-computed values; fit against stretch on a non-square layout; a 3D layout sampling its front face; no new frame, no gather.

**Backlight wiring (#140).** The layout exists as `moonlive/layouts/backlight.mll`, one strand clockwise from the top-left corner; #140's own commit extends that script, the name staying Backlight since its lights sit on the edge pixels of a wall's canvas.

```
class BacklightLayout {          // moonlive/layouts/backlight.mll
  byte wallCols, wallRows;       // kept: the wall's size in pixels
  byte across, down;             // kept: lights along the top and along a side
  byte startCorner = 0;          // where the strand enters, a select with the corner names
  byte clockwise = 1;            // which way it runs
  int offset = 0;                // lights past the corner where it begins, counted along the strand
  void placeLights();            // one walk around the edge, rotated and reversed; only the indices move
}
```

Tests: each start corner and direction pinned as the expected sequence on a small wall; an offset along a counter-clockwise strand; the defaults reproducing today's order; the script's line in the IR golden table. Later, optionally: Hyperion's separate count per edge, which allows a gap for a TV stand.

**Color trim (#139's second commit).** Output calibration, so it lives in the fixture profile and the correction, judged against a 3×3 correction matrix before code.

```cpp
// light/drivers/FixtureProfilesModule.h: per profile, beside the channel roles
uint8_t trimRed = 255, trimGreen = 255, trimBlue = 255;   // gain per channel
// light/drivers/Correction.h: rebuild() folds the trim into each channel's table, so the hot path is unchanged
```

Tests: a trim scaling one channel and leaving the others; a trim of 255 being the identity; the trim surviving a profile save and load.

## Where it stands (2026-10-14)

Built on this branch: G1 to G7, and around them what measuring Network Receive on real boards turned up.

<img src="../../assets/light/effects/NetworkReceiveEffect.png" width="404" alt="Network Receive receiving a sender-mapped stream in one copy">

What the card added: `mapped_by` picks who maps a frame onto the wiring, the status names the sender and the path its frame takes ("one copy" or "mapped twice"), and `packets_per_s` and `frames_per_s` count the last second, so a scenario compares them with what the sender sent. Here the S3 testbench receives `fseq_play`'s 64x64 serpentine sweep: alone on its layer, the frame goes to the output in one copy, and the tick fell from 5.83 ms to 2.96 ms against G1's two mappings.

- **Network Receive** drains its sockets in a receive task on core 1, shows its path in the status ("one copy" or "mapped twice"), and counts `packets_per_s` and `frames_per_s`.
- **TryLock** releases with an exchange: in PSRAM a plain store raced the emulated read-modify-write, left the latch set with no owner, and took the WiFi stack down with the stuck receive task.
- **The gather** runs in 4.5 ms instead of 7.3 ms at 12,288 lights on the S3.
- **Defaults:** a UDP mailbox of 32 on every board, and 30 Ethernet receive buffers of 1600 bytes on the P4, transmit kept at 10.
- **Scenarios:** `scenario_NetworkReceive_two_boards` and `scenario_NetworkSend_one_sender_many_receivers` drive several boards (`--board name=host`), recorded per combination of boards.

Received at 12,288 lights, 40 frames/s sent (1,040 packets/s), mapped by the receiver:

| Sender to receiver | Packets/s | Frames/s | Receiver tick |
|---|---|---|---|
| S3 WiFi to S3 WiFi | 600 to 980 | 20 to 23 | 6.8 to 8.2 ms |
| P4 Ethernet to S3 WiFi | 616 | 23 | 5.8 ms |
| S3 WiFi to P4 Ethernet | 984 | 32 | 0.3 ms |
| P4 Ethernet to P4 Ethernet | 790 to 846 | 8 to 19 | 0.8 ms |

P4 to P4 started the day at about 200 packets/s and 0 to 1 frames. What remains is the sender's burst: the access point drops it on WiFi, and on Ethernet the push packet at its end is the one most often lost. Network Send paces its packets, set by `packet_gap` and off at 0 (a 0.5 ms gap recovered 96% on WiFi), which took P4 to P4 to 26 to 36 frames/s. G5 removes the double mapping that sender mapping cost on a layout that is not the identity (4 ms against 1.6 ms on the S3): a stream alone on its layer goes to the output in one copy, and a static modifier on its layer transforms it. G8 is next. The rest is in the backlog below.

## Backlog, found while measuring

- **A blocking HTTP write freezes the render task under a stream.** `serveState` writes the full `/api/state` (about 35 KB) with a blocking `lwip_write` on the render task; with the S3 receiving 12,288 lights over WiFi the client's acknowledgements slow, the send buffer fills, and the write holds the render task for seconds until the task watchdog reboots the board (backtrace: `handleConnection` → `serveState` → `JsonSink::flushStage` → `lwip_write`, 2026-10-14). The UI no longer reads the full state since state by subscription, so the trigger was the scenario runner's reads at every measure step; only the diagnostic bundle, assign mode and a backup restore still read it, each on a user's explicit action. The textbook fix: write without blocking and resume a partial write on the next tick, as uploads already stream. Check whether the WebSocket state pushes, smaller TCP writes on the same task, can block the same way.
- **The UDP mailbox of 32 and the paced send task on boards without PSRAM.** `CONFIG_LWIP_UDP_RECVMBOX_SIZE=32` is in the base defaults for every board. It costs about 128 bytes per socket, and on WiFi the queued packets are WiFi's own receive buffers, capped at 32. On Ethernet without PSRAM they come from the internal heap at about 1.6 KB each, so a full queue holds about 50 KB on a classic board with 100 to 170 KB free, which could starve the HTTP server during a burst. Measure it with `scenario_NetworkReceive_two_boards` on a classic Ethernet board (the Olimex at 192.168.1.210) receiving from a P4, reading free heap and the HTTP answers; if it hurts, the 4 MB classic fragments override it with a smaller value. Measure Network Send's paced task there too, with the classic as the sender: about 5.5 KB of internal RAM per driver (a 4 KB stack, the 1.45 KB packet buffer, a timer), and on a single-core chip such as the C3 the send task outranks the frame loop, so each packet briefly interrupts it.
- **The two-board scenarios on every platform.** `scenario_NetworkReceive_two_boards` and `scenario_NetworkSend_one_sender_many_receivers` run today on S3 boards over WiFi and P4 boards over Ethernet, in all four pairings. Add the Teensy 4.1 (Ethernet), desktop builds over WiFi, and the NanoPi over Ethernet, as sender and as receiver, so every platform's receive cost and loss sits in the same table. A desktop or NanoPi board joins with `--board name=<ip>:8080`. The Teensy build runs bare-metal with no scheduler, so Network Send and Network Receive work inside their tick (`spawnPinnedTask` returns false); QNEthernet fills its receive buffers in an interrupt, so a late drain loses less there. Its runs show whether threads (TeensyThreads or a FreeRTOS port, with stacks in its tight RAM and shared structures made thread-safe) are worth it.
- **E1.31 multicast never arrives.** Network Receive binds 5568 but never joins the multicast groups, so both receivers in `scenario_NetworkSend_one_sender_many_receivers` read 0 packets while Network Send multicast; `UdpSocket::membership` exists in the platform layer and goes unused. Join the groups of the universes the layer covers, from `universe_start` on.
- **The Art-Net broadcast counts do not add up.** In the same scenario at 64x64 (about 1,000 packets/s sent, 40 frames), the S3 counted 1,212 packets/s and 96 frames/s while the P4 counted 488 and 39. Another bench board broadcasting, or duplicates over WiFi; a packet capture on port 6454 settles it.
- **A sending P4 reboots with 30 Ethernet transmit buffers.** With 30 x 1600 bytes for transmit a P4 rebooted about 15 s into sending; IDF takes every Ethernet DMA buffer from internal RAM, about 96 KB with 30 + 30, so internal-heap exhaustion is the suspect, unverified. troyhacks raised and reverted the same values within 26 minutes ([bdf6c17e](https://github.com/troyhacks/esp32-arduino-libs/commit/bdf6c17e), [c8a43b7e](https://github.com/troyhacks/esp32-arduino-libs/commit/c8a43b7e)). Transmit stays at 10.
- **The P4's HTTP server answers "incomplete request body" under a stream.** Two `set_control` writes and one `apply_state` failed this way while the P4 received DDP over Ethernet; the same short body window backlog-core already names.
- **`CONFIG_ETH_IRAM_OPTIMIZATION=y` on the P4.** The one Ethernet change troyhacks kept; measure it with `scenario_NetworkReceive_two_boards`, P4 to P4.
- **A UDP mailbox of 64 on the P4.** Espressif's own P4 Arduino default; 32 against 64 measured the same on the S3 over WiFi, so measure it P4 to P4 over Ethernet, where bursts arrive at wire speed.
- **The receive task's priority on Ethernet.** Ours runs at 3 on core 1, below the driver task (`mmEncode`, 5); troyhacks' Art-Net receive runs at 20 on core 0, blocking in `recvfrom()`, above lwIP's own thread (`tcpip`, 18), which a busy drain could starve. On the S3 over WiFi 6 against 3 changed nothing, the loss being at the access point. Measure 6 (above the driver task) and 19 (above lwIP) P4 to P4.
- **Network Send paces its packets.** Back-to-back bursts are what the access point drops (a 0.5 ms gap recovered 96% on WiFi) and what loses the push packet at the end of an Ethernet burst. Spread a frame's packets over part of the frame period, and back off on a full transmit queue (`ENOMEM`) rather than adding buffers, as Espressif advises ([esp-idf#13602](https://github.com/espressif/esp-idf/issues/13602)) and troyhacks' raw Ethernet driver does.
- **The live runner restores no board when a board fails mid-run.** A two-board run whose receiver rebooted left the sender's scenario canvas and its Network Send on P4 .139, still streaming to the S3. The restore of every board belongs in a `finally`, so an exception or a dead board still puts the others back.
- **HTTP to an S3 fails under a stream from the same computer.** With a 40 fps DDP stream from the Mac into either S3 over WiFi, 4 to 6 of 20 HTTP probes from that Mac answered, while the render loop stayed healthy (log: 5.5 ms ticks, the HTTP server idle) and the Ethernet P4 took the same stream with every probe answered. A phone opening the S3's page during such a stream tells whether the S3's WiFi suffers or only the sending computer's link carrying both.

## Open questions

- The names: `PictureService` and `PictureEffect`, or names closer to the industry's (media source, pixel map)?
- How far video goes on a board without a hardware decoder: GIF and image sequences, or a software decoder measured in flash first.
- How HDMI out is wired on the ESP32-P4-PC: its sources name HDMI and MIPI-DSI but no bridge chip; Olimex's separate P4 HDMI adapter uses Lontium's LT8912B (MIPI-DSI to HDMI, up to 1080p60), so check the schematic.
- Whether a layout reads xLights model exports, so a 3D model's node order and positions come from the file that designed the show.

## Prior art

- GStreamer and FFmpeg: transport, format and geometry as separate concerns; one element model with typed outputs.
- TouchDesigner: one operator model, separated by data type into channel operators (CHOPs: DMX In, Art-Net, MIDI, audio) and texture operators (TOPs: video, files, capture), the reference for the two families.
- Inverse mapping: a GPU fragment shader sampling a texture through a composed coordinate transform, and OpenCV's `remap` with a precomputed table; the reason MoonLight composes geometry instead of chaining frames.
- MADRIX: the closest tool, LED pixels with effects, layers, a 3D patch, media, capture and standalone playback.
- Resolume: video layers with pixel mapping to fixtures on a PC.
- Hyperion and HyperHDR: grabbers, LED layout, mean-color zones, smoothing, black-border detection.
- xLights and the FSEQ format (v2: header, channel count, frame count, step time, optional zstd and sparse ranges); Falcon Player for standalone playback and MultiSync; ESPixelStick for FSEQ from SD on an ESP32.
- WLED: the Image effect (PR #3835), PixelForge (PR #4982), DDP over WebSocket (PR #4997), and the unmerged FSEQ attempts (#2413, #4570, #5641).
