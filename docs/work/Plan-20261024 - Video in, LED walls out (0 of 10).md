# Plan: Video in, LED walls out

Plan of 2026-10-24, a Feature, for approval. **Recommended (2026-10-25):** approve it. MoonLight takes video from a PC, a grabber or a camera and shows it on an LED wall: ColorLight cards, HUB75 panels or any other driver, with OSC carrying the control between boards. Five new picture sources make that work: MJPEG over the network, Video4Linux capture, PipeWire, a browser tab and GIF files. MoonLight's own output also becomes a PipeWire node, and the ESP32-P4-PC's HDMI out feeds a wall's sending card. belrik, Delphium and robwomp asked for it on Discord (2026-10-21 to 2026-10-24) and offered to test.

## What exists

- **The picture family** ([streams design study](../design/streams.md)): a source is a transport plus a decoder delivering RGB888 frames, and the Picture effect samples them per light. A live source paces itself and the newest frame wins.
- **The Picture service** (`PictureService.h`) has two sources, an enum: `test pattern` and `usb capture`. A capture source runs its own task, which awaits a frame, decodes it into the service's buffer and publishes it. `adding-a-stream-source.md` names "a source per file behind a small registry" as the next step.
- **The capture seam** (`platform.h`, `videoCapture*`) is "decode into RGB888 at most this size". Only the P4 implements it, for a UVC grabber's MJPEG through the hardware JPEG decoder. The desktop has a stand-in that synthesizes frames for the tests; `usbvideo_frames.h` holds the shared mode pick, JPEG header parse and area shrink.
- **No JPEG decoder on the desktop**, and no YUV conversion anywhere.
- **HTTP fetches whole bodies only** (`httpFetch`, `JsonFetch`): a stream that never ends is refused as too big.
- **NDI** shows the pattern for an optional runtime: `NdiDriver` sends the rendered output, and the platform loads the NDI library with `dlopen` and reports it unavailable when absent.

## Design

### A source per file, behind a registry

Four sources would make the enum a branch per source in every function of the service. Each source becomes a small class with the same three calls: open with the wanted size, await a frame, decode it into the buffer. The service holds the running one and its task. The `source` control lists what the registry holds on this build, so a desktop without PipeWire does not offer it.

### Decoders are shared, transports are not

