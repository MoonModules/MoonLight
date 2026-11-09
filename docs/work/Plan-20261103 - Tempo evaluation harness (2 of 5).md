# Plan: tempo evaluation harness

The desktop build runs real music through `AudioService` faster than real time, and a MoonDeck script scores the tempo it reads against labeled tracks: Acc1 and Acc2, per genre, with the 3:2 readings counted apart. The tracker itself is [the audio plan's step 6](Plan-20261028%20-%20Audio%20that%20answers%20the%20music%20(11%20of%2012).md); the harness lands first, so that step is measured while it is built, and any later tracker is judged on the same footing.

The yardstick is the field's own: Gouyon et al., *An experimental comparison of audio tempo induction algorithms* (2006), which defines Acc1 and Acc2 at 4%, as Schreiber and Müller (2018) use them, and `mir_eval` for beat and downbeat scores once the frame carries a phase.

## The design basis

- **The analysis is the one the boards run.** The harness feeds samples to the same block analysis the render tick calls, through a seam, so the flux and onsets scored are the shipped ones. The tempo read from them is a baseline in Python, Ellis's autocorrelation, for as long as the [baseline stays](#what-it-removes).
- **Faster than real time.** A block's time comes from its sample count, not the clock, so a two-minute track takes seconds and a thousand tracks take minutes.
- **Decoding stays out of the firmware.** FFmpeg decodes any format to mono samples at the service's rate in the script; the C++ side reads raw samples on stdin and builds no codec.
- **The agent runs, the person reviews.** Runs come from the command line, an agent's along the [Measure the tempo](../moonwalk.md#measure-tempo) process, which agrees each run with the product owner first; MoonDeck's tempo cards only show results and start nothing. A card that needs the person, such as tapping, waits until they say they are ready.
- **Datasets stay outside the repository.** The script takes a local path. It prints where open sets come from and generates its own test tracks.
- **Every label carries its provenance.** A score against a tapped label is accuracy; a score against another program's estimate is agreement. The report keeps the two apart.

## What exists

- **`AudioService`** ([AudioService.h](../../src/core/services/AudioService.h)): a 512-sample block analyzed on an audio task of its own, its onsets timed by the stream's own clock (the samples read over the rate), and `analyzeForTest(block)`, the host's seam into that analysis.
- **The micro-bench precedent:** `mm_bench` ([bench_kernels.cpp](../../test/bench/bench_kernels.cpp)), a host binary that [bench_kernels.py](../../moondeck/check/bench_kernels.py) builds alone and reports on, a report rather than a check.
- **`MidiService`** with Web MIDI, USB MIDI host and RTP-MIDI, which the tap tool's MIDI route mirrors on the computer.
- **FFmpeg**, which MoonTube's scripts already call.

## Prior art

- **MatrixPrism's tempo evaluation** (Anjin, 2026-11-03): the scoring rules here (Acc1, Acc2, a 2% tolerance beside Gouyon's 4%, per genre, 3:2 apart, paired comparison by exact McNemar test), run on the Freesound Loop Dataset and other sets against several published trackers. The harness carries the rules over; its code is written fresh for MoonDeck.
- **`mirdata`** (BSD-3-Clause): loaders and download scripts for GiantSteps Tempo, Ballroom and the Groove MIDI Dataset, which the harness reuses rather than writing its own.
- **`mir_eval`** (MIT): the textbook beat, downbeat and tempo metrics.

## Steps

The steps land as pull requests based on main, each as large as its topic needs with the maintainer's agreement and ending in a desktop run: the seam, the runner, the report, the generated set, then the history, the open sets and tapping.

### 1. The seam into the analysis ✅

- `analyzeForTest(block)` runs a block through the analysis the input runs, on the stream's own clock, so a file fed faster than real time reads as it would live; it landed with the audio rewrite (#232). `mm_tempo` calls it after `resetAnalysisForTest()`, and this plan adds nothing to `AudioService`.

### 2. The harness on generated tracks ✅

