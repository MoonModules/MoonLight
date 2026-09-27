# Audio DSP roadmap — source-seam extensions + adaptive noise gate (design study)

> Forward-looking design study (backlog, present-tense-exempt). The *shipped* audio path is
> documented present-tense in `src/core/services/AudioService.h`'s `///` (and its generated moxygen
> page); this study holds the **prior-art analysis** and the **not-yet-built** extensions the
> module's `///` credit points at.

## Prior art studied (credit by name)

Audio-reactive lighting is a long-standing idea in the LED-controller world (WLED-MM and MoonLight
are the closest lineage). MoonLight's audio path is its own implementation, designed from the INMP441
datasheet and standard DSP — not traced from any one project — but three people's thinking is studied
here with respect and credited by name (the *Industry standards, our own code* principle: study hard,
write fresh).

**Frank (softhack007)** — main author of the WLED-MM audioreactive usermod (the most-used open-source
audio-reactive LED implementation), a direct ancestor of the ideas this module learns from. The
product owner worked alongside Frank for years on WLED-SR / WLED-MM before MoonLight and MoonLight.
His concept is the worked example in the *Adaptive noise gate* section below: his idea, our analysis,
written fresh against our architecture.

**Troy (troyhacks)** — MoonModules team; keeps a WLED-MM fork where he reworked the audioreactive DSP
onto Espressif's **esp-dsp** library ("stupid fast compared to ArduinoFFT"), very low latency on S3/P4.
His contribution has two parts:
- *esp-dsp FFT.* Troy uses esp-dsp's **radix-4** real FFT (`dsps_fft4r_fc32`) with a Blackman-Harris
  window. This validates the path MoonLight is already on — **we use esp-dsp too**, the **radix-2**
  float real FFT (`dsps_fft2r_fc32`) in `platform_esp32_i2s.cpp`. Same library; the one open
  optimisation is **radix-4 vs radix-2** (fewer butterfly stages, log₄N vs log₂N — a measure-then-
  maybe tune-up, not a gap; today the float FFT on the FPU is well inside one tick). Two adjacent,
  *not-yet-adopted* options for the record: (a) esp-dsp's **int16 / fixed-point** path uses the
  **built-in FFT instructions on S3/P4** — the lever for low-power FPU-less chips (C3/S2); we run
  float today because our targets have an FPU; (b) Espressif's standalone **`dl_fft`** component does
  *only* FFT without esp-dsp's shared twiddle tables — the right pick if a future build wants the FFT
  without the rest of esp-dsp (we take the whole esp-dsp because we also want its DSP primitives).
- *Biquad pre-filters.* Before the FFT, Troy runs the samples through **biquad** HP/LP/peaking filters
  via esp-dsp's `dsps_biquad_f32`, coefficients designed in EarLevel's Biquad Calculator v2. Squarely
  industry-standard (the biquad / second-order section is *the* canonical EQ building block; the Audio
  EQ Cookbook is the reference). Our pipeline does one fixed DC-blocker HP (~40 Hz); Troy's work shows
  the next step — a **configurable biquad chain** (HP to kill rumble, LP to tame aliasing, optional
  peaking to lift the mids the FFT under-reports). **Assessment: the biquad pre-filter chain is the
  higher-value idea to adopt** (improves spectral accuracy with off-the-shelf primitives), and it
  composes cleanly with the adaptive gate below (a learned gate on a cleanly-filtered signal beats one
  on a raw signal).

**LedFx** (Python, host-side) — a network LED effect engine whose whole purpose is audio reactivity,
running on a PC and streaming pixels to WLED-class devices. Different architecture to ours (the host
renders, the device receives; we already interoperate through Art-Net / E1.31 / DDP in both
directions), so most of it does not transfer. Two pieces of its *analysis* do, and both are studied
in § Band spacing below: its **mel/bark band spacing** with hand-tuned variants, and its
**per-band asymmetric smoothing**. Worth naming because it reached those two independently of the
WLED lineage above, and its own source comments are unusually candid about which variants work.

**Damian Schneider (DedeHai)** — WLED core dev; WLED's audioreactive usermod carries an integer /
fixed-point FFT path (~1.5 ms on a C3, >10× ArduinoFFT on FPU-less chips). The consensus (Troy + Frank)
is that with esp-dsp FFT + biquads, **fixed-point is not necessary on FPU chips** (S3/P4) — MoonLight's
exact position: float on FPU targets, the int16 / `dl_fft` hardware path reserved for low-power chips.
DedeHai's current audio experiment is a PoC MSGEQ7-based path (offloading the spectrum to a dedicated
analyser chip) — a different point in the same space, noted for completeness.

## Source-seam extensions (widen what feeds the pipeline)

All of the following widen the **source seam** — what feeds the pipeline — leaving the DC-blocker / RMS
/ FFT / band analysis untouched. In roughly increasing hardware complexity:

