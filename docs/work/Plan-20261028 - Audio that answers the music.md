# Plan: audio that answers the music

Approved 2026-10-30 and revised 2026-11-03 and 2026-11-06, built in small steps judged on the bench, so the steps below follow what the music shows. The goal is the best visible answer to the music: silence is dark, a quiet passage stays quiet, a loud one is loud, the bars sit balanced across the spectrum, every band answers its own range and not its neighbor's, the bass separates note from note, a kick drum is found whatever song is playing, the beat lands on the beat, and the lights follow the harmony. The least to set follows from it, since a level that learns the song beats any knob set for one song. The analysis stays one pipeline on every board; calibration against a measured chain is not the goal and goes to the backlog.

The yardstick is twofold. For the bands it is the real-time analyzer, whose third-octave filters a standard defines (IEC 61260-1). For what is read from them it is music information retrieval, the field whose libraries (Essentia, librosa, aubio) describe music to a machine: loudness, harmony, spectral balance, onsets per band, and tempo with beat phase, in that order. Step 1 builds the bands, steps 2 to 6 one descriptor each, each step reading what the ones before it give. Two things the field settles that this plan follows: a band per instrument is not a thing (instruments overlap in frequency, and separating them is a neural net on a GPU), while the drum kit does separate by range; and genre presets lose to adaptation (a floor, a gate and a gain that learn the dynamics give a quiet classical passage and a loud rock chorus the same treatment with no switch).

**Every step lands twice.** `AudioSpectrumEffect` shows the core audio as it is, the developer's view of what the step changed, and one showcase effect shows it the way an end user picks it for a show. `BeatRipplesEffect` is the model: stones dropped on the music, readable at a glance by an audience. A signal nobody draws is not verified, so the pair is part of the step, not a follow-up.

## The design basis

