# Plan: HDMI scaled by the display controller

Plan of 2026-11-23, a Feature, for approval. **Goal: the HDMI output costs no CPU for scaling, on a Linux board as on a Mac or Windows PC**, as a video player's does.

## What exists

- **The Linux HDMI output** (`platform_host_hdmi.cpp`) runs a thread that takes the layer's frame, scales it nearest-neighbor in software to the screen, with bars where the aspect differs, and copies each row into a kernel dumb buffer, about 8 MB a frame at 1080p.
- **Measured on a NanoPi R3S on 2026-11-23**: the HdmiDriver's tick 3.6 to 9 ms a frame at 640×360 shown on 1280×720, beside 29 to 50 ms of rendering.
- **The display controllers scale in hardware**: a DRM plane has a source rectangle and a screen rectangle of their own, and Rockchip's VOP, the Pi's HVS and Intel and AMD display engines scale between them, the way video players (VLC, mpv, GStreamer's kmssink) show a small video full screen with the CPU idle.

## End state

- The output hands the display a framebuffer the size of the layer and sets the plane to scale it onto the screen's fitted rectangle; the CPU writes the layer's pixels once, into that buffer, and pages between two of them.
- A display that refuses the plane's scaling, found by a test-only atomic commit, keeps today's software scaling, so every board still shows the picture.
- macOS and Windows hand the window server a frame at the layer's size and let it scale; the ESP32-P4's LT8912B output scales with its PPA.

## Steps

1. ✅ **The primary plane, scaled (2026-11-23):** the display controller scales the frame on the primary plane, checked on the R3S's monitor, [the display scales](../../src/platform/host/platform_host_hdmi_linux.cpp).
2. ❌ **Write once, closed as measured (2026-11-24).** The driver would write the layer straight into the back buffer as XRGB8888, saving the handoff's copy and the output thread's conversion. Measured on the R3S with step 1 in place, 640×360 at 74 fps: the frame 13.4 ms, Effects 9.5 to 10.3 ms, the HdmiDriver's tick 2.8 to 3.2 ms. That tick is the draw, the layout and the correction of 230,400 lights that every driver does, and the copy, about 0.3 ms. The saving is about 2% of the frame, against a lend-and-return pair in every platform's HDMI seam and a second handoff for the software fallback (product owner, 2026-11-24). A cheaper draw for big frames is the GPU plan's.
3. ✅ **Measure and document (2026-11-24)** at 640×360 on 1920×1080, the HdmiDriver at 30 fps. Its tick is wall-clock time averaged over the render frames, so it compares one board before and after, and a busy board's figure includes waiting for a core.
    - **Pi 5:** 384 to 395 µs before (dev.146, scaled in software) and 289 to 299 µs after (dev.195), at about 970 render fps; the log reads `the display scales 640x360 onto 1920x1080 on plane 83`, and 900×600 onto a 3840×2160 screen scales as well.
    - **R3S:** 2.8 to 3.2 ms after, at 74 render fps, against 3.6 to 9 ms before on 1280×720 (step 1's record).
    - The [HDMI card](../../src/light/drivers/HdmiDriver.h) names who scales and the log line for each path.
4. ⏳ **macOS and Windows: the OS scales (added 2026-11-24).** Both hand the screen a frame at the layer's size and let the window server scale it on the GPU: on macOS a Core Animation layer whose contents is the frame, aspect-fit, with a nearest-neighbor magnification filter; on Windows a DXGI swap chain with buffers at the layer's size, stretched by the compositor, the bars placed by the window. MoonLight's own scaler then serves the Linux fallback alone. Measured on the product owner's MacBook and the Windows mini PC, the HdmiDriver's tick and the output thread's share before and after.

## Open questions

- **Effects reads unsteady on the Windows mini PC (Intel N100, measured 2026-11-24, five layers at 640×360, the clang-cl build).** With the HDMI driver off Effects takes 4.3 ms and MoonFlock's layer 4.1 ms; with it on, which adds work, Effects takes 2.9 ms and that layer 2.2 ms. The MSVC build showed the same: Effects 2.3 ms beside a 4.3 ms composite, 4.9 ms with Drivers disabled. A hypothesis, untested: Windows' Balanced power plan clocks the cores down while the frame leaves them idle. To test: the same run on the High performance plan, and the per-core frequency beside the layer times.
- ~~Whether the hardware scaler softens the lights.~~ On the R3S the picture looks as sharp as the software path's (product owner, 2026-11-23), so no `sharp` control.
- Whether the plane's buffer can be the layer's buffer itself, skipping even the one write, for a layer stored as XRGB8888.

## What it removes

The screen-sized scaling and copy on every board whose display accepts a scaled plane; the software path stays as the fallback.
