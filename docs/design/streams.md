# Streams: every frame from outside, one model

Cameras, video, files, the network and the browser all deliver a stream of frames from outside the device. This study models them once, by the textbook, so each becomes a combination of a few parts rather than a feature of its own. What is built of it, the two families and how a sender-mapped frame lands in a layer, is in [MoonLight, frames from outside](../explanation/architecture/moonlight.md#frames-from-outside); the picture sources still to come are planned in [Video in, LED walls out](../work/Plan-20261024%20-%20Video%20in%2C%20LED%20walls%20out%20(0%20of%2010).md).

## Three independent axes

Every media framework separates the same three questions, as GStreamer, FFmpeg, Media Foundation and Video4Linux all do: where the bytes come from, how they are encoded, and what shape the frame has.

| Axis | Values in MoonLight's world |
|---|---|
| **Transport** | a file on flash or the SD card, the network (UDP, TCP, the browser's WebSocket), USB, a camera port (MIPI-CSI) |
| **Format** | raw RGB, PPM, GIF, MJPEG, FSEQ, the DDP, Art-Net and E1.31 framings; H.264 on the way out |
| **Geometry** | a picture (width × height), a light list (N lights, mapped by the sender), a volume (width × height × depth) |

A source is a transport plus a decoder, delivering frames with a small descriptor: the geometry, the pixel format and the frame's time. An output is an encoder plus a transport. What users ask for is a combination:

| What users ask for | Transport | Format | Geometry |
|---|---|---|---|
| An xLights show (FSEQ) | SD file | FSEQ | light list |
| Network Receive | UDP | DDP, Art-Net, E1.31 | light list |
| A GIF animation | flash or SD file | GIF | picture |
| An HDMI grabber | USB | MJPEG | picture |
| A camera on the P4 | MIPI-CSI | raw, through the image processor | picture |
| A screen shared from a browser tab | WebSocket | raw or DDP | picture or light list |
| A volumetric show for a cube | SD file | FSEQ | volume, or a light list placed in 3D |
| HLS and RTSP | HTTP, RTSP | H.264 | picture |
| Recording | SD file | FSEQ | light list |

Recording is the bridge between transports and files: a live stream in, an FSEQ file out, written from the drivers' output. Art-Net "from the SD card" is that recording, as Falcon Player records it; FSEQ does not travel as a stream, since Falcon Player copies the file to its remotes.

## Live and recorded

Streams split a second way, by who sets the pace: the textbook split between live and on-demand media.

| | Live | Recorded |
|---|---|---|
| Examples | Network Receive, USB and CSI capture, a browser tab | FSEQ, GIF and image sequences from flash or SD, a baked show |
| Who sets the pace | the sender: a frame arrives when it arrives | the device's clock: it pulls the frame the timeline asks for |
| Late or missing frames | the newest frame wins and an old one is dropped, never queued, since a queued frame is a late frame | none: the next frame is read ahead, one frame of buffer at most |
| Seek, loop, pause | not possible | the frame index comes from the clock, so they are arithmetic |
| Sync across boards | each board shows what it receives | every board computes the same frame index from a shared clock |
| What it costs | latency, kept to one frame | storage bandwidth, kept to one frame per tick |

The transport decides the kind: a socket, a grabber and a camera are live; a file is recorded. A file can also play at the pace of a sender elsewhere, which is how a recording replays exactly what arrived. A third case never reaches the device as a stream: **authoring**, such as baking a video into FSEQ for a layout, runs in the browser at whatever speed it takes, and only its result plays in real time.

## Browser and device

One rule decides the split, and MoonLight already follows it where the device cannot do a job (the Firmware card's download for a board without TLS, MoonCloud's outbox, scripts fetched from GitHub by the page): **what must run with no browser open belongs on the device; what happens once, with a person at the screen, belongs in the browser.**

| Tier | Where | Examples |
|---|---|---|
| Authoring: once, a person present | browser | crop, scale and convert images; video to GIF or to FSEQ; fonts; an xLights model imported as a layout |
| Live, a browser as the source | the browser samples, the device displays | a screen (`getDisplayMedia`), a webcam or a video in a tab, over the existing WebSocket |
| Run time: autonomous | device | effects, playback from flash or SD, USB and CSI capture, sync, recording, a show running for days |

**The browser can bake for this layout.** It knows every light's position, since it draws them in the preview, so it can sample a picture or a video per light once, with the area filter in linear light, and write an FSEQ file. The device plays it on the cheap light-list path, with no decoder.

| | Baked in the browser (FSEQ) | Decoded on the device (GIF) |
|---|---|---|
| Device cost | lowest: read and copy | decode and sample every frame |
| Follows a layout change | no: bake again | yes |
| Storage | large: 8000 lights at 40 fps is about 1 MB/s, so SD only | small: fits in flash |

A live browser source costs lights × 3 bytes × fps: about 60 KB/s for a 600-light TV frame at 30 fps, too much for 8000 lights over WiFi. A TV's picture from a set-top box or console never passes through a computer, so a backlight following it needs the grabber on the board.

## 3D layouts and the P4

- **A light list carries no positions:** the layout places it, on 1D, 2D and 3D layouts alike, numbering its lights as the xLights model does.
- **A picture reaches a 3D layout by projection:** each light samples the picture at its position seen along one axis, the planar projection media servers use.
- **A volume is the native 3D stream:** a frame per depth slice, copied straight into a 3D layer.
- **The P4's hardware video codec encodes H.264 but does not decode it**; its JPEG codec works both ways. So video arrives as MJPEG from a grabber, raw from a camera, or baked in the browser, and leaves through the H.264 encoder HLS drives. The Olimex ESP32-P4-PC carries every piece on one board: HDMI and MIPI-DSI out, a MIPI-CSI connector, four USB host ports, a microSD slot, Ethernet with PoE and 32 MB of PSRAM.

## Where MoonLight stands among the tools

| Tool | Weight on | Runs on |
|---|---|---|
| Resolume | video clips in layers, pixel mapping to fixtures | PC |
| TouchDesigner | a general dataflow graph, anything to anything | PC |
| MADRIX | LED pixels: generative effects, layers, a 2D and 3D patch, media and capture, many outputs, record and replay on a standalone player | PC, standalone nodes |
| xLights with Falcon Player | sequencing offline, playback standalone | PC, Raspberry Pi |
| Hyperion, HyperHDR | capture to LED zones | Raspberry Pi, PC |
| WLED | effects on segments, a realtime override | ESP32 |

MoonLight is closest to MADRIX: generative effects, layers, 2D and 3D layouts and many outputs, running on the controller itself. MADRIX's media, capture and record-for-standalone-playback are what this model adds: MADRIX-class pixel control, embedded.

## What WLED did, and where it stopped

Checked 2026-10-13 against WLED main and WLED-MM.

- **One picture format, decoded on the device.** The Image effect (PR #3835, WLED 16.0) plays a GIF from the internal file system, up to 320 × 320, with about 24 KB of decoder RAM, scales by nearest neighbor, and redraws only changed pixels, which upsets brightness limiting.
- **Every conversion in the browser.** PixelForge (PR #4982) crops, scales and encodes a GIF in the page; Video Lab turns video into GIFs.
- **No capture on the device.** The only video path is a browser page streaming DDP over the WebSocket (PR #4997); Hyperion does backlight processing on a computer.
- **FSEQ: five attempts, none merged** (#2413, #3551, #4570, #5641). They bolted playback onto the realtime override, which replaces effects rather than composing with them, and each bundled an SD driver, a player, a UI and Falcon Player sync in one pull request.

ESPixelStick, the reference small-device FSEQ player, plays uncompressed FSEQ v2 with sparse ranges from SD, floors the step at 25 ms, and takes the frame index from a monotonic clock plus a sync offset. What it teaches: GIF first among picture formats; conversion as browser pages; the layer as the home of every stream, exactly where WLED's FSEQ attempts went wrong.

## Shapes left out, and why

| Shape | Why not |
|---|---|
| A backlight effect of its own | one sampler setting of the picture effect; a card of its own duplicates the picture path |
| PPM as a user format | GIF covers the need with tools users own; PPM stays a test fixture |
| HDR tone curves (PQ, HLG) | no planned source delivers HDR |
| Video decoding beyond MJPEG on the device | the browser bakes video; the device decodes only what its hardware decodes |
| FSEQ extras up front (zstd, `.xlz`, FPP Connect, MultiSync) | the bundling that stalled WLED's attempts; each extra waits for a measurement or a user |
| One module for every transport and data type | one module model with typed data, as GStreamer and TouchDesigner do; one module would flatten the pixel protocols' semantics |
| A feed written straight into the drivers' output | WLED's realtime override gives up blending, modifiers and composition |
| FPP MultiSync for multi-board sync | MoonLight's own clock sync first; FPP compatibility later, as an adapter |

## Open questions

- The names: `PictureService` and `PictureEffect`, or names closer to the industry's (media source, pixel map)?
- Whether a layout reads xLights model exports, so a 3D model's node order and positions come from the file that designed the show.

## Prior art

- GStreamer and FFmpeg: transport, format and geometry as separate concerns.
- TouchDesigner: one operator model separated by data type, channel operators (DMX, Art-Net, MIDI) and texture operators (video, capture), the reference for the two families.
- Inverse mapping: a fragment shader sampling through a composed transform, OpenCV's `remap`.
- MADRIX, Resolume, Hyperion and HyperHDR, as in the table above.
- xLights and the FSEQ v2 format; Falcon Player for standalone playback and MultiSync; ESPixelStick for FSEQ from SD on an ESP32.
- WLED: the Image effect (#3835), PixelForge (#4982), DDP over WebSocket (#4997), the unmerged FSEQ attempts.
