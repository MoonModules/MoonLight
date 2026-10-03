# Plan: RTSP video out on the P4

## Why

HLS delays a stream by seconds: it ships whole segments, and a player buffers several before it starts. RTSP carries each frame as it leaves the encoder, so a viewer sees it far sooner. The factor is what verification step 3 measures.

## What already exists

The P4 encoder path is most of the work, and it is built:

- `platform_esp32_h264.cpp` drives the hardware encoder and holds each frame's **raw H.264 NAL units** in `nal_`, with a keyframe flag and a 90 kHz PTS already computed, before any muxing.
- `platform::UdpSocket` is the wire ArtNet and DDP send on, proven at frame rates.
- `platform::TcpServer` and `TcpConnection` serve the web UI and MQTT.

RTSP therefore reuses the encoder and both sockets, and adds the packetisation and the session protocol.

## Scope of the first version

**P4 only.** `hasRtsp` is true on the P4, so the driver compiles in there and its card appears there.

**UDP/RTP only.** Interleaved TCP arrives when a network blocks the negotiated port pair, which is the trigger that earns the second path.

## Steps

### 1. A platform seam that hands out frames

A narrow seam beside `EncoderConfig`:

- `rtspFrameReady()` reports whether a fresh encoded frame is waiting.
- `rtspTakeFrame(const uint8_t** nal, size_t* len, uint32_t* pts90, bool* keyframe)` hands out the frame the encoder just produced, by pointer.

Both are gated by `hasRtsp`. On the P4 they read the same `nal_` buffer the muxer reads, so one encode feeds HLS and RTSP together.

### 2. RTP packetisation, RFC 6184

One NAL becomes one RTP packet while it fits the MTU, and an oversized NAL becomes a run of FU-A fragments. The header carries the sequence number, the 90 kHz timestamp the encoder already computed, and the marker bit on a frame's last packet.

Each keyframe is preceded by its SPS and PPS, so a client joining mid-stream decodes from the next keyframe onward.

### 3. The RTSP control server

Four verbs on a TCP listener, RFC 2326: `OPTIONS`, `DESCRIBE` (answering SDP that names H.264 and the profile), `SETUP` (negotiating the client's RTP port pair), `PLAY` and `TEARDOWN`. One session at a time.

### 4. The driver

`RtspDriver.h` beside `HlsDriver.h`, reusing `DriverBase`'s correction and window blocks. Controls: `targetFps` and `scale`, mirroring HLS, plus a read-only `url` a viewer copies into VLC or ffplay, and a status line naming the connected client.

### 5. Tests

- RTP packetisation is pure data: a NAL under the MTU makes one packet, an oversized one fragments and reassembles, and the marker bit lands on the last packet of a frame.
- The RTSP verb sequence, driven against a fake connection: `SETUP` precedes `PLAY` for a session to start, and `TEARDOWN` releases it.
- A scenario covering the driver's lifecycle, as the HLS driver has.

### 6. Documentation

A card on the drivers page saying which boards carry it and what to point at it, a `## RTSP, details` section for the transport choice, and a line in the FAQ's "The HLS stream lags by seconds" entry naming RTSP as the lower-latency remote view.

## Verification

1. Host tests for the packetiser and the verb sequence.
2. `ffplay rtsp://<device>/` on the bench P4, showing the wall.
3. **The number that justifies this**: glass-to-glass delay measured the same way for HLS and RTSP, on the same P4 and the same content, photographed side by side. No factor is claimed anywhere until this is measured, and this plan is realized when it is.
4. A second viewer connecting takes the session over, and the displaced one sees its connection close. This replaces the refusal the plan first called for: a player that vanishes without TEARDOWN leaves a socket open and silent, so refusing new arrivals strands the stream for as long as TCP takes to notice.
5. The render tick holds while streaming: the encode already happens for HLS, and packetisation stays off the render thread.

## Risks

- **The send path runs on a nonblocking tick**, at RTP's packet rate. Where the send falls behind it drops whole frames at the source and the client sees fewer, the rule [the preview transport](../../moonmodules/light/drivers.md#preview) already follows.
- **UDP wants a reachable port pair.** A network that blocks it leaves the stream silent, which reads as a fault. The status line says a client is connected and how many packets have left the device, so the difference is visible.
- **PSRAM.** The NAL buffer is 128 KB today and shared with the muxer, so a second reader releases it within the encoder's frame.
