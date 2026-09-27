# Plan — NDI output: MoonLight as a video source

## The licensing constraint, and what it dictates

MoonLight is GPL-3.0. The NDI runtime is proprietary and its licence requires a redistributor's own
EULA to carry NDI's terms forward, which GPL-3 forbids. **So MoonLight must not redistribute it.**

This is not a blocker; it is a design constraint MoonLight has already met once. **Npcap** is the
precedent: proprietary, required for raw L2 on Windows, and
[platform_desktop.cpp:814](../../../src/platform/desktop/platform_desktop.cpp) resolves `wpcap.dll`
with `LoadLibrary` rather than linking it, declaring the five functions with pcap's own signatures
rather than including `pcap.h`. The user installs Npcap; the panel-card tutorial says so; the binary
builds and runs identically without it and reports raw send unavailable.

NDI follows exactly that arrangement:

- **The user installs the NDI runtime.** We bundle nothing and ship no SDK.
- **Resolve at run time** (`LoadLibrary` on Windows, `dlopen` elsewhere), never link.
- **Declare the needed functions with the SDK's own signatures**, never include its headers — so the
  SDK is not a build requirement for CI or for any contributor.
- **Degrade visibly** when it is absent (ADR 0002, allocate-and-degrade): a status line, not a
  failure.

## Design

### The platform seam

NDI is a host capability, so it lives behind `platform::` like every other one, and the driver never
sees a `dlopen`. Following `hasNamedNetInterfaces`, each platform declares a `constexpr bool hasNdi`
in its own `platform_config.h`: **true on desktop, false on ESP32** (no runtime to load, and the
encode cost does not belong on a microcontroller).

The seam is deliberately tiny — four functions, mirroring the pcap surface:

```cpp
bool ndiAvailable();                                  // runtime present and loaded
bool ndiSenderOpen(const char* name);                 // create a named source
void ndiSenderClose();
bool ndiSendFrame(const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t fps);
```

`ndiSendFrame` takes a tightly-packed RGB buffer and the geometry; the platform layer converts to
NDI's frame struct. A stub in the no-NDI build returns false from everything, which is what keeps
`mm_tests` and every ESP32 target compiling untouched.

### The driver

`NdiDriver : DriverBase`, registered like the others in `main.cpp`, gated on `hasNdi` so it is
offered only where it can run. It follows `PreviewDriver` closely, which is the existing driver that
also turns the rendered buffer into a frame for a remote consumer:

- **Controls**: `sourceName` (what appears in OBS's source list, defaulting to the device name),
  `fps` (a ceiling, as in PreviewDriver), plus the inherited correction controls.
- **`tick()`**: rate-limit to `fps`, read the source buffer, hand it to `platform::ndiSendFrame`.
  `MM_NONBLOCKING`, and no allocation in the tick — the RGB staging buffer is sized in `prepare()`.
- **`prepare()`**: size the staging buffer to the layer, open the sender, set the status. Re-runs on
  a geometry change, exactly as `affectsPrepare` governs elsewhere.
- **Status**, which is the whole user-facing diagnostic:
  - no runtime → `NDI runtime not installed`
  - open failed → the reason
  - running → `sending <w>x<h> at <fps> fps as '<name>'`

### What this deliberately does not do

- **No audio.** NDI carries it; MoonLight has no video-audio pairing to send.
- **No NDI HX / compression choice.** Ship the default; add a control only if a user needs it.
- **No receive.** Its own feature.

## Steps

1. **The platform seam.** Declare `hasNdi` in both `platform_config.h` files and the four functions
   in `platform.h`. Implement the runtime load in `platform_desktop.cpp` beside the Npcap block,
   reusing its shape. ESP32 needs no implementation (the flag is false).
   *Tests:* the stub path — `ndiAvailable()` false with no runtime, and every call safe.
2. **`NdiDriver`.** The driver above, registered in `main.cpp` behind the `hasNdi` gate.
   *Tests:* controls round-trip; a prepare with no runtime reports the status and does not crash;
   the frame conversion is pinned against a known buffer.
3. **Docs.** A driver card in `docs/moonmodules/light/drivers.md`, and a short section in the
   panel-cards tutorial's sibling — where to install the runtime per OS, exactly as §6.1 does for
   Npcap.
4. **Bench.** Install the NDI runtime and OBS with the DistroAV plugin; confirm MoonLight appears as
   a source by name and that the wall's image arrives. **This is the gate: an output path is not
   verified until a receiver shows the frames.**

## Risks

1. **Nothing is verifiable without a receiver.** Steps 1 and 2 can be written and unit-tested blind,
   but "it works" requires step 4. The plan is ordered so the untestable claim comes last.
2. **The SDK's function signatures must be right without including its headers.** Getting one wrong
   is a silent crash rather than a compile error — the same hazard the Npcap block carries, and the
   reason its comment names the five functions explicitly. Take them from the SDK's public docs and
   record where each came from.
3. **Frame format.** NDI wants a specific FourCC and stride; a mismatched stride shows as a skewed
   image rather than an error. Pin the conversion with a unit test.
4. **CPU cost of the encode**, which runs on the desktop render thread. Measure before claiming a
   frame rate; the `fps` ceiling is the mitigation.

## Verification

- `cmake --build build` (zero warnings) and `ctest` on a machine with **no** NDI runtime, proving the
  degrade path is the default one CI sees.
- The same, on a machine **with** the runtime.
- OBS (DistroAV) on the same machine, then on a second machine, confirming the cross-machine claim
  that chose NDI over Spout in the first place.
- The product owner's eyes on the OBS preview.
