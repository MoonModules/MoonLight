# Plan: audio that answers the music

Draft for the product owner's approval. The goal is the best visible answer to the music: silence is dark, a quiet passage stays quiet, a loud one is loud, the bars sit balanced across the spectrum, a kick drum is found whatever song is playing, and the beat lands on the beat. The least to set follows from it, since a level that learns the song beats any knob set for one song. The analysis stays one pipeline on every board; measurement-grade accuracy (an analyzer's calibrated bars) is not the goal and goes to the backlog.

The yardstick is music information retrieval, the field whose libraries (Essentia, librosa, aubio) describe music to a machine: loudness, spectral balance, onsets per band, and tempo with beat phase, in that order. Steps 2 to 5 build one each; step 1 makes the bands those descriptors are read from. Two things the field settles that this plan follows: a band per instrument is not a thing (instruments overlap in frequency, and separating them is a neural net on a GPU), while the drum kit does separate by range; and genre presets lose to adaptation (a floor, a gate and a gain that learn the dynamics give a quiet classical passage and a loud rock chorus the same treatment with no switch).

## The design basis

- **One pipeline, no engine select** ([CLAUDE.md, principle 3](../../CLAUDE.md#principles)): one analysis path, the same on every chip.
- **The textbook.** A band is the power in its slice of the spectrum, normalized so that white noise reads flat (Welch's periodogram). A noise gate opens fast and closes slow with hysteresis, as every studio gate does. Automatic gain is one offset that holds the loudest band at the top, with a fast attack and a slow release, as a compressor's make-up gain does.
- **Study, credit, write fresh.** MoonLight's audio path is its own implementation, from the INMP441 datasheet and standard DSP; the people whose thinking it studies are credited by name below and in the header's `@attribution`.
- **Measured on synthesized signals** on the desktop, then judged by eye on the bench: the product owner's eyes are the measurement for a light show.

## What exists

- **`AudioService`** ([AudioService.h](../../src/core/services/AudioService.h)): a 512-sample block, a DC blocker, a Hann window, esp-dsp's FFT on the ESP32 and a DFT on the desktop, a frame every 23 ms.
- **The bands** ([AudioBands.h](../../src/core/services/AudioBands.h)): sixteen log-spaced bands, each the loudest bin of its slice. Pink noise, the spectrum real music resembles, falls toward the treble on that estimator, which the learner in `BandConditioner` then corrects per band.
- **The level path**: `floor` is a silence threshold and a window bottom at once; `levels` switches between a manual `gain` and a learned window; `BandConditioner` and `LevelConditioner` learn a floor and a peak each and apply a compressor's ratio. Three mechanisms share the one job of "show the music at the right size", from three places.
- **`OnsetDetector`**: one `onset` for a hit anywhere in the spectrum, so a kick and a snare look alike.
- **The readers**: thirteen effects read `bands[16]`, the WLED sync packet carries sixteen, and MoonLive reads `audioBand(i)` and `audioPeakHz`.

## Prior art

Audio-reactive lighting has a lineage (WLED-SR, WLED-MM, MoonLight), and four people's thinking is weighed here, their ideas and our analysis.

**Frank (softhack007)**, main author of the WLED-MM audioreactive usermod, who the product owner worked alongside for years: the adaptive noise gate that step 2 builds. His concept, analyzed with his permission: a standard noise gate on the time-domain level, open fast and close slow with hysteresis, keyed to a *learned* floor so it self-calibrates to whatever source is connected, with thresholds as factors of that floor rather than absolute counts. His constraints bind step 2: the gate decides "is there sound" and leaves scaling to the gain; it reads the RMS the level path already computes, so it adds no delay; the whole chain stays under 30 ms, and a 512-sample block at 22,050 Hz already spends 23 of them, so every smoother is a one-pole.

**Troy (troyhacks)**, MoonModules: [his AudioReactive rewrite](https://github.com/troyhacks/WLED), commit `ed544259` of 2026-10-25, on an ESP32-P4, checked against a hardware analyzer. Most of it chases measurement accuracy (a 4096-point transform, biquad pre-filters, calibration from a measured impulse response, 31 third-octave bands, self-tests at boot), which this plan leaves to the backlog. Two decisions carry over:

| Decision | What it does | Why it matters here |
|---|---|---|
| **A unit contract** | Every band is a power density normalized by the sample rate and the window's power, so white noise reads flat by definition and pink noise falls 3 dB an octave | A test can assert "balanced bars" instead of a learner correcting for the estimator |
| **Four quantities, kept apart** | The level is the RMS of the fresh audio in dB; the floor snaps down at once and creeps up with a 10 s time constant, only while the level sits within 3 dB of it; the gate opens at `squelch` dB above the floor and shuts 4 dB lower; the AGC is one dB offset holding the loudest band at 0 dB, 20 ms attack, 1,500 ms release, frozen while the gate is shut | Nothing reads its own output, so the deadlocks of one number doing four jobs are impossible; our three mechanisms approximate this from three places |

A later version, described on Discord on 2026-10-28 and unpushed, replaces the FFT with filter banks, which time a hit within a tick rather than within a 23 ms block; it changes no step here and is [the trial in the backlog below](#backlog), run once the steps give it a bench.

**LedFx**, the host-side effect engine whose whole purpose is audio reactivity: its mel-spaced bands and per-band asymmetric smoothing, reached independently of the WLED lineage. Step 1 keeps log spacing (mel and log coincide above 1 kHz and the sixteen readers depend on the layout); the asymmetric smoothing is the attack and release step 2 gives the gain.

**Damian (DedeHai)**, WLED: the fixed-point FFT for chips without an FPU. Every MoonLight audio target has one, so the float transform stays; the int16 path is the lever if a C3 ever analyzes audio.

## Steps

Each step ends in a desktop test and a bench look.

### 1. Balanced bars by construction ⏳

- A band is the power summed over its slice, normalized by the sample rate and the window's power, in `AudioBands.h`; the loudest-bin estimator goes.
- Desktop tests: synthesized white noise reads every band within 1 dB of flat; pink noise falls 3 dB an octave; a sine lights one band; the dominant peak reads as before. Flux and onset are recomputed from the new bands, and a synthesized kick still fires `onset` while steady pink noise does not.
- Bench: pink noise from a phone through the microphone, and the bars sit level.

### 2. One level path, no knobs ⏳

- Four quantities, as the prior art has them: the level in dB, a floor that snaps down and creeps up only near the floor, a gate with hysteresis that opens fast and closes slow (Frank's adaptive gate, under his constraints above; the silence threshold `floor` already gives is its set half, the learned floor its adaptive half), and one dB offset holding the loudest band at the top, frozen while the gate is shut.
- A setting stays when the bench shows a better result with it than without, and goes when the automatic path matches it. `floor` stays, the one setting a room may need, with a default that suits a MEMS microphone; `levels`, `gain`, the conditioner's ratio and maximum gain, and the two learned tables are each tried against the automatic path on the bench. A choice that belongs to an effect (which signal it follows: a band, the bass, the onset, the beat) sits on the effect's card, part of the effect rather than a tuning.
- Tuned in a quiet room first, then in a noisy one with a varying low-frequency ambient, the case that made the earlier self-calibrating prototype hard to settle.
- The card shows the four quantities, so a gated room reads as "gated at −52 dB" rather than as a dead audio path, the bug report a high default floor earns today.
- Bench: silence is dark, a quiet passage stays quiet, a loud one is loud, a song that starts soft and ends loud fills the rig at both ends, and the StadBeest's eyes follow on the bench network.

### 3. The kick is found in any song ⏳

- Three sums beside the sixteen bands, each a `uint8_t` 0 to 255 on the bands' scale: the power of the FFT bins whose centers fall in 20 to 150 Hz, 150 Hz to 2 kHz, and 2 kHz to Nyquist, normalized as step 1 normalizes a band, so a sum reads on the same dB scale as a band, so an effect keyed on the bass finds the kick whichever band it lands in; MoonLive gains `audioBass`, `audioMid` and `audioTreble`.
- Bench: `BeatRipplesEffect` keyed on the bass sum through a kick-drum track.

### 4. A kick and a snare look different ⏳

- An onset per range (low, mid, high) from the sums of step 3, each with the flux-against-its-mean decision `OnsetDetector` already makes; `onset` stays as the any-hit signal. Moved here from [the live-performance plan, step 5](Plan-20261013%20-%20Audio%20reactive%20live%20performance%20(0%20of%207).md), which keeps the drum-trigger route.
- Desktop test: a synthesized kick fires the low onset and not the high one; a synthesized hi-hat the reverse.
- Bench: a drum track, the kick splashing from the floor and the hi-hat sparkling at the top, on one layer each.

### 5. The beat lands on the beat ⏳

- Tempo and beat phase from the onset curve, the standard algorithm (Ellis, *Beat tracking by dynamic programming*, 2007: autocorrelation of the flux history over the last few seconds picks the tempo in 60 to 200 BPM, and the phase that lines up with the recent onsets picks the beat). The frame carries `bpm`, `beatPhase` in 0 to 1 and `beat` for the block the phase wraps. MoonLive's `audioBeat`, a level-above-its-average guess until now, returns the tracked beat, and `audioOnset` and `audioBpm` are added. A reactive effect trails a hit by a block; a beat-synchronous one fires on it, and knows when the next one comes.
- The beat phase is what the live-performance plan's `MidiFrame` takes from a MIDI clock when one runs; the audio estimate fills the same field when none does, so an effect reads one phase from either source.
- Desktop tests: a synthesized click track at 120 BPM reads 120 within 1 BPM and `beat` fires within a block of each click; a track that steps to 140 BPM follows within four beats; silence reads no tempo.
- Bench: `BeatRipplesEffect` on `beat` instead of `onset` through a steady song, the ripples on the pulse rather than on every hit.

## What an effect reads

[`AudioFrame`](../../src/core/util/AudioFrame.h) is the whole contract: one flat struct per tick, every field a small integer, read through `AudioService::latestFrame()` by an effect and through builtins by a MoonLive script. Today it carries the level (raw and smoothed), the sixteen bands (raw and with meter ballistics), the dominant frequency with its magnitude, the flux and `onset`. So an effect sees volume, a spectrum and "a hit happened"; twelve of the seventeen audio effects draw the bands, the rest read the level or `onset`. The plan widens the struct and nothing else:

| Step | New fields | MoonLive |
|---|---|---|
| 1, 2 | none; `bands` and `level` mean what they did, read better | |
| 3 | `bass`, `mid`, `treble` | `audioBass`, `audioMid`, `audioTreble` |
| 4 | `onsetLow`, `onsetMid`, `onsetHigh` | `audioOnset(range)` |
| 5 | `bpm`, `beatPhase`, `beat` | `audioBpm`, `audioBeat` |

A field is 0 when its source is absent, so an effect on a board without a microphone draws its idle state, as today. The new fields are local: the WLED audio-sync packet carries none of them, so a board in receive mode leaves them at 0 while it keeps its own `bandsSmoothed` across the fresh frame, and a desktop test pins both.

## What it costs, per chip

**Analysis is not where the frame goes.** AudioService's fixed I2S block read adds 13 to 22 µs to an average tick ([performance.md](../reference/performance.md), 2026-06-17); the 512-point FFT every 23 ms at 22,050 Hz waits for a measurement of its own. Each step keeps it that way, and its desktop test and bench run record the tick it adds.

| Step | What it adds per block | Estimate, to be measured |
|---|---|---|
| 1 | a power sum over the 256 bins, replacing the loudest-bin search | the same loop as today |
| 2 | a few multiplies for the floor, gate and offset | negligible |
| 3 | three sums over bins step 1 already reads | negligible |
| 4 | three flux decisions | negligible |
| 5 | an autocorrelation of about 6 s of flux, some 260 values, over the 30-odd lags of 60 to 200 BPM, plus a phase fit | about 8,000 multiply-adds, under 0.1 ms on a classic ESP32 at 240 MHz, and 1 KB of history |

**Beat tracking is cheap here because it runs on the onset curve, not on the audio.** The expensive half of a beat tracker is the onset analysis, which the FFT already pays for. A tracker gets costly when it evaluates every lag at the audio rate, or runs a dynamic program over a long history every sample; this one evaluates once per block over a few hundred values.

**The same signals on every chip, the headroom spent on resolution.** Every chip that analyzes audio carries all five steps, so an effect behaves alike on each, as decided for the filter-bank trial. A chip with spare cycles spends them on how finely the signals are measured, never on extra ones:

- **A 1024-point FFT for the bass.** At 22,050 Hz and 512 points a bin is 43 Hz wide, so 20 to 150 Hz is three bins; 1024 points halve the bin width, at twice the block latency. The decimated FFT in [the backlog](#backlog) is the other route to the same bass.
- **50% overlap.** A new block every 256 samples halves the onset step from 23 to 12 ms, which tightens the beat phase.
- **44,100 Hz** for the treble sum and the hi-hat onset.

Each is chosen per chip by the platform, a default rather than a user setting, so it adds no knob. The classic ESP32 is the floor: each step is measured there first, and a step over its budget there, about 1% of a 60 fps tick, gets cheaper on that chip rather than dropped from it.

## Effects on the new signals

Each is a small change once its field exists, and together they are the bench for the plan, since a signal nobody draws is not verified. In the order the steps land:

| Effect | Reads | What it shows | Step |
|---|---|---|---|
| **An audio modifier** | `levelSmoothed` | Scales any effect's brightness, speed or size with the music, so every effect in the catalog answers it without an audio variant each. The one that makes the level path visible on a whole rig | 2 |
| **`BeatRipplesEffect` on the bass** | `bass`, then `onsetLow` | Stones dropped on the kick rather than on any hit, the first effect to find the kick in any song | 3, 4 |
| **Three zones** | `bass`, `mid`, `treble` | Three colors, the bass on the floor, the mids in between, the treble at the top, breathing with their range: the spectrum reduced to what an audience reads at a glance | 3 |
| **Drum kit** | `onsetLow`, `onsetMid`, `onsetHigh` | A kick splashes from the floor, a snare flashes the middle, a hi-hat sparkles the top, each a short-lived sprite the `SpriteFountainEffect` already knows how to throw | 4 |
| **`PulseEffect` and `PaintBrushEffect` on the beat** | `beat` | One shell, one stroke, per beat instead of per hit, steady through a song | 5 |
| **Beat sweep** | `beatPhase` | A bar that crosses the rig once per beat, position from phase, the classic sweep in time that a reactive effect cannot do because it moves between hits | 5 |
| **Palette per bar** | `beat` | The palette advances every fourth beat, so color changes land on the bar line | 5 |

The modifier is the recommended first, since it turns the existing catalog audio-reactive at once; the rest follow their steps.

## Backlog

On the topic and not a step. What remains here when the plan ships returns to the Project as cards, item by item, then.

- **Analyzer-grade accuracy**, what a rig cannot show, each taken when a user or an effect asks: a transform sized by the chip (2048 points on the S3, 4096 on the P4) for two bass notes to read as two bars; a biquad pre-filter chain (rumble high-pass, anti-alias low-pass) from esp-dsp's `dsps_biquad_f32`; calibration against a measured chain and a self-test on the card; the 31 ISO 266 third-octave bands for an analyzer effect. Troy's rewrite has all four, checked against a hardware analyzer.
- **A second, decimated FFT for the low bands (2026-10-05).** At the default 22.05 kHz a 512-sample block gives 43 Hz bins every 23 ms, so the bands below about 130 Hz own one bin each and two bass tones a semitone apart land in the same band. A longer FFT alone buys resolution at the cost of time for every band (2048 samples: 10.8 Hz, but 93 ms). The textbook multirate answer keeps both: low-pass and decimate by 4 to 5.5 kHz, run a 512-point FFT on that for the bands under about 2.7 kHz (about 11 Hz bins), and keep the 23 ms FFT for the rest. Cost is one more 512-point FFT and a small decimation filter per block. Pinned on the desktop by two synthesized bass tones (50 and 60 Hz) landing in different bands, then measured on an S3 against the Audio module's tick time.
- **A filter bank for the bands (2026-10-28), a trial.** Troy's unpushed version computes each band as a resonator answering every sample, so a band's time resolution matches its frequency and a hit is timed within a render tick rather than within the 23 ms FFT block: sharper onsets, a better beat phase, a bass that follows a note. Sixteen second-order biquads at 22 kHz are a few MFLOPS, inside an S3's FPU and trivial on a P4, Teensy or desktop; the classic and the C3 are the question. Every device behaves the same, so this is a trial of one against the other, not a choice per chip: both behind the one `AudioFrame` with the same unit contract and the same tests, a select on the card for the trial only, run after the steps give it a bench (the synthesized tests on the desktop, onset timing on a click track, the tick cost measured on the slowest chip, and the bench by eye). Then one of the two stays, on every chip, and the other and the select go; `peakHz` keeps a transform either way.
- **More audio effects on the bands**, 2D and palette-driven frequency-reactive ones, beside [the effects on the new signals](#effects-on-the-new-signals).
- **Pin auto-scan**: detect the microphone's `sdPin` with `wsPin` and `sckPin` fixed, a noise prompt and a confirm; explicit pin controls ship today.
- **Sources beyond the I²S microphone**, each a variant behind the same platform read, the analysis untouched. The I²S peripheral already drives MCLK (`mclkPin`) for a line-in ADC.
    - **PDM microphones**: the IDF `i2s_pdm` driver, a sub-mode of the same peripheral.
    - **Analog line-in**: DedeHai has it on the S3 and Troy in ParrotRadio, where he recorded and played back through it. An analog path is validated by listening to it, since one that only feeds a level meter "may not be as accurate as it looks" (Troy).
    - **I²C-configured codecs** (the ES8311 class): Espressif's `esp_codec_dev` component carries the register tables for many codecs, so none is hand-rolled.
    - **Multi-channel front-ends**: a TDM codec (the ES7210 on the Waveshare RGB-Matrix board) or a DSP board giving one analyzed channel per instrument, the input [the live-performance plan](Plan-20261013%20-%20Audio%20reactive%20live%20performance%20(0%20of%207).md) needs for acoustic drums.
- **Desktop audio capturing what the machine plays (2026-09-29).** `platform_desktop_audio.cpp` enumerates capture devices only (`ma_context_get_devices` with null playback lists) and opens `ma_device_type_capture`, so on a Windows bench the picker offers `default` and the microphone and the speakers are absent, with no Stereo Mix to fall back on. miniaudio has `ma_device_type_loopback`, WASAPI-only, taking a playback device: enumerate playback devices too, mark them in the picker so a microphone and "what is playing" tell apart, and open loopback when one is chosen. macOS has no equivalent and needs a virtual device such as BlackHole, so the picker offers what the platform can do rather than a row that fails. A disconnected microphone jack reports a plausible level (RMS 20 to 30 at mains hum), which the status of step 2 tells from a working input.

## Open questions

- **Which settings stay on the card:** step 2 settles it per setting on the bench. **Recommended (2026-10-28):** the result decides, not the count; the expectation is `floor` alone, and a response-speed setting (attack and release) is the likeliest second if a bench case wins with it.
- **Whether step 2 keeps a manual mode** for a show that wants deterministic levels. **Recommended (2026-10-28):** no; freezing the floor and the offset is the deterministic mode, as a `freeze` toggle, if a show asks.

## What it removes

In code: the loudest-bin estimator and the bin-ownership loop in `AudioBands.h` with their technical notes, and, as step 2 confirms, `levels`, `gain`, the conditioner's ratio and maximum gain and the two learned tables. In the docs: the audio DSP roadmap is gone, its prior art and adaptive gate in this plan and its source list in the backlog above; the audio-reactive follow-ups section left the light backlog and the desktop-capture and noise-floor items left the core backlog, into steps 2 and 3 or the backlog above; per-band onsets left the live-performance plan for step 4.
