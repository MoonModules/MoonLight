# blazoncek/WLED: monthly activity digest

What landed on [blazoncek/WLED](https://github.com/blazoncek/WLED)'s `dev` branch, month by month. External-context reference, a factual log of a friend repo's activity, not MoonLight's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](index.md).

This is the personal fork of upstream [wled/WLED](https://github.com/wled/WLED) by Blaž Kristan (blazoncek), a long-time WLED core developer; the fork's builds call themselves "WLED-BB" and track the 0.15 line. It carries his own work ahead of or apart from upstream: segment blending modes and transitions, an alpha (transparency) color channel, bus-manager and LED-driver rewrites, HUB75 matrix support, memory tuning for large installations, and many effect rewrites. Several of these are later offered upstream as pull requests.

**Branch note.** The fork's default branch is `stable`, a release line that has 6 commits in the whole window (2025-12-31 and 2026-01-03, the "WLED-BB 0.15.2" release and four follow-ups). All day-to-day work lands on `dev`, so the months below summarize `dev` (first-parent view). Two side branches also saw work: `bus-memory` (HUB75 and bus memory calculation, January 2026) and `fastled_replacement` (a FastLED replacement brought in from upstream, March to June 2026). No versioned GitHub release is published (the releases list is empty and the issue tracker is disabled), so months are kept whole.

## September 2026

35 commits on `dev`, most of them effects, LED current limiting and bus work.

- New **LED current limiting** for APA102, SK9822 and HD108 strips, and for TM1814 and SM16825 (set in mA per LED), which gives finer brightness steps at low brightness.
- Gamma correction is now applied before brightness, and the old custom raw gamma table is removed.
- New and reworked effects: **Twinkles 2D** (with wrapping, then drifting like snowflakes), a filled DNA spiral with a variable trail, smoother Colored Bursts, and several effects fixed for the RGBW (white channel) color type.
- Effect search hides 2D effects on segments that are not 2D.
- Timer presets are simplified, and usermod settings sections can be collapsed.
- SD card files can be served over the web, and the code is tuned for PSRAM, for a "final tuning before 0.15.3 release" commit (2026-09-12).
- The active preset is no longer cleared by JSON requests during a brightness transition (from upstream, #5856).
- Library dependencies are all resolved by URL so corporate proxies do not block the build, and the old IDF 3 environment is dropped.

_Checked: 35 first-parent commits on `origin/dev` with author-date 2026-09-01..2026-09-30 (newest 36c82dfb, oldest 2194d02a), via a local blobless clone and `git log --first-parent`. The default branch `stable` has no September commits (tip b8c96235, 2026-01-03). Releases published: none (`repos/blazoncek/WLED/releases`). Issues: the tracker is disabled on this fork (`has_issues` false), so there are no issues to search. Release-pipeline commits, build bumps, comment edits and const tweaks are omitted._

## August 2026

65 commits, the busiest month of the window: a drawing-primitives and transparency pass, HUB75 rework, and memory tuning.

**New**

- Circles and ellipses are redrawn with finer fixed-point math, plus a blurred ellipse, gradient lines and smoother lines; effects Blobs and Ripple are updated to use them.
- Effects can use **transparency**: segments can fade out to transparent, clear to transparent black, and fades use a parabolic curve instead of a linear one.
- The segment blending fast path is now on by default, and it honors rotation and per-pixel opacity.
- **HUB75 simplified** (breaking, needs the config saved again): no reboot after a config change, simpler panel selection, panels 40, 48 and 80 pixels high, and a build for ESP32-S3.
- "PixelForge Lite" (the image and scrolling-text interface) gets fixes and optimizations.
- Memory work for very large matrices: lower output bit depth, PSRAM tuning and cache pre-loading, and an attempt at RTC RAM.

**Fixed**

- Legacy sunrise and sunset timers load correctly again, and a timer bug is fixed.
- Rotation can go both clockwise and counter-clockwise.
- MAC address inputs are improved (upstream #5797), and a small `drawCircle` fix follows upstream #5798.
- A crash in the Scrolling Text effect from a null reference is fixed.

_Checked: 65 first-parent commits on `origin/dev` with author-date 2026-08-01..2026-08-31 (newest e8957110, oldest 70e33be0), via `git log --first-parent` on a local blobless clone. One commit dated 2026-02-11 (04ea3fc8) merged in August and is counted in February by author-date. Releases published: none. Issues: tracker disabled on this fork. Build bumps, comment and const-only commits omitted._

## July 2026

35 commits: SD card support, HUB75 boards and bufferless segments.

- **Full SD card support** in the file editor (SPI SD card, upload and edit files from the UI), replacing an older LittleFS library.
- **Bufferless segments** for very large installations: draw straight to the strip buffer, ignore segment layering, and use simpler transitions limited to color and palette.
- More HUB75 boards and options: Waveshare ESP32-S3-RGB-Matrix, a Seengreat v1 pinout, 2x3 and 2x4 panel layouts, a chain orientation option, and fixes for DMA memory and I2S speed.
- New **Import Image** effect (from Aircoookie), and a fix in the Stream effect.
- A usermod JavaScript inject API lets usermods change the UI; resources load sequentially in the UI (adapted from a contributor's change).
- Turning WLED off with a non-Fade transition no longer causes a one-frame blackout, and no white pixel is painted on RGBW strips.

_Checked: 35 first-parent commits on `origin/dev` with author-date 2026-07-01..2026-07-31 (newest 6fdf8f19, oldest ccc6be43), via `git log --first-parent`. Releases published: none. Issues: tracker disabled on this fork. Internal refactors and the merge of the `more-timers` branch are omitted._

## June 2026

19 commits, audio-reactive, effects and network fixes.

- New **Flow 2D** effect, and a rewritten Perlinscape effect with rotation again.
- Audio-reactive usermod changes: simulation choice moved into the info dialog, palette recreation improved, and microphone type "none" means network receive mode.
- A Seengreat v2 HUB75 pinout for ESP32-S3.
- UDP sync is applied to all selected segments (upstream #5705), gamma is applied consistently to live data (upstream #5661), and IR and ESP-NOW remote handling no longer depends on the global JSON buffer.
- An internal pseudo-random-number namespace replaces scattered helpers, and FastLED functions move into a slim replacement library.
- The `fastled_replacement` branch picks up the same changes (tip 79ea6edd, 2026-06-08).

_Checked: 19 first-parent commits on `origin/dev` with author-date 2026-06-01..2026-06-30 (newest a752e494, oldest 1bbfc619). Releases published: none. Issues: tracker disabled on this fork._

## May 2026

30 commits: transitions and on/off handling were reworked.

- **Transitions reworked**: a segment blending fast path, smooth color change even while a transition is running, a faster push transition, and a corrected Off transition (peek preview brightness included).
- On/off and brightness handling is rewritten and a debug statistics option is added; the rarely used `WLED_SAVE_RAM` build option is removed.
- Nightlight no longer interferes with transitions (upstream #5620) and restores the original brightness when canceled.
- Better network connect: the best SSID is stored for the next boot and a BSSID change forces a reconnect.
- Improv input handling is more robust, and the ledmap is read without the global JSON buffer.
- New boards and effects: Huidu HU-FW2 HUB75, the Perlinscape effect by a contributor, and a color desaturation helper.
- Dual WS2811 is removed as a duplicate of FW1906.

_Checked: 30 first-parent commits on `origin/dev` with author-date 2026-05-01..2026-05-31 (newest e4f008bc, oldest 47faf977). Releases published: none. Issues: tracker disabled on this fork._

## April 2026

8 commits, mostly fixes.

- 2D transition and blending style options no longer show as enabled when they do not apply (upstream #5498).
- New MQTT sync and notification filter, and DDP receive and notification cleanups.
- The gap file is read without ArduinoJSON, which avoids using the global JSON buffer (upstream #5518).
- A build tagged 0.15.3-b2 (2026-04-19).

_Checked: 8 first-parent commits on `origin/dev` with author-date 2026-04-01..2026-04-30. Releases published: none. Issues: tracker disabled on this fork._

## March 2026

3 commits.

- A proof of concept for WS281x RGB+CCT strips (dual chip).
- A transmit-power bug (set too early at init) and a DDP packet-count tweak.
- On `fastled_replacement`, three commits (2026-03-21) bring in upstream's FastLED replacement.

_Checked: 3 first-parent commits on `origin/dev` with author-date 2026-03-01..2026-03-31. Releases published: none. Issues: tracker disabled on this fork._

## February 2026

17 commits.

- **Per-output brightness scaling** (a brightness factor per LED output).
- Exclusive CCT, a DDP invalid-packet filter, and an audio PLL fix for audio-reactive with Ethernet.
- Custom JSON timezone support, with an example file for the Chatham time zone.
- The inverse gamma option is removed, and segment effects no longer speed up during a transition.

_Checked: 17 first-parent commits on `origin/dev` with author-date 2026-02-01..2026-02-28. Releases published: none. Issues: tracker disabled on this fork._

## January 2026

36 commits on `dev`, plus 4 on `stable` (2026-01-03).

- **HUB75 support** and a delay of LED output after a relay switch (a5517478, 2026-01-31); "blending style" is renamed "transition style" in the UI, and a custom NeoPixelBus fork is adopted.
- New HD108 LED type (16-bit, 2-pin), and bit-banging output is dropped on ESP8266.
- Gamma correction and brightness scaling move into the bus manager.
- Effect updates: Pride, Colorwaves, Palette, 2D Drift, Drift Rose, Ghost Rider and Rotozoomer.
- Open files are no longer corrupted by writes to an open object (credit to a contributor, upstream #5276), and preset saving is fixed.
- On `stable`: ledmap loading, palette preview, and WebSocket broadcasts that wait while the JSON buffer is locked.

_Checked: 36 first-parent commits on `origin/dev` and 4 on `origin/stable` with author-date 2026-01-01..2026-01-31. The `bus-memory` branch carries the same HUB75 work (tip f768e07b, 2026-01-27) and `simpler-tz` carries the timezone work. Releases published: none. Issues: tracker disabled on this fork._

## December 2025

42 commits on `dev`, plus 2 on `stable` (2025-12-31).

- **Segment zoom and rotate** (by a contributor, upstream #5202) with wrapping and mirroring options.
- **Particle system update**: 1D particle effects and the particle fire are removed, particles carry their own size and mass, collisions use real radii, and fireworks explosions become round.
- New LED type: dual WS281x chip RGBW strips; new effect Moving Octopus.
- Circle and ellipse drawing use fixed-point coordinates, and Wu pixel drawing is optimized.
- The global AWM (auto white mode) setting is removed in favor of a runtime override in JSON and HTTP.
- "WLED-BB 0.15.2" lands on `stable` on 2025-12-31 (f96941f0), with a bus-wrapper rewrite, RMT fix, variable buttons, the CRGBA alpha channel, segment rotate and zoom, and non-gamma-adjusted palettes. A 0.15.3-b1 build follows on `dev`.

_Checked: 42 first-parent commits on `origin/dev` and 2 on `origin/stable` with author-date 2025-12-01..2025-12-31. Releases published: none on GitHub; the versions are named in commit messages only. Issues: tracker disabled on this fork._

## November 2025

18 commits.

- The `rmt-hi` branch (the high-priority RMT driver) merges into `dev` on 2025-11-28.
- A dropdown for LED output pins, unique LED types with reserved GPIO, and UI memory calculation.
- Fire 2012 bug fix and effect tuning; assorted UI and flash-size reductions.

_Checked: 18 first-parent commits on `origin/dev` with author-date 2025-11-01..2025-11-30. Releases published: none. Issues: tracker disabled on this fork._

## October 2025

24 commits.

- **LED outputs can be re-arranged by drag and drop**, and 1D arc expansion is faster.
- Effects consolidated: Blink and Strobe (with a color time/space swap), Running and Saw, Chase, Gradient and Loading, Twinklefox and Twinklecat, Spots and Spots Fade, Phased and Phased Noise.
- Bouncing Balls uses integer math: about 4x faster on ESP8266 and 2x on ESP32.
- The custom palette editor is aware of local files, and upstream's wled-tools are added.
- The CRGBA alpha-channel branch merges with the particle-system import branch.

_Checked: 24 first-parent commits on `origin/dev` with author-date 2025-10-01..2025-10-31. Releases published: none. Issues: tracker disabled on this fork._

## September 2025

20 commits, the alpha channel arrives.

- **Alpha (transparency) color channel**, CRGBA (067b23ff, 2025-09-09), which can replace FastLED's CRGB; it adds the **HSV and Stencil segment blending modes** and updates effects to be alpha-aware. A **Chroma** blending mode came just before the window (ce04f1bd, 2025-08-31).
- **True variable custom palettes**: more than 10 allowed through a build setting (ce7e0499, 2025-09-11; a5a24fd2, 2025-09-15).
- Virtual buses are no longer limited in count (upstream #4924), and temperature resolution is variable (credit to a contributor, upstream #4916).
- Better handling behind a reverse proxy, a fix for more than 10 buses, and bus-creation fallbacks.
- Shimmer effect (upstream #4923, authored 2025-09-24, merged into the fork's `dev` in November).

_Checked: 20 first-parent commits on `origin/dev` with author-date 2025-09-01..2025-09-30, via `git log --first-parent` on a local blobless clone. Releases published: none. Issues: tracker disabled on this fork._