Every source here delivers one of three formats: MJPEG, YUYV (packed 4:2:2) or NV12 (4:2:0, a Cam Link 4K's default). The YUV conversions are a few lines of standard BT.601 and BT.709 math in core, tested once. JPEG decodes in the P4's hardware, as today, and on the desktop through libjpeg-turbo's TurboJPEG API, the system's own library. One `jpegDecode` seam serves the USB grabber and the network source alike.

### MJPEG over the network

The textbook stream is HTTP with `Content-Type: multipart/x-mixed-replace; boundary=…` (RFC 2046 multipart), a JPEG per part. ffmpeg serves it (`-f mpjpeg`), as do VLC, mjpg-streamer, go2rtc and nearly every IP camera. Two pieces:

- **A streaming HTTP read in the platform layer**: open a URL, read what has arrived with a timeout, close. Plain HTTP only, since these servers sit on the LAN. It runs on the source's own task, never on the render loop.
- **A multipart parser in core**, hardware-free and tested on the desktop: it takes a part's `Content-Length` when the server sends one and scans for the boundary when it does not. A part larger than the frame buffer is skipped, not truncated.

### Video4Linux on Linux

The capture seam's Linux implementation, so the `usb capture` source becomes `capture` and covers USB grabbers, the HDMI and CSI inputs of SBCs, and OBS's virtual camera (v4l2loopback). It uses V4L2's textbook streaming I/O: `VIDIOC_REQBUFS`, mapped buffers, `VIDIOC_QBUF` and `VIDIOC_DQBUF`, `select` to wait. A `device` control lists `/dev/video*`, and the mode pick prefers MJPEG, then NV12, then YUYV, at the smallest size that covers the wall. It runs headless, which a video processor box needs. macOS and Windows capture (AVFoundation, Media Foundation) stay in the backlog.

### PipeWire, in and out

PipeWire is the Linux media graph: a camera, an application's video and a consumer are each a node.

- **The source** connects to a video node by name (`target.object`): a camera WirePlumber exposes, a GStreamer pipeline, OBS. Screen capture under Wayland needs the xdg-desktop-portal handshake over D-Bus first, so it waits for a later step.
- **The sink** is a driver beside `NdiDriver`: the rendered output as a node that OBS, a recorder or another tool takes in, the Linux counterpart of NDI on one machine.

### The browser, as a source and as an authoring tool

The split follows the streams study's [browser and device](../design/streams.md#browser-and-device) rule: what runs with no browser open belongs on the device, what happens once with a person at the screen belongs in the browser.

- **The browser as a picture source**, `browser`: a page draws a video file, a webcam or a shared screen (`getDisplayMedia`) onto a canvas at the picture's size and sends the RGB frames over the existing WebSocket into the service's pixels, as WLED streams a tab (DedeHai, PR #4997). A `<video>` element first, since it plays every file the browser does, and GIF frame by frame after it through `ImageDecoder`. About 7 KB a frame at 64x36, 200 KB/s at 30 fps. It needs no hardware, so every board tests the picture path with it.
- **Converter and baking pages**: crop, scale and convert images and video to GIF, and bake a whole video into FSEQ for this layout ahead of time, sampled per light with the area filter in linear light, so the board plays it standalone through the FSEQ player. An hour is about 1.3 GB at 64x64 and 30 fps and 5.3 GB at 12,288 lights and 40 fps, so a baked movie travels on the SD card rather than over WiFi.

### GIF on the device

A picture source decoding GIF from flash or the SD card, one decoder per source rather than one global decoder, as WLED's Image effect does with bitbank2's AnimatedGIF in about 24 KB. PPM stays a test format for the decoder tests. A GIF plays at its own frame delays, so it is a recorded source: the device's clock picks the frame.

### HDMI out on the ESP32-P4-PC

HDMI out is the textbook way to feed commercial LED walls: Novastar and Colorlight sending cards take HDMI or DVI as their input. The Olimex ESP32-P4-PC (on the bench) has HDMI and MIPI-DSI out; a driver beside `NdiDriver` writes the rendered output to it as a picture, scaled to the mode the card asks for.

### The video processor

Nothing new connects the ends: a picture source, the Picture effect on a layer, and the panel-card or HUB75 driver already chain, and OSC (`OscService`) already carries the control between boards; the frames reach each wall through its driver. The plan proves the chain on the bench and writes it down as one how-to and one MoonWalk process.

## Estimate

| Step | Work |
|---|---|
| 1. The source registry; the two sources move behind it | 1 day |
| 2. YUV conversions in core; `jpegDecode` on the P4 (split from the grabber) and the desktop (TurboJPEG) | 1.5 days |
| 3. Video4Linux capture on Linux, with the device control | 2 days |
| 4. Streaming HTTP read, the multipart parser, the network MJPEG source, desktop and P4 | 2.5 days |
| 5. PipeWire source and sink | 2.5 days |
| 6. The chain on the bench, docs, MoonWalk | 1.5 days, plus bench time |
| 7. The browser as a picture source | 2 days |
| 8. GIF decoding, PPM as a test format | 1.5 days |
| 9. The converter and baking pages | 3 days |
| 10. HDMI out on the ESP32-P4-PC | 3 days, plus bench time |
| **Total** | **about 20 days** |

## Steps

1. ⏳ Picture sources behind a registry, a file each; `test pattern` and `capture` move over unchanged, and their tests pass untouched.
2. ⏳ YUYV and NV12 to RGB888 in core, tested against reference pixels; `jpegDecode` on the P4 and the desktop, the grabber using it; a desktop JPEG test frame decoded and compared.
3. ⏳ Video4Linux capture behind the capture seam on Linux: mode pick, mapped streaming, a device that vanishes mid-stream reported in the status and reopened; tested through `v4l2loopback` in CI, and on belrik's Cam Link 4K and composite grabber.
4. ⏳ The network MJPEG source: the platform's streaming read, the multipart parser with tests for both framings, a lost server and an oversized part; an ffmpeg `mpjpeg` stream shown on the desktop and on a P4.
5. ⏳ PipeWire: the source on a named node and the sink driver, the library optional at build time; a camera node shown, and the output taken into OBS.
6. ⏳ A grabber and a network stream on a ColorLight wall and on HUB75, a P4 and a Linux box, the frame time measured; the how-to, the Picture service page, `adding-a-stream-source.md`, a MoonWalk process.
7. ⏳ The browser as a picture source behind the registry: a video file, a webcam and a shared screen shown on every board, the frame rate and bandwidth measured.
8. ⏳ GIF from flash and the SD card as a picture source, PPM as the decoder's test format; its RAM and flash measured on an S3 and a P4.
9. ⏳ Browser pages: a converter to GIF, and a video baked into FSEQ for the layout and played by the FSEQ player.
10. ⏳ HDMI out on the ESP32-P4-PC as a driver, shown on a Novastar or ColorLight sending card.

## Decisions (recommended)

- **The registry before the sources.** It removes the enum and the `kRunsInTask` table, and every later source is one file.
- **TurboJPEG, from the system.** It is the decoder every Linux distribution and Homebrew ship, its API is one call, and nothing is vendored. Like libcurl, the build compiles it out when absent and reports the format unavailable.
- **PipeWire at build time, optional, like libcurl,** rather than `dlopen` as NDI uses. Its stream setup is built from inline SPA headers, which cannot be declared by hand the way NDI's functions are. The release `.deb` builds with it.
- **V4L2 directly, PipeWire beside it, not instead.** A headless box runs no PipeWire, and V4L2 is what a video processor runs on.
- **Plain HTTP for the network stream.** An MJPEG server on the LAN speaks nothing else, and the ESP32 then needs no TLS session for it.

## Open questions

- Screen capture through the portal: worth a step of its own once the PipeWire source runs? **Recommended (2026-10-25):** to the backlog until the PipeWire source runs, then a step of its own.
- The Picture service's 512-pixel cap: a wall of 768x384 samples a 512-wide picture. Enough, or does the cap rise on boards with PSRAM? **Recommended (2026-10-25):** keep 512: sampling a 512-wide picture onto a 768x384 wall reads fine, and the cap rises on PSRAM boards when a wall needs it.
- RTP/JPEG (RFC 2435) beside HTTP: only if a tester's source speaks nothing else.
- How far video goes on a board without a hardware decoder: GIF and image sequences, or a software decoder measured in flash first.
- How HDMI out is wired on the ESP32-P4-PC: its sources name HDMI and MIPI-DSI but no bridge chip; Olimex's separate P4 HDMI adapter uses Lontium's LT8912B (MIPI-DSI to HDMI, up to 1080p60), so check the schematic.

## What it removes

- The `Source` enum, its option table and the `kRunsInTask` table in `PictureService.h`, for the registry.
- The grabber's private JPEG path on the P4, for the shared `jpegDecode`.
- The backlog entries "A capture device on Linux feeds the Picture service", "MJPEG over the network feeds the Picture service" and "PipeWire as a source and a sink on Linux", which this plan replaces.