- **I²S with MCLK, for line-in — largely SHIPPED.** The INMP441 is self-clocked (no MCLK); a line-in
  codec needs a master clock the ESP32 drives. The module already carries an `mclkPin` control (the
  I²S peripheral drives MCLK for a line-in ADC; a codec board uses the codec's own MCLK). Remaining
  source-seam work is the other three types below.
- **PDM mics** — a different I²S sub-mode (the IDF `i2s_pdm` driver), a variant behind the same
  platform read.
- **Analog line-in.** DedeHai got analog input working on the S3; Troy got it working in ParrotRadio.
  Troy's testing-confidence nuance worth recording: he considers his ParrotRadio analog path
  better-exercised (he actually recorded + played back through it), whereas an unlistened-to analog
  path "may not be as accurate as it looks." **If MoonLight adopts analog line-in, validate by
  listening**, not just by watching the level meter.
- **I²C-configured codecs (e.g. ES8311).** Do **not** hand-roll each codec's register config — pull in
  Espressif's **`esp_codec_dev`** component (carries option tables for many codecs), supporting "a
  bunch more codecs for free." The *Industry standards, our own code* call applied to codec bring-up.

Troy also has **DSP boards** (I²S front-ends "way beyond regular codecs") — recorded so the line-in /
codec work leaves room for that class of source.

## Adaptive noise gate (softhack007's concept, our analysis)

**Partly built.** `floor` is now a real silence threshold on both the level and the band paths: below
it a band reads zero AND is not learned from, which is what stopped the learner amplifying an empty
room to full scale. What remains from the design below is the ADAPTIVE half: hysteresis, the
asymmetric open-fast/close-slow timing, and a learned threshold rather than a set one.

The original framing, kept because the remaining work is judged against it: replace the borrowed
`squelch`/`noiseFloor` knob ("a WLED-SR workaround, not a real gate") with a proper adaptive gate. From softhack007 (granted permission to analyse); the assessment is ours.

**The concept:** a standard [noise gate](https://en.wikipedia.org/wiki/Noise_gate) (below a threshold
the signal is silenced, above it passes), **asymmetric bang-bang timing** (open fast, close slow;
hysteresis avoids threshold chatter), driven by a **new "detect silence" function** (the explicitly
unfinished part). Leave the GEQ/FFT bands untouched (the gate acts on the time-domain signal). The
closing pre-condition should be **relative** ("percentage of average signal"), not an absolute count.
Optionally feed the gate **compressed samples** (sqrt/log) so the threshold behaves perceptually.

**Five design constraints (the load-bearing part):** (1) samples are signed, arbitrary magnitude —
scaling to an effect range is AGC's job, not the gate's; (2) every `abs()` must be justified (a
rectify discards sign/phase); (3) prefer relative factors to absolute thresholds (the one allowed
absolute: changes < 2 counts are sampling noise); (4) smooth before thresholding; (5) every filter
adds delay — total audio delay must stay < 30 ms.

**Verdict: yes, directionally — it's squarely industry-standard** (a hysteresis gate with
fast-attack/slow-release is how studio gates, radio squelch, and voice-activity detectors all work),
and moves us off the borrowed `squelch` constant. The relative-threshold insight (constraint 3) is the
valuable core: a gate keyed to a *learned* floor self-calibrates to whatever source is connected.
**Two cautions:** (1) timing is tight — a 512-sample block at 22050 Hz is already ~23 ms, leaving
< ~7 ms of the 30 ms budget; any smoothing must be cheap (one-pole) and proven on hardware; (2) it
overlaps the already-scoped per-band floor, so decompose and adopt in steps, don't overhaul.

**Per-band floor overlap:** the backlogged per-band noise floor learns each band's idle baseline
(frequency-domain floor — kills a steady tone like the bench's ~258 Hz hum); the proposed gate answers
"is there any sound at all" (time-domain floor). Complementary halves, not competitors.

**Decomposition (cherry-pick, most value early):**
1. **Per-band noise floor (already backlogged)** — ship first; frequency-domain half, smallest change,
   kills the concrete hum. Independent.
2. **Relative thresholds reusing the RMS we already compute** — `computeLevel` already produces a
   per-block RMS (an envelope estimate, and the one justified `abs()` under constraint 2). A
   learned-floor follower over that RMS with open/close as *factors* of it is a small, host-testable
   addition needing no new DSP stage and no extra delay. **The cherry to pick.**
3. **Hysteresis + asymmetric timing** — two time-constants on that follower plus a close-hold; where
   the < 30 ms budget gets measured for real.
4. **Defer until proven:** log/dB-domain thresholds (downstream `magToByte` already compresses
   perceptually), a true soft gate (0..1 gain vs hard 0/1).

**Eventually retires:** the `floor` knob's hard-squelch role — `floor` becomes the *display*
noise-floor only (the dB-window bottom in `magToByte`), while the learned gate decides "is there
sound." A clean subtraction, but the *end* of the path, not the first step.
