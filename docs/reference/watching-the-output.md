# Watching the output as video

Three drivers hand the lights to a video tool. NDI feeds OBS, Resolume or TouchDesigner, HLS feeds VLC, a browser or an Apple TV, and RTSP feeds VLC or `ffplay`. Each needs one install on a desktop, and then a player opens the stream.

Steps: [Watch the output as video](../moonwalk.md#watch-output).

## Installing ffmpeg

HLS and RTSP on a desktop encode through ffmpeg, which MoonLight never ships or links. Any 5.x or newer on the PATH works:

| OS | Command |
|---|---|
| macOS | `brew install ffmpeg` |
| Windows | `winget install ffmpeg` |
| Debian, Ubuntu, Raspberry Pi OS | `sudo apt install ffmpeg` |

Without it the driver's status reads `ffmpeg not found` and nothing else changes. The ESP32-P4 needs none of this: its own H.264 block encodes.

### Picking an encoder

HLS's `encoder` control picks which encoder ffmpeg runs. `libx264`, the default, is software and ships in practically every ffmpeg build. A hardware encoder offloads the work and suits a large grid:

| Machine | `encoder` |
|---|---|
| A Mac | `h264_videotoolbox` |
| A Raspberry Pi | `h264_v4l2m2m` |
| An NVIDIA card | `h264_nvenc` |

An encoder your ffmpeg lacks exits at once, and the status reads `encoder exited - check ffmpeg`.

## On the ESP32-P4

The P4 encodes in hardware and MoonLight packages the stream itself, served from RAM so the flash is not worn. Its encoder takes even dimensions from 80x80 to 1920x2032: an odd wall has its scale doubled, and a wall too large is refused with a status. HLS and RTSP share that one encoder, so the second to start reports it in use.

## Sizing the picture

`scale` at 0 picks the smallest whole factor that lifts both axes to 80 pixels, so a 20x10 wall streams as 160x80 and a wall past 80 stays 1:1. Each light stays a solid square. Raising `scale` by hand on a large wall costs encode time and buys nothing a player's own zoom does not.

The bitrate follows from the picture size and `targetFps`, at about 0.1 bits per pixel per frame: a 512x512 wall at 30 fps lands near 800 kbit. Lower `targetFps` to save bandwidth, since fewer frames suits LED content better than a blockier picture.

## Installing the NDI runtime

The NDI driver runs on a desktop, and MoonLight cannot ship the NDI runtime. Until it is installed the driver reports `NDI runtime not installed` and everything else works.

| OS | Where it comes from |
|---|---|
| macOS | [NDI Tools](https://ndi.video/tools/), free. It keeps the runtime inside its app bundles, where MoonLight looks for it; a Resolume install carries one too. |
| Windows | The [NDI Tools](https://ndi.video/tools/) or SDK installer, which puts `Processing.NDI.Lib.x64.dll` on the PATH. |
| Linux | The NDI SDK. |

To see the output, open a receiver: **NDI Video Monitor**, part of NDI Tools, is the simplest, and OBS gains an "NDI Source" through the [DistroAV](https://github.com/DistroAV/DistroAV) plugin. MoonLight appears under the driver's `sourceName`, or the device's own name when that is blank.

## Playing an HLS stream

Open the `url` the HLS driver shows in VLC or Safari, or hand it to an Apple TV. HLS buffers whole segments, so the picture runs two to five seconds behind the lights. For a view that keeps up, use RTSP.

## Playing an RTSP stream

Any RTSP player opens the `url` the RTSP driver shows. These two are what the driver is tested against:

```sh
ffplay -fflags nobuffer -flags low_delay rtsp://<device>:554/
vlc --network-caching=100 rtsp://<device>:554/
```

`-fflags nobuffer -flags low_delay` makes ffplay show each frame as it arrives instead of filling a buffer first. VLC buffers about a second by default, which `--network-caching=100` shortens.

To watch the lights on a monitor, scale the grid up with nearest-neighbor, so each light stays a crisp square instead of a blurred blob:

```sh
ffplay -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 \
       -vf "scale=768:768:flags=neighbor,format=yuv420p" rtsp://<device>:554/
```

`-probesize 32 -analyzeduration 0` skips the startup probing, so the picture appears at once. `format=yuv420p` silences ffplay's `No accelerated colorspace conversion` notice, which is about the player's own window and says nothing about the stream.

### When an RTSP stream stays silent

- **One viewer plays at a time.** A new connection takes the session over, and the displaced player sees its connection close.
- **The video travels over UDP.** The player talks to port 554 over TCP, and the frames go to the UDP port it names. A network that blocks that port leaves the stream silent while the session looks connected.
- **The rate is what the device renders.** A heavy effect that renders at 8 fps streams at 8 fps. Each module's tick time in the app says which one spends the time.
