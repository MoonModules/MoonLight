# Plan: HDMI scaled by the display controller

Plan of 2026-11-23, a Feature, for approval. **Goal: a Linux board's HDMI output costs no CPU for scaling or copying**, as a video player's does.

## What exists

- **The Linux HDMI output** (`platform_host_hdmi.cpp`) runs a thread that takes the layer's frame, scales it nearest-neighbor in software to the screen, with bars where the aspect differs, and copies each row into a kernel dumb buffer, about 8 MB a frame at 1080p.
- **Measured on a NanoPi R3S on 2026-11-23**: the HdmiDriver's tick 3.6 to 9 ms a frame at 640×360 shown on 1280×720, beside 29 to 50 ms of rendering.
- **The display controllers scale in hardware**: a DRM plane has a source rectangle and a screen rectangle of their own, and Rockchip's VOP, the Pi's HVS and Intel and AMD display engines scale between them, the way video players (VLC, mpv, GStreamer's kmssink) show a small video full screen with the CPU idle.

## End state

- The output hands the display a framebuffer the size of the layer and sets the plane to scale it onto the screen's fitted rectangle; the CPU writes the layer's pixels once, into that buffer, and pages between two of them.
- A display that refuses the plane's scaling, found by a test-only atomic commit, keeps today's software scaling, so every board still shows the picture.
- macOS, Windows and the ESP32-P4's LT8912B output stay as they are.

## Steps

1. ✅ **The primary plane, scaled (2026-11-23):** the display controller scales the frame on the primary plane, checked on the R3S's monitor, [the display scales](../../src/platform/host/platform_host_hdmi_linux.cpp).
2. ⏳ **Write once.** The driver writes the layer's RGB straight into the back buffer as XRGB8888 and flips on the next vertical blank: one pass over the layer's pixels, no screen-sized copy.
3. ⏳ **Measure and document** on the R3S, a Pi 5 and an Intel mini PC: the HdmiDriver's tick before and after, at 640×360 on 1080p; the HDMI page names the path and the fallback.

## Open questions

- ~~Whether the hardware scaler softens the lights.~~ On the R3S the picture looks as sharp as the software path's (product owner, 2026-11-23), so no `sharp` control.
- Whether the plane's buffer can be the layer's buffer itself, skipping even the one write, for a layer stored as XRGB8888.

## What it removes

The screen-sized scaling and copy on every board whose display accepts a scaled plane; the software path stays as the fallback.
