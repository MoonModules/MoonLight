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

MoonModules: [his AudioReactive rewrite](https://github.com/troyhacks/WLED), branch `troy/hdmi-on-p4-experimental`, running on the bench's Olimex ESP32-P4-PC at 192.168.1.223 and checked against a hardware analyzer: 31 ISO 266 bands from two Butterworth filter banks and no FFT, the design step 1's bank follows.

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

A multirate third-octave filter bank measuring the 31 ISO bands replaced the FFT, at 24 kHz on every board, read and analyzed on an audio task of its own; the sixteen WLED bars and the dominant frequency derive from it, and `WaterfallEffect` shows it. The design is in [FractionalOctaveBank.h](../../src/core/services/FractionalOctaveBank.h) and [AudioBands.h](../../src/core/services/AudioBands.h), the task and the cost per chip in [AudioService.h](../../src/core/services/AudioService.h).

### 2. One level path, no knobs ✅

Four quantities kept apart, the level, a floor by minimum statistics, a gate and the window's top, replaced the conditioners and the `levels` and `gain` controls, leaving `squelch`: [AudioDynamics.h](../../src/core/services/AudioDynamics.h), with the change in [MIGRATING](../reference/MIGRATING.md). `AudioSpectrumEffect` draws the gate and the dominant frequency.

### 3. The lights follow the harmony ✅

A semitone bank to 2.5 kHz beside the third octaves gives the twelve pitch classes, the harmonic change, the key and the tuning, behind a `harmony` switch: [AudioHarmony.h](../../src/core/services/AudioHarmony.h), the two banks in [AudioBands.h](../../src/core/services/AudioBands.h). The showcase is [harmony.mlp](../../moonlive/palettes/harmony.mlp), a palette turning on each chord change.

### 4. The kick is found in any song ✅

The bass, mids and treble and the bass line's note sit beside the bands, read by MoonLive and drawn by `AudioSpectrumEffect`; `BeatRipplesEffect` gained a `trigger` on the kick. The fields are in [AudioFrame.h](../../src/core/util/AudioFrame.h).

### 5. A kick, a snare and each tom look different ✅

An onset per range, per third-octave band and per note, read every 5 ms with each band's delay subtracted and the kick timed by its click: [AudioOnsets.h](../../src/core/services/AudioOnsets.h). The showcase is [DrumKitEffect](../../src/light/effects/DrumKitEffect.h).

### 6. The beat lands on the beat ⏳

- Tempo and beat phase from the onset curve. Two trackers, run side by side: the textbook (Ellis, *Beat tracking by dynamic programming*, 2007: autocorrelation of the onset curve over 12 s or more picks the tempo in 60 to 200 BPM, and the phase that lines up with the recent onsets picks the beat), and Troy's phase-locked comb, measured on real house music, its comb run over the continuous onset curve rather than picked kicks, corrected for the score a period earns by chance and given a lock confidence, so noise no longer locks near 130. The two fail in opposite directions, Troy's at double the tempo and the textbook's to be measured, which is what makes their agreement a lock. Where the two agree the tempo is locked, the confidence an effect reads; where they disagree the frame says so. The frame carries `bpm`, `beatPhase` in 0 to 1 and `beat` for the block the phase wraps. MoonLive's `audioBeat`, a level-above-its-average guess until now, returns the tracked beat, and `audioOnset` and `audioBpm` are added.
- **Harmonic changes find the downbeat.** A beat tracker hears beats but not which is the bar's first; Troy's says so of its own. Chords mostly change on the bar line, so step 3's harmonic changes, lined up with the beat grid, place the one, and the frame carries `downbeat` and the beat in the bar. A board without harmony counts the bar from the lock, as Troy's does.
- The beat phase is what the live-performance plan's `MidiFrame` takes from a MIDI clock when one runs; the audio estimate fills the same field when none does, so an effect reads one phase from either source.
- **A Pioneer deck first, and the beat to every board (2026-11-07).** Pro DJ Link's master deck is the first beat source, ahead of the MIDI clock, tap and the tracker, since its track was analyzed beforehand and it knows the bar. The sending board puts the beat it decided in the sync packet's beat section, so every board lands on the same beat: [the proposal](../reference/audio-sync.md#a-proposal-the-beat-section), to agree with WLED-MM first. troyhacks's WLED-MM reads the decks in `usermod_v3_pioneer_prolink.h` and keeps the beat on the board.
- **Tap tempo, the escape route (robwomp, 2026-11-04).** A third beat source beside the MIDI clock and the tracker, as lighting consoles, DJ mixers and pedals have it: the tempo is the mean of the last four to eight intervals between taps, a gap over about 2 s starts a new count, and the last tap sets the phase. The order is MIDI clock, then tap, then the tracker; a tap also re-phases a tracker whose tempo is right and whose beat is not. A button on the card, a MIDI pad or a GPIO through input mapping; four taps average the WiFi's 5 to 20 ms jitter well inside ±20 ms.
- **Beat-synchronous analysis once the beat locks (Anjin, 2026-11-04).** Each feature averaged over a beat rather than a block (Bello and Pickens 2005; Ellis 2007), time-synchronous averaging in vibration analysis: step 3's pitch classes per beat, their change checked on the beat boundaries where chords change, so fewer false changes and each on the beat; step 5's band onsets folded at the beat period, so hats and rides gather at their places in the sixteenth grid and random noise spreads. It sharpens timing, not the spectrum, and needs a tempo within about 1%, a 2% error smearing the fold within a few bars; Troy's bass-led tracker guides it. The bank suits it, every band updating every block, so a beat boundary falls anywhere.
- **`AudioSpectrumEffect`** shows the tempo and a marker on each beat.
- **Showcase: `BeatRipplesEffect`** on `beat` instead of `onset`, the ripples on the pulse rather than on every hit, a **beat sweep**, a bar crossing the rig once per beat, its position from the phase, and a **palette per bar** turning on the downbeat.
- **Desktop tests:** a synthesized click track at 120 BPM reads 120 within 1 BPM and `beat` fires within a block of each click; a track that steps to 140 BPM follows within four beats; silence reads no tempo; a synthesized progression changing chord every four beats puts `downbeat` on the change.
- **Corpus tests:** Anjin's labelled sets and [the tempo harness](Plan-20261103%20-%20Tempo%20evaluation%20harness%20(2%20of%205).md) through the desktop build, the Freesound loops with a 12 s warm-up each, known tempos across 40 to 190 BPM, scored as Gouyon et al. (2006) define it (accuracy 1 within 4%, accuracy 2 forgiving the double, half and triple) and reported per genre, with 3:2 as its own class rather than "wrong"; beside Troy's tracker's 97% on house, about 70% on mixed EDM and 47% on loops, and BpmDj's table.
- **Chance and silence tests:** random onsets at 90 and 175 BPM lock at neither, after the chance correction; beatless music (ambient, implied beats) reads "no lock" rather than a tempo.
- **Bench material:** drum and bass, hard techno with a 909 kick whose fast click must not read as a hat, Jeff Mills' unquantized swing, breakcore and sliced amen breaks, and a jam band for the non-dance end.

### 7. Every effect reads the new engine 🧪

Built: one onset, the range hits counted once; one test for sound, `AudioFrame::heard()`; a beat as `onset` until step 6 lands; `level` in its own dynamics window and `levelSmoothed` on the bands' ballistic. Every audio effect reads them, each effect's reading documented in its header, the contract in [AudioFrame.h](../../src/core/util/AudioFrame.h) and the change in [MIGRATING](../reference/MIGRATING.md). PaintBrush follows troyhacks's look, with `glide` and `surge`: [PaintBrushEffect.h](../../src/light/effects/PaintBrushEffect.h). `AudioSpectrumEffect`'s level meter ticks the gate on the level's own scale, and `SpriteFountainEffect` throws a sprite on each drum hit: [AudioSpectrumEffect.h](../../src/light/effects/AudioSpectrumEffect.h), [SpriteFountainEffect.h](../../src/light/effects/SpriteFountainEffect.h).

- **Bench:** PaintBrush, MovingHead and SpaceInvaders side by side before and after, on the step 5 drum material and a long pop song, judged by eye for strokes that sweep and heads that move on the drums. Every effect reading `onset`, `level` or the bands needs its look judged again, and `levelSmoothed` on the peak-meter ballistic rises faster than the old average, so an effect tuned to the old lag reads livelier.

### 8. Audio sync carries the new engine 🧪

Built: one packet a frame in the format the Audio card's `syncFormat` picks, the MoonLight format carrying the whole frame in skippable sections and the WLED format mapped in one place, the hits between two packets riding in the next, and one receive path telling the formats by their header: [Audio sync format](../reference/audio-sync.md), its one home, and [AudioService.h](../../src/core/services/AudioService.h).

- **Bench:** a board on `WLED` feeding the WLED-MM board (.223), which reacts as before (2026-11-07: .223 on receive takes .221's packets as V2 audio sync, its sixteen bars and peak following the music; the look is the product owner's to judge); a board on `MoonLight` feeding a MoonLight board, whose AudioSpectrum shows the thirds, the range hits, the harmony and the keys as the sender's does.

### 9. The notes a player could play 🧪

Built: a `notes` select on Keyboard and Strings, `all`, `no overtones` and `hands`, over the harmonic sieve and peak picking in [HarmonicSieve.h](../../src/core/util/HarmonicSieve.h) and the hand models in [playerHands.h](../../src/light/powerfunctions/playerHands.h): two hands of five keys within a tenth on the keyboard, one note a string within four frets on strings. A hand follows the notes being struck, a recent strike weighing 32 times its level, and the sieve spares a note struck on its own with a level, since an overtone starts with its note; on a mixed rock track the hand still stays low, which step 11 measures.

- **Showcase:** Keyboard and Strings themselves, on the plan's solo piano and guitar songs, judged by eye against the score: fewer stray keys and dots, the chord shapes a player's hands make.
- **Out of scope:** labeling notes by hand or coloring the hands; it would be a best guess, about 90% on exact MIDI with pitch and continuity, lower on notes read from audio, and wrong where the hands cross.

### 10. How a string was played 🧪

Built: Strings is laid out as the real instrument, the body and bridge at the left and the frets closing in toward it, 19 on the guitar, 20 on the bass and two octaves on violin and cello, and draws a strum as a sweep in its direction, a sounding string lit from the bridge to its fret, and on violin and cello a bow across the strings that strokes while a note is bowed, turning at each new note and faster the louder, read from the notes' starts and levels: `StringPlay` in [playerHands.h](../../src/light/powerfunctions/playerHands.h) and [StringsEffect.h](../../src/light/effects/StringsEffect.h).

- **What the audio does not show:** which finger or whether a pick plucked a string, the bow's direction beyond a guess from where notes start, palm muting and other technique: none of it reaches the sound in a form the engine reads, and a fast strum landing in one block has no direction.
- **Showcase:** Strings on a strummed acoustic guitar song and a solo cello piece with pizzicato passages, from the plan's playlist.

### 11. The notes are measured 🚧

Built (2026-11-07): `mm_tempo --notes` runs the effects' own `keep`, moved out of their drawing so the harness and the effects share it, and `bench_notes.py` scores each stage with mir_eval, reading GuitarSet, URMP, audio beside its MIDI and a generated set: [MoonDeck.md](../../moondeck/MoonDeck.md#bench_notes).

- **First measurement, the generated set** (a scale, triads, a solo over a held chord, a melody over a bass; recall at 50 ms): the engine 87%, `no overtones` 87%, Keyboard's hands 62%, Strings' hand 56%, with `chord` 53% and 25%. Precision stays under 16% at every stage: the notes invented are mostly flashes, an attack lighting the high keys for one to three frames.
- **What it found:** a note a semitone above the one before, F4 after E4, marks no onset, its key already lit by the leakage; over a held chord Strings' hand keeps 36% of the solo where `no overtones` keeps 93% and Keyboard's hands 86%, and with `chord` none, the held chord's four strings triggering the snap.
- **GuitarSet, all 360 recordings** (2026-11-07), solos and accompaniment apart, recall and precision: the engine finds 64% of a solo's notes and 38% of an accompaniment's, the strummed chords' repeated strikes marking no onset on keys still sounding; Strings' hand keeps 57% and 30% at 9% and 13% precision. Each played note brings about 14 flashes in a solo and 5 in an accompaniment, most of every stage's errors. The hand puts 15% of solo notes and 22% of accompaniment notes on their labeled string.
- **Next, in that order, each measured before and after:** the attack's flashes, a key counting as sounding only once it outlasts the attack; the strike on a key still sounding, the repeated chord and the semitone step; the hand's placement on GuitarSet's strings. MAESTRO is 108 GB, so piano runs on MIDI rendered through FluidSynth and the Salamander piano; URMP waits for its request form.

Steps 3, 9, 10 and 12 read notes from audio, and so far the bench judges them by eye; on a mixed rock track that cannot tell a solo note from an overtone or a drum's click, which stalled tuning the fretting hand by ear (2026-11-07). This step gives them a number, as [the tempo harness](Plan-20261103%20-%20Tempo%20evaluation%20harness%20(2%20of%205).md) does for the tempo: music whose notes are known runs through the boards' own analysis on the desktop, and the notes the engine keeps are scored against the score.

**Known notes, per instrument the effects draw.** Each stays outside the repository, as the tempo harness's datasets do; the script takes a local path and prints where each set comes from.

- **Piano:** MIDI rendered through a freely licensed piano sound font, such as the Salamander Grand Piano, with FluidSynth at the engine's rate, every note's pitch, start and end exact; and MAESTRO (Google Magenta, CC BY-NC-SA), competition performances on instrumented pianos, which brings the room, the pedal and the microphone in.
- **Guitar:** GuitarSet (Xi et al., ISMIR 2018, CC BY 4.0), acoustic guitar recorded with a pickup on every string, each note labeled with its string and fret, so the fretting hand is scored on where it places a note as well as which; and MIDI solos rendered through the General MIDI overdriven and distortion guitars, the electric solo high on the neck that GuitarSet lacks, synthetic but exact.
- **Violin and cello:** URMP (Li et al., 2018, CC BY-NC-SA), chamber pieces with each instrument recorded apart and its notes annotated, for the bowed strings and the bow.
- **Chords:** step 12's sets, chords rendered from MIDI and Isophonics' annotations.

**The engine as the boards run it.** The tempo harness's `mm_tempo` feeds decoded audio through `AudioService::analyzeForTest` and prints a line a frame; it gains the frame's sounding notes and their hits, and each stage the effects apply, the sieve, the peak picking, the hands and the chord snap, with the hands' placement (key, or string and fret), so each stage is scored on its own: what it keeps of the notes played, and what it drops.

**Scored by the field's measure.** Note precision, recall and F-measure with `mir_eval`'s transcription scores: a note counts when its pitch matches and its start falls within 50 ms, and again with its end within a fifth of its length. Reported per stage and per texture, a melody over a bass, a solo over held chords, slow and strummed chords, dense passages and the pedal, and for GuitarSet the share placed on the labeled string and fret, with octave errors and leakage to a neighbor counted apart, since those are the errors the sieve and the peak picking target.

**What it tunes.** The sieve's margin, the peak picking's margin, the hands' 250 ms delay and their weight on a recent strike, and whether a struck note out of a hand's reach moves it at once, the rule left open on 2026-11-07, are set to the values that score best, instead of by eye; each later change to the note path is measured before and after, as the tempo tracker will be.

- **Desktop tests:** a rendered C major scale scores every note at `all`; a rendered chord with its overtones scores better at `no overtones` than at `all`; a rendered solo above held chords keeps its notes under `hands`; the scoring itself is checked on a set whose engine output is the score.
- **Depends on:** the tempo harness's `mm_tempo` and `bench_tempo.py`, so it extends them rather than adding a second harness; FluidSynth beside FFmpeg on the machine that renders; `mir_eval` in the script's dependencies.
- **Out of scope:** a neural transcriber; the field's 90% and more comes from networks a microcontroller cannot run, so this measures what the boards' own engine achieves.

### 12. The chord is named 🧪

Built: the chord named by template matching, nine qualities on twelve roots, changing only at a harmonic change, in [AudioChords.h](../../src/core/services/AudioChords.h); `noteNames` writes it beside the key, and a `chord` checkbox snaps the note effects to it, a Keyboard hand of three keys or more and Strings' fretting hand, once three strings or more sound, taking the chord's shape ([KeyboardEffect.h](../../src/light/effects/KeyboardEffect.h), [playerHands.h](../../src/light/powerfunctions/playerHands.h)).

- **Measured by step 11:** chords rendered from MIDI, and the chord annotations of a public set such as Isophonics' Beatles, scored for the share of time the named chord is right, the field's measure.
- **Expected:** template matching names roughly two chords in three on popular music in published comparisons, a recalled order of magnitude to confirm with step 11, and costs 108 short correlations a change, nothing on the render path.

## What an effect reads

[`AudioFrame`](../../src/core/util/AudioFrame.h) is the whole contract: one flat struct per tick, every field a small integer, read through `AudioService::latestFrame()` by an effect and through builtins by a MoonLive script. The plan widens the struct and nothing else. Step 6 adds `bpm`, `beatPhase`, `beat`, `downbeat` and the beat in the bar, with `audioBpm`, `audioBeat` and `audioDownbeat` in MoonLive, and audio sync carries them in [the beat section](../reference/audio-sync.md#a-proposal-the-beat-section).

## What it costs, per chip

The engine's measured share of a core per chip is in [AudioService.h](../../src/core/services/AudioService.h). Step 6 adds an autocorrelation or comb over 12 s or more of the onset curve, a phase fit, and the harmonic changes lined up with the grid: about 16,000 multiply-adds per block, estimated well under a millisecond on a classic ESP32, to be measured on the card's `cpu`.

## The showcases

Steps 1 to 5 and 7 landed with `WaterfallEffect`, the harmony palette, `BeatRipplesEffect` on the kick, `DrumKitEffect` and `PaintBrushEffect`; step 6's are in its section.

More effects on the same signals, each a small change once its field exists: **a harmony color**, the rig tinted by the twelve pitch classes, from step 3; **three zones** (bass on the floor, mids between, treble at the top, breathing with their range) from step 4; **`PulseEffect` and `PaintBrushEffect` on the beat**, one shell or stroke per beat, from step 6.

## Backlog

On the topic and not a step. What remains here when the plan ships returns to the Project as cards, item by item, then.

- **A sync packet for every band count.** WLED receivers accept only the 44-byte `00002` packet and drop anything over 96 bytes, so the sixteen stay in it and the bands travel in a second packet type under its own header, which WLED ignores. It describes itself: bands to the octave, the reference frequency, the first band and the count, then the values (about 45 bytes for 31 bands, 135 for 121). A receiver sums finer bands into the coarser ones its effects read; a coarser packet cannot give finer bands, so an effect that needs them idles. MoonLight decides the format (2026-11-03).
- **Troy's display settings, for a like-for-like comparison.** The pattern that shifts the sixteen −3 to +3 bands, the dB window span, auto or fixed range, the dB reference, attack and release. A run pins each on both boards.
- **Calibration**, what a rig cannot show, taken when a user or an effect asks: a biquad pre-filter chain (rumble high-pass, anti-alias low-pass) from esp-dsp's `dsps_biquad_f32`; calibration against a measured chain and a self-test on the card. Troy's rewrite has both, checked against a hardware analyzer.
- **More audio effects on the bands**, 2D and palette-driven frequency-reactive ones, beside [the showcases](#the-showcases).
- **Pin auto-scan**: detect the microphone's `sdPin` with `wsPin` and `sckPin` fixed, a noise prompt and a confirm; explicit pin controls ship today.
- **Sources beyond the I²S microphone**, each a variant behind the same platform read, the analysis untouched. The I²S peripheral already drives MCLK (`mclkPin`) for a line-in ADC, and `codecGain` sets an ES8311 or ES7210 between microphone and line level.
    - **PDM microphones**: the IDF `i2s_pdm` driver, a sub-mode of the same peripheral.
    - **Analog line-in**: DedeHai has it on the S3 and Troy in ParrotRadio, where he recorded and played back through it. An analog path is validated by listening to it, since one that only feeds a level meter "may not be as accurate as it looks" (Troy).
    - **Multi-channel front-ends**: a TDM codec (the ES7210 on the Waveshare RGB-Matrix board) or a DSP board giving one analyzed channel per instrument, the input [the live-performance plan](Plan-20261013%20-%20Audio%20reactive%20live%20performance%20(0%20of%207).md) needs for acoustic drums.
- **A fast roll as separate hits (2026-11-05).** A band's onset keeps strokes apart up to about 10 a second; at 20 a second it fires 2 of 10, since the band falls only about 14 dB between strokes through the bank's 10 ms detector and the flux-against-its-mean decision raises its own threshold. A rise measured from the band's recent low point, rather than against its mean flux, would keep a snare roll's strokes; a desktop test pins 20 a second.
- **An instrument tuner (2026-11-04).** The semitone bands say which note sounds, not how far off it is, and a tuner needs 1 to 2 cents, a hundredth of a semitone. The bank picks the note; a period detector on the waveform itself, YIN (de Cheveigné and Kawahara 2002) or McLeod's normalized square difference (2005), the textbook pitch detectors, measures it to a tenth of a cent, for one instrument through line-in, never a full mix. The note's power against its two neighbors, as `peakHz` reads the third octaves, gives a few cents at best: enough for flat or sharp. Shown on AudioSpectrum's `noteNames` line as the note and its cents, and readable by an effect, a strip lit green when in tune. Not step 3's tuning estimate, which measures how far a recording sits from A440 over many notes.
- **Desktop audio capturing what the machine plays (2026-09-29).** `platform_desktop_audio.cpp` enumerates capture devices only (`ma_context_get_devices` with null playback lists) and opens `ma_device_type_capture`, so on a Windows bench the picker offers `default` and the microphone and the speakers are absent, with no Stereo Mix to fall back on. miniaudio has `ma_device_type_loopback`, WASAPI-only, taking a playback device: enumerate playback devices too, mark them in the picker so a microphone and "what is playing" tell apart, and open loopback when one is chosen. macOS has no equivalent and needs a virtual device such as BlackHole, so the picker offers what the platform can do rather than a row that fails. A disconnected microphone jack reports a plausible level (RMS 20 to 30 at mains hum), which the status of step 2 tells from a working input.
- **The input's rate, and 48 kHz on the P4 (2026-11-04).** The measured rate wanders, 20,700 to 22,300 Hz on the classic and 23,500 to 24,700 on the S3; at 48 kHz the P4 loses a fifth of its frames for 3% more of a core, the audio task waking the render core twice as often being a hypothesis.
- **The onsets' own cost (2026-11-05).** They add 5 to 7 points of a core, more than the per-reading arithmetic predicts; a measurement with the onsets alone tells them from the boards' other load.
- **A `freeze` toggle**, holding the floor and the top, if a show asks for deterministic levels.
- **A fixed-point bank** if a C3, which has no FPU, ever analyzes audio, on Damian (DedeHai)'s reasoning from WLED's fixed-point FFT.

## What it removes

Steps 1 and 2 removed the FFT with its window and the platform's `audioFft` seam, the loudest-bin estimator, the band conditioners and the `levels` and `gain` controls; step 7 the broadband flux onset, the level-against-average beat test in three effects and a builtin, and five copies of the silence threshold. In the docs, the audio DSP roadmap, the audio-reactive follow-ups and the desktop-capture and noise-floor items moved into this plan, and per-band onsets left the live-performance plan for step 5.