- **One pipeline, no engine select** ([CLAUDE.md, principle 3](../../CLAUDE.md#principles)): one analysis path, the same on every chip: the filter bank, with every output derived from its bands.
- **The textbook.** A third-octave band is the power through its band-pass filter, as IEC 61260-1 defines it and every digital analyzer implements it, measured by a multirate filter bank. A noise gate opens fast and closes slow with hysteresis, as every studio gate does. Automatic gain is one offset that holds the loudest band at the top, as a compressor's make-up gain does.
- **Study, credit, write fresh.** MoonLight's audio path is its own implementation, from the standards and standard DSP; the people whose thinking it studies are credited by name below and in the headers' `@attribution`.
- **Measured on synthesized signals** on the desktop, then judged by eye on the bench: the product owner's eyes are the measurement for a light show.

## What exists

The engine of steps 1 to 5, each header its home: [AudioService.h](../../src/core/services/AudioService.h) (the audio task, memory, the cost per chip, audio sync), [FractionalOctaveBank.h](../../src/core/services/FractionalOctaveBank.h) (the multirate bank), [AudioBands.h](../../src/core/services/AudioBands.h) (the sixteen, the dominant frequency, the two banks), [AudioDynamics.h](../../src/core/services/AudioDynamics.h) (level, floor, gate, top), [AudioHarmony.h](../../src/core/services/AudioHarmony.h) (pitch classes, harmonic change, key, tuning), [AudioOnsets.h](../../src/core/services/AudioOnsets.h) (range, band and note onsets, the kick), and the contract an effect reads, [AudioFrame.h](../../src/core/util/AudioFrame.h).

## Prior art

Audio-reactive lighting has a lineage (WLED-SR, WLED-MM, MoonLight). Frank (softhack007)'s adaptive noise gate, Troy's filter banks and LedFx's asymmetric smoothing are built into steps 1 and 2 and credited in their headers. What the open steps still draw on follows.

### Troy (troyhacks)

MoonModules: [his AudioReactive rewrite](https://github.com/troyhacks/WLED), branch `P4_Experimental_Dev`, running on the bench's Olimex ESP32-P4-PC at 192.168.1.223 and checked against a hardware analyzer: 31 ISO 266 bands from two Butterworth filter banks and no FFT, the design step 1's bank follows.

- **A tempo tracker** (`ar_tempo.h`, commit `9b416ad3` of 2026-11-06): onsets from the 90 to 320 Hz energy of the 3 kHz stream, a phase-locked comb over the last 12 s that scores each candidate period from 90 to 175 BPM by how tightly the onsets cluster in phase, a PLL grid and a weak prior at 125 BPM; median hit rate 0.817 on a hundred house tracks. Prior art for step 6.

### Anjin's measurements, and the trackers they test against

Anjin, on the MoonModules Discord (2026-11-03), ran a faithful port of Troy's tracker and his own filter-bank front end through labelled sets, against Essentia, the QM beat tracker, Beat This!, madmom's TempoDetector and TCN:

- **Troy's tracker:** 97% within 2% of the label on house, level with the best offline methods; about 70% on mixed EDM and 47% on short loops of eight genres. The 125 BPM prior holds it near 125, and the comb's chance score rises with tempo, so noise locks at about 130, as .223 showed: 127 to 128 BPM from the microphone's noise floor before any music arrived. On a microcontroller nothing he tested is as small or as fast.
- **His filter-bank front end:** 8 bands with edges at 30, 100, 200, 400, 800, 1,600, 3,200, 6,400 and 16,000 Hz, one RBJ band-pass section each at the full rate, the rise in each band's level summed into a flux read 200 times a second, no FFT on the beat path: 88.4% on Beatport and 75.3% on the loops, with a fair periodicity stage. A sharper bank, he expects, gives cleaner input still, since its low bands separate the kick from the bass.
- **Agreement as confidence:** where independent trackers agreed, they were right 99 to 100% of the time, so two trackers that agree are a lock that costs almost nothing.
- **Filter delay:** a band-pass delays its band by about the inverse of its bandwidth, tens of milliseconds below 100 Hz against under one for the hats, so onsets compared across bands, a tom sweep and the downbeat need each band's delay measured with a click and subtracted.
- **What to test on:** known tempos across 40 to 160 BPM, and drum and bass in particular, at 170 to 175 BPM with a half-time feel near 87, where a 125 prior invites the octave error. His harness and data are reusable, offered for MoonLight's desktop build.
- **More from the thread (2026-11-03):** the comb's score for random onsets is 0.39 at 90 BPM and 0.58 at 175, since its ±70 ms window covers more of a short beat, and noise locks near 130 at about 0.45, so a raw score is no confidence until the comb is chance-corrected; 12 s of picked kicks is thin evidence, and a longer memory or the comb run over the continuous onset curve does better; Troy's tracker tends to fail at double the tempo and Anjin's at half, opposite directions that make them a strong pair. A reference tracker, **BpmDj** (an offline tool; not Troy's numbers), reads 89.8% within 2% on 774 Beatport previews, but drum and bass only 28%, with 55% double or half, mostly label errors (Beatport lists 116 to 119 where trackers answer 174 to 178); on 296 held-out Freesound loops Anjin's fold reads 75.3%, BpmDj 65.5%, QM 63.2%, madmom's comb 61.5%, Beat This! 57.4% and Troy's 47.0%. Anjin's Beatport figure reads 88.4% in one message and 90.6% in his table, possibly two variants, to be asked before it is cited further. The material: the [Freesound Loop Dataset](https://zenodo.org/records/3967852), with a 12 s warm-up per loop, and the [Beatport EDM Key Dataset](https://zenodo.org/records/1101082) for key.

The textbook behind it: **Scheirer** (*Tempo and beat analysis of acoustic musical signals*, JASA 1998) is the filter-bank tracker this all descends from: a few bands, each envelope differentiated and half-wave rectified, summed, and a bank of comb resonators for the tempo. **Goto**'s real-time beat tracker found the bar boundaries from chord changes, the downbeat of step 6. Tempo is scored as MIREX scores it: accuracy 1 within 4% of the label, accuracy 2 also forgiving the double, half and triple.

### Twelve to the octave: notes and harmony

Notes to color on a microcontroller has precedent in **ColorChord** (Charles Lohr, [on an ESP32 with WS2812B LEDs](https://spectrum.ieee.org/amp/how-to-turn-music-into-colors-with-a-wifi-bridge-2650274517)), chords from a chromagram shown only above a confidence score in **[the Chord Colourizer](https://arxiv.org/html/2510.10173v1)** (2025), lighting cues on a chord sequence in **[a patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/5859382)**, and one note within the octave in WLED's **Rocktaves**. The textbook bank, Müller and Ewert's Chroma Toolbox, is credited in [AudioHarmony.h](../../src/core/services/AudioHarmony.h).

What a full mix allows, by the field's results, which steps 11 and 12 measure against:

| Signal | Realistic | Why |
|---|---|---|
| The moment the harmony changes | yes | needs only that the twelve pitch classes shifted, not which chord; the standard harmonic-change detector is robust and cheap |
| A color from the harmony | yes | the same chord always gives the same color, and a misread gives a near color rather than a visible error |
| Major or minor, the key | mostly | needs 10 s or more of music: a slow mood color |
| The chord's name | sometimes | templates over the pitch classes get roughly 60 to 70% right on pop, neural nets roughly 80 to 85%: too often wrong to show a name |
| The bass line's note | in sparse mixes | the kick shares the range, and low notes need 0.3 to 0.6 s to separate |
| The melody, and its octave | no | every note also lights its overtones, and the octave is the classic failure of pitch detection |

A solo instrument through line-in does much better than a full mix, and a MIDI instrument gives the notes exactly and at once.

## Steps

Each step ends in a desktop test, its two effects, and a bench look. ✅ done, 🧪 built and waiting for the product owner's test, ⏳ to build. The bench's songs, for the product owner and for end users, are one playlist: [MoonLight audio test songs](https://open.spotify.com/playlist/0iL47MM8XcUpJzzdKdHQD3).

For the StringsEffect, one solo per instrument, all on Spotify: guitar: Fernando Sor, "Etude No. 22 in B Minor" (Paul Erickson); bass: Miroslav Vitouš, "Variations On Spanish Themes"; violin: Hilary Hahn, Bach "Sonata No. 1 in G minor for solo violin, BWV 1001: I. Adagio" (violin alone); cello: Rafael Krux, "Cello Solo".

### 1. The bands by the book ✅

[FractionalOctaveBank](../../src/core/services/FractionalOctaveBank.h) and [AudioBands](../../src/core/services/AudioBands.h): third octaves by IEC 61260.

### 2. One level path, no knobs ✅

[AudioDynamics](../../src/core/services/AudioDynamics.h): level, floor, gate, top.

### 3. The lights follow the harmony ✅

[AudioHarmony](../../src/core/services/AudioHarmony.h) and the harmony palette.

### 4. The kick is found in any song ✅

[AudioFrame](../../src/core/util/AudioFrame.h): the ranges, `onsetLow`, the bass note.

### 5. A kick, a snare and each tom look different ✅

[AudioOnsets](../../src/core/services/AudioOnsets.h) and [DrumKitEffect](../../src/light/effects/DrumKitEffect.h).

### 6. The beat lands on the beat ⏳

- **First, the beat reaches the lights (product owner, 2026-11-23), whatever decides it.** Before any tracker runs here, the consumer half: a beat clock in the core runs the receiver rules of [the beat section](../reference/audio-sync.md#beat), the phase advancing with the tempo between packets and each packet pulling it back, a change of the beat count a beat taken once, and the beat ending four beats after the last packet. Effects and MoonLive read it: `audioBeat()` fires on the beat and falls back to the onset while no beat runs, and `audioBpm()`, `audioBeatPhase()` and `audioBeatInBar()` are added. `BeatRipplesEffect` gets a `beat` trigger, and **`BeatSweepEffect`** is the showcase, a bar crossing the layout once a beat or a bar over the bands, as troyhacks' Beat Sweep does in his WLED, free-running at its own speed while no beat runs. The simulated song states its beat, 100 BPM, so a desktop shows it with no sender. It works today with a beat over MoMo from troyhacks' WLED (its decks, Ableton Link or its tracker); the trackers below fill the same field as source 0, and every effect follows them on the day they land. Tests: synthesized 120 BPM packets give the phase between packets, one beat a count, missed beats counted and the beat ending in silence. **Built (2026-11-23)**, with a beat readout on the Audio card and AudioSpectrum's beat column, and seen on the desktop following troyhacks' tracker on .224's line-in.
- Tempo and beat phase from the onset curve. Two trackers, run side by side: the textbook (Ellis, *Beat tracking by dynamic programming*, 2007: autocorrelation of the onset curve over 12 s or more picks the tempo in 60 to 200 BPM, and the phase that lines up with the recent onsets picks the beat), and Troy's phase-locked comb, measured on real house music, its comb run over the continuous onset curve rather than picked kicks, corrected for the score a period earns by chance and given a lock confidence, so noise no longer locks near 130. The two fail in opposite directions, Troy's at double the tempo and the textbook's to be measured, which is what makes their agreement a lock. Where the two agree the tempo is locked, the confidence an effect reads; where they disagree the frame says so. The frame carries `bpm`, `beatPhase` in 0 to 1 and `beat` for the block the phase wraps. MoonLive's `audioBeat`, a level-above-its-average guess until now, returns the tracked beat, and `audioOnset` and `audioBpm` are added.
- **Harmonic changes find the downbeat.** A beat tracker hears beats but not which is the bar's first; Troy's says so of its own. Chords mostly change on the bar line, so step 3's harmonic changes, lined up with the beat grid, place the one, and the frame carries `downbeat` and the beat in the bar. A board without harmony counts the bar from the lock, as Troy's does.
- The beat phase is what the live-performance plan's `MidiFrame` takes from a MIDI clock when one runs; the audio estimate fills the same field when none does, so an effect reads one phase from either source.
- **A Pioneer deck first, and the beat to every board (2026-11-07).** Pro DJ Link's master deck is the first beat source, ahead of the MIDI clock, tap and the tracker, since its track was analyzed beforehand and it knows the bar. The sending board puts the beat it decided in the sync packet's beat section, so every board lands on the same beat: [the beat section](../reference/audio-sync.md#beat), in the MoonModules format since 2026-11-22. troyhacks's WLED-MM reads the decks in `usermod_v3_pioneer_prolink.h` and keeps the beat on the board.
- **Tap tempo, the escape route (robwomp, 2026-11-04).** A third beat source beside the MIDI clock and the tracker, as lighting consoles, DJ mixers and pedals have it: the tempo is the mean of the last four to eight intervals between taps, a gap over about 2 s starts a new count, and the last tap sets the phase. The order is MIDI clock, then tap, then the tracker; a tap also re-phases a tracker whose tempo is right and whose beat is not. A button on the card, a MIDI pad or a GPIO through input mapping; four taps average the WiFi's 5 to 20 ms jitter well inside ±20 ms.
- **Beat-synchronous analysis once the beat locks (Anjin, 2026-11-04).** Each feature averaged over a beat rather than a block (Bello and Pickens 2005; Ellis 2007), time-synchronous averaging in vibration analysis: step 3's pitch classes per beat, their change checked on the beat boundaries where chords change, so fewer false changes and each on the beat; step 5's band onsets folded at the beat period, so hats and rides gather at their places in the sixteenth grid and random noise spreads. It sharpens timing, not the spectrum, and needs a tempo within about 1%, a 2% error smearing the fold within a few bars; Troy's bass-led tracker guides it. The bank suits it, every band updating every block, so a beat boundary falls anywhere.
- **`AudioSpectrumEffect`** shows the tempo and a marker on each beat.
- **Showcase: `BeatRipplesEffect`** on `beat` instead of `onset`, the ripples on the pulse rather than on every hit, a **beat sweep**, a bar crossing the rig once per beat, its position from the phase, and a **palette per bar** turning on the downbeat.
- **DrumKit's kick on the beat (product owner, 2026-11-09).** In a full mix DrumKit's kick follows `onsetLow`, of whose fires 38% are kicks on MDB Drums' mixes; on Billie Jean the synth bass's eighths fire it 41 times in 20 s against about 20 kicks, with gaps of three beats. Once the tracker locks, a low onset counts as a kick only near a beat position where kicks recurred in the last bars, the beat-synchronous fold above, so the kick keeps the song's pattern; unlocked, DrumKit falls back to the onsets. FastLED's `MusicalBeat` detector rejects onsets that do not fit the tempo the same way.
- **Desktop tests:** a synthesized click track at 120 BPM reads 120 within 1 BPM and `beat` fires within a block of each click; a track that steps to 140 BPM follows within four beats; silence reads no tempo; a synthesized progression changing chord every four beats puts `downbeat` on the change.
- **Corpus tests:** Anjin's labelled sets and the tempo harness through the desktop build, the Freesound loops with a 12 s warm-up each, known tempos across 40 to 190 BPM, scored as Gouyon et al. (2006) define it (accuracy 1 within 4%, accuracy 2 forgiving the double, half and triple) and reported per genre, with 3:2 as its own class rather than "wrong"; beside Troy's tracker's 97% on house, about 70% on mixed EDM and 47% on loops, and BpmDj's table.
- **Chance and silence tests:** random onsets at 90 and 175 BPM lock at neither, after the chance correction; beatless music (ambient, implied beats) reads "no lock" rather than a tempo.
- **Bench material:** drum and bass, hard techno with a 909 kick whose fast click must not read as a hat, Jeff Mills' unquantized swing, breakcore and sliced amen breaks, and a jam band for the non-dance end.

### 7. Every effect reads the new engine ✅

[AudioFrame](../../src/core/util/AudioFrame.h) `heard()` and `levelSmoothed`; [MIGRATING](../reference/MIGRATING.md).

### 8. Audio sync carries the new engine ✅

[Audio sync](../reference/audio-sync.md) and [AudioSync](../../src/core/services/AudioSync.h).

### 9. The notes a player could play ✅

[HarmonicSieve](../../src/core/util/HarmonicSieve.h) and [playerHands](../../src/light/powerfunctions/playerHands.h).

### 10. How a string was played ✅

[StringsEffect](../../src/light/effects/StringsEffect.h) and [playerHands](../../src/light/powerfunctions/playerHands.h).

### 11. The notes are measured ✅

Closed at its plateau (product owner, 2026-11-08): the hands reach 23 to 29% F on GuitarSet. The baseline scores are in [bench_notes](../../moondeck/MoonDeck.md#bench_notes) and [bench_drums](../../moondeck/MoonDeck.md#bench_drums), the rules with their measured basis in [HarmonicSieve](../../src/core/util/HarmonicSieve.h), [AudioOnsets](../../src/core/services/AudioOnsets.h) and [playerHands](../../src/light/powerfunctions/playerHands.h), the lessons in [lessons](../contributing/lessons.md). The unbuilt items are cards on the Project.

### 12. The chord is named ✅

[AudioChords](../../src/core/services/AudioChords.h).

## What an effect reads

[`AudioFrame`](../../src/core/util/AudioFrame.h) is the whole contract: one flat struct per tick, every field a small integer, read through `AudioService::latestFrame()` by an effect and through builtins by a MoonLive script. The plan widens the struct and nothing else. Step 6 adds `bpm`, `beatPhase`, `beat`, `downbeat` and the beat in the bar, with `audioBpm`, `audioBeat` and `audioDownbeat` in MoonLive, and audio sync carries them in [the beat section](../reference/audio-sync.md#beat).

## What it costs, per chip

The engine's measured share of a core per chip is in [AudioService.h](../../src/core/services/AudioService.h). Step 6 adds an autocorrelation or comb over 12 s or more of the onset curve, a phase fit, and the harmonic changes lined up with the grid: about 16,000 multiply-adds per block, estimated well under a millisecond on a classic ESP32, to be measured on the card's `cpu`.

**The cost of a new analysis feature (product owner, 2026-11-25)**, the tempo tracker first: as lean and fast as it can be made, as every hot path is (principle 1), its time per 21 ms block on a classic ESP32 and its internal RAM measured and reported with the feature, so each later version compares against the last. A fixed cap stood here until 2026-11-25 (0.5 ms and 4 KB) and was dropped: the design that works measured above it, and a number no working design meets leaves a PR no finish line. It runs on the audio task, never the render path, allocates nothing per block, and uses float, not double. It is built separable: its state is its own allocation, held only while it runs, entered through one call per block, so a control can switch it off today and the subscription below can tomorrow. A chip that cannot afford a feature at all leaves it out at compile time, by a `platform_config` flag.

## The showcases

Steps 1 to 5 and 7 landed with `WaterfallEffect`, the harmony palette, `BeatRipplesEffect` on the kick, `DrumKitEffect` and `PaintBrushEffect`; step 6's are in its section.

More effects on the same signals, each a small change once its field exists: **a harmony color**, the rig tinted by the twelve pitch classes, from step 3; **three zones** (bass on the floor, mids between, treble at the top, breathing with their range) from step 4; **`PulseEffect` and `PaintBrushEffect` on the beat**, one shell or stroke per beat, from step 6.

Effects on the measured notes, each gaining from every change step 11 scores, since they read the same `keep` and the same strikes as Keyboard and Strings (2026-11-08): `SheetMusicEffect`, built in step 11, writing them on a staff or a piano roll; **a note fountain**, each struck note launching a spark from its pitch's place along a strip, colored by its pitch class as ColorChord does; **a walking bass**, a light stepping along the floor to each bass note's pitch on its strike; **a circle of fifths**, twelve segments in a ring with the sounding pitch classes lit and step 12's chord drawn as its shape.

## After action review (2026-11-09)

Kept here until the plan is deleted; its lessons are in [lessons.md](../contributing/lessons.md#lessons-from-the-audio-plans-measured-steps-notes-and-drums-november-2026) and its stop rule in [the plan process](../moonwalk.md#pick-spec-plan).

- **Planned:** one descriptor a step, each landing in `AudioSpectrumEffect` and a showcase, judged on the bench before the next.
- **Happened:** steps 1 to 5 in a week from the textbook; step 6 to Anjin's team with the tempo harness; steps 7 to 10 and 12 built and waiting for the product owner's look; step 11 added on 2026-11-07, grown to eleven items and closed at its plateau on 2026-11-08; beside them SheetMusic, Soap, the sectioned sync format and Demand.
- **Went well:** every change scored before and after, reverts decided on numbers; every textbook choice kept and beating its alternative; one harness for tempo, notes and drums, and the notes cleaned once in the core; reviews catching three real defects before they shipped; the `harmony`, `levels` and `gain` controls and SheetMusic's sliders removed.
- **Went less well:** building outran the product owner's look, five steps waiting, against the plan's own rhythm and CLAUDE.md's "invite the product owner to test, then stop"; step 11's 50% target set without the field's ceiling, so it closed on shrinking gains rather than a planned rule; a synthetic kick without a click misled the drum tuning; tries without a written prediction; the analysis's cost on a P4, about 14% of a core in AudioAnalysis.h against Friday's 8.8% measured, never re-measured; two Python harness breaks from core changes found only at pre-commit; scope drifting inside sessions.
- **Learned:** a measured step's stop rule set from the literature first; synthetic signals calibrated on real recordings; the prediction written before the run; `test_tooling --python` right after a core interface change; a cost claim measured on hardware when its code changes, in one home; the next step waits for the product owner's eyes on the last.

## Backlog

On the topic and not a step. What remains here when the plan ships returns to the Project as cards, item by item, then.

- **One clean frame on request (Troy, 2026-11-09).** A route returning the layout's current frame as a PNG, so an agent or a test sees what the lights show without a browser screenshot, which empties the desktop it targets, or a video pipeline; Troy's agent reads the low-resolution `/wsp` preview for it and asked for a screenshot function. Judged by an audio effect's frame checked against the music in a test
- **A sync packet for every band count.** WLED receivers accept only the 44-byte `00002` packet and drop anything over 96 bytes, so the sixteen stay in it and the bands travel in a second packet type under its own header, which WLED ignores. It describes itself: bands to the octave, the reference frequency, the first band and the count, then the values (about 45 bytes for 31 bands, 135 for 121). A receiver sums finer bands into the coarser ones its effects read; a coarser packet cannot give finer bands, so an effect that needs them idles. MoonLight decides the format (2026-11-03).
- **Troy's display settings, for a like-for-like comparison.** The pattern that shifts the sixteen −3 to +3 bands, the dB window span, auto or fixed range, the dB reference, attack and release. A run pins each on both boards.
- **Calibration**, what a rig cannot show, taken when a user or an effect asks: a biquad pre-filter chain (rumble high-pass, anti-alias low-pass) from esp-dsp's `dsps_biquad_f32`; calibration against a measured chain and a self-test on the card. Troy's rewrite has both, checked against a hardware analyzer.
- **More audio effects on the bands**, 2D and palette-driven frequency-reactive ones, beside [the showcases](#the-showcases).
- **Pin auto-scan**: detect the microphone's `sdPin` with `wsPin` and `sckPin` fixed, a noise prompt and a confirm; explicit pin controls ship today.
- **Sources beyond the I²S microphone**, each a variant behind the same platform read, the analysis untouched. The I²S peripheral already drives MCLK (`mclkPin`) for a line-in ADC, and `codecGain` sets an ES8311 or ES7210 between microphone and line level.
    - **PDM microphones**: the IDF `i2s_pdm` driver, a sub-mode of the same peripheral.
    - **Analog line-in**: DedeHai has it on the S3 and Troy in ParrotRadio, where he recorded and played back through it. An analog path is validated by listening to it, since one that only feeds a level meter "may not be as accurate as it looks" (Troy).
    - **Multi-channel front-ends**: a TDM codec (the ES7210 on the Waveshare RGB-Matrix board) or a DSP board giving one analyzed channel per instrument, the input the live-performance plan needs for acoustic drums.
- **A fast roll as separate hits (2026-11-05).** A band's onset keeps strokes apart up to about 10 a second; at 20 a second it fires 2 of 10, since the band falls only about 14 dB between strokes through the bank's 10 ms detector and the flux-against-its-mean decision raises its own threshold. A rise measured from the band's recent low point, rather than against its mean flux, would keep a snare roll's strokes; a desktop test pins 20 a second.
- **An instrument tuner (2026-11-04).** The semitone bands say which note sounds, not how far off it is, and a tuner needs 1 to 2 cents, a hundredth of a semitone. The bank picks the note; a period detector on the waveform itself, YIN (de Cheveigné and Kawahara 2002) or McLeod's normalized square difference (2005), the textbook pitch detectors, measures it to a tenth of a cent, for one instrument through line-in, never a full mix. The note's power against its two neighbors, as `peakHz` reads the third octaves, gives a few cents at best: enough for flat or sharp. Shown on AudioSpectrum's `noteNames` line as the note and its cents, and readable by an effect, a strip lit green when in tune. Not step 3's tuning estimate, which measures how far a recording sits from A440 over many notes.
- **Desktop audio capturing what the machine plays (2026-09-29).** `platform_host_audio.cpp` enumerates capture devices only (`ma_context_get_devices` with null playback lists) and opens `ma_device_type_capture`, so on a Windows bench the picker offers `default` and the microphone and the speakers are absent, with no Stereo Mix to fall back on. miniaudio has `ma_device_type_loopback`, WASAPI-only, taking a playback device: enumerate playback devices too, mark them in the picker so a microphone and "what is playing" tell apart, and open loopback when one is chosen. macOS has no equivalent and needs a virtual device such as BlackHole, so the picker offers what the platform can do rather than a row that fails. A disconnected microphone jack reports a plausible level (RMS 20 to 30 at mains hum), which the status of step 2 tells from a working input.
- **The input's rate, and 48 kHz on the P4 (2026-11-04).** The measured rate wanders, 20,700 to 22,300 Hz on the classic and 23,500 to 24,700 on the S3; at 48 kHz the P4 loses a fifth of its frames for 3% more of a core, the audio task waking the render core twice as often being a hypothesis.
- **The onsets' own cost (2026-11-05).** They add 5 to 7 points of a core, more than the per-reading arithmetic predicts; a measurement with the onsets alone tells them from the boards' other load.
- **A `freeze` toggle**, holding the floor and the top, if a show asks for deterministic levels.
- **An audio feature runs only while something reads it (product owner, 2026-11-08)**, publish and subscribe: each effect declares the features it reads, a MoonLive script and audio sync while it sends count too, and `AudioService` runs the union each tick, replacing the `harmony` setting. **Built (2026-11-09)** as the core's generic [Demand](../../src/core/module/Demand.h): a reader names the provider's type and a feature and turns its demand on in its prepare, the module's release turns it off, and the provider counts the tree again on each change, never per frame; the `harmony` control is gone. The note effects, the bar effects showing the keys, AudioSpectrum's pitch classes and note names, effect scripts calling a note builtin and sending the MoonModules format demand the notes. The beat tracker joins it as its next feature. The third octaves feed the sixteen bands, the ranges and every onset, so they run whenever the audio does.
- **A low note confirmed from its overtones (2026-11-08).** Its 2nd and 3rd harmonics sit in bands that report sooner (about 70 and 45 ms against 139 ms at the guitar's low E), the harmonic stacking Basic Pitch reads. Two things come first: the engine dating each note where it says it began, whichever band confirmed it, since the harness dating it back by its own band's delay would score an early note as 70 ms early; and telling a low note from an octave and a fifth struck together, a guitar's power chord, which looks the same and would add a wrong bass note under it.
- **esp-dsp's biquads on the S3 and the P4 (2026-11-08).** The bank is biquads, and Espressif's `dsps_biquad_f32` runs them in assembly, behind the platform layer; with `harmony` on the analysis takes about 14% of a P4 core, 22% of an S3's and 26% of a classic ESP32's ([AudioAnalysis.h](../../src/core/services/AudioAnalysis.h)), so it is worth it when a board runs short, measured on the card's `cpu`.
- **A fixed-point bank** if a C3, which has no FPU, ever analyzes audio, on Damian (DedeHai)'s reasoning from WLED's fixed-point FFT.

## What it removes

Steps 1 and 2 removed the FFT with its window and the platform's `audioFft` seam, the loudest-bin estimator, the band conditioners and the `levels` and `gain` controls; step 7 the broadband flux onset, the level-against-average beat test in three effects and a builtin, and five copies of the silence threshold. In the docs, the audio DSP roadmap, the audio-reactive follow-ups and the desktop-capture and noise-floor items moved into this plan, and per-band onsets left the live-performance plan for step 5.