- **`mm_tempo`** (`test/bench/bench_tempo.cpp`): reads s32 mono samples on stdin, steps the clock by the block's length, and writes one tab-separated line per frame after a header naming the rate, the block and the columns: time, level, flux, onset and the sixteen bands, with `bpm`, `beatPhase` and `beat` as columns once the audio plan's step 6 adds them.
- **`moondeck/check/bench_tempo.py`**: builds `mm_tempo` alone, decodes each file with FFmpeg, pipes it through, and writes a run file and a Markdown report.
- **`--generate <dir>`** writes tracks with an exact tempo: click tracks from 60 to 200 BPM, swung and triplet patterns, silence and noise; `--generated` writes them once to `build/tempo/generated` and adds them to a run. A tempo step from 120 to 140 waits for per-section labels.
- **The Tempo Report card** on MoonDeck's Desktop tab shows the latest report in the View pane, through `/api/tempo-view`, and starts nothing; Tempo History and Tempo Sets join it with their steps, as read-only cards too.
- **A test of the whole path:** the generated set through FFmpeg, `mm_tempo` and the baseline, each track's verdict pinned.
- **Comparing runs:** `tempo_compare.py` pairs two run files by the audio's hash, with an exact McNemar test on the tracks right in one run only and every track whose reading changed; `--last` takes the two newest runs on the same files. It answers whether a change to the analysis helped.
- **The baseline:** Ellis's autocorrelation of the frame's flux under a log-normal prior at 120 BPM, which reads the generated clicks within 2% from 60 to 160 BPM and 174, 185 and 200 at half: the prior weighs a tempo and its half alike at 120 × √2, about 170 BPM, and the half wins above.

### 3. Real music, with provenance 🚧 the open sets' own labels, rekordbox and Traktor exports

