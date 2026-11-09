# Plan: features computed on demand

A provider computes a feature only while some module reads it, through the core's [Demand](../../src/core/module/Demand.h), which the audio plan's notes use first. This plan carries it to the other providers that compute every block whether or not anything reads the result, found by a survey of the tree (2026-11-09).

## The design basis

- **Control, not data.** A demand decides whether a provider computes; the data still travels by shared pointer, zero-copy, as `latestFrame()` and a Layer's buffer do.
- **Nothing per frame.** A demand changes at a prepare or a release, and the provider counts the tree again then; the hot path and the memory of a board without PSRAM stay as they are.
- **Only where it pays.** A feature earns a demand when it costs real CPU or RAM and often runs unread. A cheap one, or one its own consumer already gates (connected clients, an open socket, a configured pin), stays as it is.

## What exists

- **Demand** ([Demand.h](../../src/core/module/Demand.h)): a reader names the provider's type and a feature and turns its demand on in its prepare; the module's release turns it off; the provider's `onDemandChanged` counts the tree again.
- **AudioService's notes** ([AudioService.h](../../src/core/services/AudioService.h)): the semitone bank runs while a note effect, a bar effect showing the keys, AudioSpectrum's pitch classes or note names, an effect script calling a note builtin or the MoonLight sync format reads it.

## Steps

### 1. The audio analysis runs while something reads it ⏳

In local mode the audio task analyzes every block, the third octaves, the onsets and the dynamics, whether or not anything reads the frame: 9 to 15% of a core (the third octaves 3.6% of a P4, 7.2% of an S3 and 8.2% of a classic ESP32 at 24 kHz, the onsets 5 to 7 points more, [AudioAnalysis.h](../../src/core/services/AudioAnalysis.h)), unread whenever a look without an audio effect runs on a board with a microphone.

- **The feature:** `AudioFeature::Bands`, beside `Notes`. The audio effects declare it, those with an `audio` switch while it is on; effect scripts through the compiler, as the note builtins do; sending audio sync counts as the service's own want.
- **What stays read:** the card's level, dynamics and peak readouts and the silent-microphone diagnosis are how a microphone is wired before any audio effect is on. The input stays open and read, which keeps the diagnosis, and only the analysis pauses; how the readouts show a paused analysis is decided in the step.
- **Measured by:** the card's `cpu` with an audio look and without, on an S3 and a classic ESP32.

### 2. The USB capture decodes while the Picture effect shows it ⏳

With a USB grabber as its source, PictureService decodes every frame on a task of its own into one of two pictures of width × height × 3 bytes ([PictureService.h](../../src/core/services/PictureService.h)), whether or not a Picture effect shows it.

- **The feature:** PictureService's `Picture`, which the Picture effect declares. Its `onDemandChanged` starts and stops the capture task, as its prepare and release already do, and frees the pictures while nothing reads them.
- **Measured by:** the decode task's share of a core and the pictures' RAM, with a Picture look and without.

## Not candidates

The survey found the rest already gated or cheap: the preview stream is a pull with no work unrequested; the HTTP state and WLED frames go out only to connected clients; the OSC socket opens only when needed; MIDI, infrared, buttons and analog inputs read only what is configured; the host lookups are lazy; the one-a-second readouts format a string. A feature whose readers are UI clients or network peers has its own gate there, outside Demand.

## What it removes

The analysis held by a board that shows no audio look, and a capture task decoding frames no effect shows.
