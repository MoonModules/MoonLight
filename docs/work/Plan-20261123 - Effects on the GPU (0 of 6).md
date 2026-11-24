# Plan: effects on the GPU

Plan of 2026-11-23, a Feature, for discussion. **Goal: a 1080p screen at full frame rate from a Linux board or a computer**, by computing a big layer's pixels where pixels are computed fastest.

## What exists

- **Every effect runs on the CPU, light by light**, the design that suits an ESP32 and tens of thousands of lights. Measured on a NanoPi R3S on 2026-11-23: 1920×1080 at 4 fps, 1280×720 at 10, 640×360 at 38, each one layer; about 120 ns a light a frame on one A55 core.
- **The boards MoonLight runs on carry GPUs**, each with a Mesa driver on Linux:

  | Board | GPU | Mesa driver | API |
  |--|--|--|--|
  | Pi 1 to 3, Zero 2 | VideoCore IV | vc4 | OpenGL ES 2.0 |
  | Pi 4 | VideoCore VI | v3d | OpenGL ES 3.1, Vulkan |
  | Pi 5 | VideoCore VII | v3d | OpenGL ES 3.1, Vulkan |
  | NanoPi R3S (RK3566), R5S (RK3568) | Mali-G52 | Panfrost | OpenGL ES 3.1 or newer, Vulkan in newer Mesa |
  | NanoPi R28S (RK3528) | Mali-450 | Lima | OpenGL ES 2.0 |

  Intel and AMD mini PCs and every Mac and Windows PC carry one too, which ANGLE reaches as OpenGL ES over Metal on macOS and Direct3D on Windows (step 2), so one shader source runs on all of them. Their CPUs already render on every core, through the render workers ([Layers rendered in parallel](Plan-20261123%20-%20Layers%20rendered%20in%20parallel%20(4%20of%204).md)). projectM, which belrik's milk-ddp streams into MoonLight over DDP, renders Milkdrop presets on them at 60 fps, 128×128 on 40% of one NanoPi core.
- **An ESP32 has no shader GPU**; the P4's PPA is a 2D blitter. This plan is for the host platform alone.

## End state

- A **shader effect**: a fragment shader in GLSL ES runs once per light of its layer on the GPU, given the time, the layer's size, the audio frame's bands and the beat as uniforms, the shader world's textbook interface (Shadertoy's), so a shader written there ports with its uniforms renamed.
- The result reaches the layer like any effect's, so modifiers, the composite and every driver work as they do; a layer shown on HDMI alone can skip reading it back.
- Off-screen and without a desktop: EGL with GBM on Linux, as kmscube does, so a headless board renders on its GPU.

## Steps

1. ⏳ **A spike on the R3S**: EGL and GBM, no X, a plasma shader into a 1920×1080 texture, read back with `glReadPixels`; measure the frame rate and the readback's share. It decides whether readback is cheap enough for every driver or HDMI wants the direct path.
2. ⏳ **The platform seam**: `platform::gpu` opens a context and runs a fragment shader into a buffer the size of a layer; Linux through EGL, macOS and Windows through ANGLE's OpenGL ES; a board without one reports it and the effect shows its status.
3. ⏳ **ShaderEffect**: the shader source as a control or a file, the uniforms filled each frame, the output read into the layer. A unit test runs a constant-color shader on the host and checks the layer.
4. ⏳ **The direct path to HDMI**: a layer that only HDMI shows renders into the scanout buffer of [HDMI scaled by the display controller](Plan-20261123%20-%20HDMI%20scaled%20by%20the%20display%20controller%20(2%20of%203).md), no readback.
5. ⏳ **A first catalog**: a handful of effects as shaders (plasma, noise, a tunnel), side by side with their CPU versions on the R3S, and the shader page in the docs.
6. ⏳ **One window for every desktop (added 2026-11-24).** The HDMI output shows through a window on Windows and macOS, each in its own platform code, and straight through the kernel on a Linux console; a Linux machine running a desktop has neither, since the desktop holds the display. The surface step 2 opens on a desktop is a window on all three, so the HDMI output takes that one: a Linux desktop gets its picture, with the same second-display rule and `ownScreen` switch, and the two window implementations of `platform_host_hdmi.cpp` and `platform_host_hdmi_mac.mm` come out.

## Open questions

- The API: OpenGL ES through EGL and ANGLE is the widest reach today; Vulkan or WebGPU (wgpu-native) are the newer textbooks and heavier to start with.
- Whether MoonLive could compile a script to GLSL, so one effect runs on a light strip's CPU and a screen's GPU alike, or shaders stay their own kind of effect.
- How a shader reads the previous frame (a feedback texture, as Milkdrop and Shadertoy's buffers do) for trails and fades.

## What it removes

Step 6 removes the two per-platform HDMI windows for one; the rest adds a kind of effect and a platform seam, for the screens the CPU path cannot fill.