- **`tempo_sets.py`** gives each open set one complete line: fetch it into `~/tempo-data/`, read its own labels with `--set` (FSL10K directly; Ballroom, GiantSteps Tempo and Groove MIDI through mirdata), and score 20 files. FSL4 is left out, its authors recommending FSL10K ([Getting test audio](#getting-test-audio)).
- **`--dir <path>`** reads a folder of audio recursively, with labels from a `labels.csv`, a rekordbox XML or Traktor NML export, or the files' tags.
- **Scoring** per provenance and per genre: right within 2% and 4%, Acc2, double or half, 3:2 and 4:3, then every miss with its ratio and verdict. Track names show unless `--private` swaps them for a hash of the audio.
- **History:** every run appends to `build/tempo/history.jsonl`, which the Tempo History card shows with a link to each run's report and the `tempo_compare.py` command for each pair of runs on the same files.

### 4. Tapping labels ⏳

- **`tap_tempo.py <file>`** plays the track and records two kinds of tap: **tap** on every beat and **downbeat** on the first beat of a bar, which counts as a beat too.
- **Two inputs.** A MIDI pad (any pad controller or drum pad; calibration learns the beat pad, any other is the downbeat), and the keyboard of a macOS or Linux terminal (space and D). The Tempo Calibrate and Tempo Tap cards drive the pad; the keyboard runs from the command line.
- **Latency calibration:** `tap_tempo.py --calibrate` plays a 120 BPM click, and the median offset of the taps is passed as `--offset-ms` from then on.
- From the taps: the tempo (the period at which the taps line up best, refined by a least-squares line through the taps on that grid: a tap between beats is left out and counted, a missed beat costs nothing), how sure it is (not saved, and asked again, under 16 taps on the beat, outside 40 to 300 BPM, past 0.5%, on a doubled tap, or with more than a quarter of the taps after an 8-tap warm-up off the beat or of the beats missed), the meter (beats between downbeats) and the bar phase. The taps are kept beside `labels.csv`. On a pad, the pad pressed most is the beat, or the one calibration saved, so the pads keep their roles from take to take.

### 5. Beat phase and downbeats ⏳

- Once the audio plan's step 6 gives the frame `beatPhase` and `beat`, the report scores the beats against tapped or annotated beat times with `mir_eval` (F-measure, CMLt, AMLt), and the bar phase against tapped downbeats.

## Getting test audio

Four routes, which differ mostly in where the labels come from:

1. **Generate it** with `--generate`: public domain by construction, exact labels, part of every run.
2. **Download an open set:**

| Set | Audio | Labels |
|---|---|---|
| [Freesound Loop Dataset (FSL10K)](https://zenodo.org/records/3967852) | each loop's own Creative Commons license, CC0 among them, in `metadata.json` | expert annotators, kept where they agree: 2,201 of 2,936 annotated loops |
| [Groove MIDI Dataset](https://magenta.tensorflow.org/datasets/groove) | CC BY 4.0, drums from an electronic kit | the metronome the drummer set |
| [GiantSteps Tempo](https://github.com/GiantSteps/giantsteps-tempo-dataset) | Beatport previews, from the set's backup at JKU | CC BY-SA 4.0 annotations, the 2018 revision |
| [Ballroom](https://mirdata.readthedocs.io/en/stable/_modules/mirdata/datasets/ballroom.html) | MTG's archive; the sources disagree on its license | hand annotations |

3. **A folder of your own music** with `--dir`. The files stay on your computer. Labels come from tapping (step 4), a `labels.csv` you write (`path,bpm,source`), a DJ library export, or tags: ID3 `TBPM`, the `BPM` Vorbis comment, the MP4 `tmpo` atom (whole numbers). A tag's writer is unknown unless `--tag-source` names it. With no labels at all, the report lists each track's estimate to check by ear or to tap.
4. **A board's own recording**, replayed on the desktop, which separates the microphone from the method.

## Where labels come from

A score is only as good as its label. Each label carries one source, and the report gives a figure per source.

| Source | Origin | A match means |
|---|---|---|
| `tapped` | a trained listener tapping beats and downbeats | accuracy, the gold standard: tempo, the felt level, meter and bar phase |
| `generated` | the harness made the audio | accuracy, exactly |
| `metronome` | the performer played to a click | accuracy, with the performance's own drift |
| `annotated` | people tapped or checked it for a dataset | accuracy against human judgment |
| `uploader` | whoever shared the file typed it | approximate |
| `store` | a shop's catalog value, set by the label or producer | approximate, whole numbers, the shop's conventions |
| `dj-software` | an estimate by rekordbox, Serato, Traktor, djay or Mixed In Key | agreement with another tracker |
| `tag` | a tag of unknown origin | agreement, as `dj-software` |

Three things to watch in any label:

- **Half and double tempo.** Shops list Drum & Bass at 170 to 175 and some dubstep at 70 rather than 140. Acc2 forgives the octave, so Acc1 and Acc2 are read side by side; a tapped label settles the felt level.
- **One label for a whole file** assumes one tempo throughout. A track with tempo changes takes per-section labels, from rekordbox's `TEMPO` entries or tapping, or stays out.
- **Swing and triplet feel** give 3:2 readings, counted apart from plain misses.

## What it costs, per chip

Nothing on a board. The seam is `AudioService`'s own; the binary and the script are host-only.

## Backlog

On the topic and not a step. What remains here when the plan ships returns to the Project as cards, item by item, then.

- **Other trackers as baselines,** each scored beside MoonLight's with its author's agreement and a license that allows it.
- **The next track checked while one plays:** a second thread checks the next untapped track (readable, long enough, decoding cleanly) while the current one is tapped, so it is ready at once and a damaged file never plays; part of the tapping batch.
- **A tap page in the browser,** if the terminal tool proves awkward, on the Web MIDI path the UI already has.
- **Labels from DJ software libraries:** rekordbox XML and Traktor NML exports (step 3), and Algoriddim djay's library, which keeps its BPMs in its own database rather than in the files' tags; each as `dj-software` labels.
- **Tempo read as it runs live:** score the last few seconds every two seconds, the time to the first right reading and the share right after it, as the audio plan's step 6 will read it; the next step after the core lands.
- **A playback level check:** a `--gain` option to show the figures do not move with level.
- **A database of runs,** the next version: every run's tracks, labels and readings queryable across runs, in place of the run files and `history.jsonl`.
- **A report page of its own,** a second pass once the harness has users: one self-contained HTML page per run with sortable misses, filters by genre and verdict, an estimate-against-label scatter that shows octave and 3:2 errors as lines, and two runs side by side, linked from the View pane. MoonDeck's cards and markdown pane carry the minimum viable report.

## Decisions

- **A plan of its own,** linked from the audio plan's step 6: the harness serves that step and outlives it.
- **2% heads the report, 4% beside it.** Gouyon's Acc1 uses 4% (MIREX's tempo task uses 8% and the P-score); 2% separates trackers that 4% ties. Every other ratio is judged at 4%, Acc2's tolerance.

## What it removes

The baseline estimator in `_tempo_baseline.py` is a second tempo tracker, so it goes once the audio plan's step 6 gives `AudioFrame` a `bpm`, and the harness scores the frame's own. That step's synthesized desktop tests run as the generated set, so their tempo cases live in one place. Beyond that, nothing in code: the change adds a host binary, two scripts, the baseline's module and a MoonWalk process, and the process takes the runs off MoonDeck's cards.
