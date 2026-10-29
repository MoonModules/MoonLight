# Backlog: Process and docs

Generated from [the Project](https://github.com/users/MoonModules/projects/5) by `moondeck/report/project_mirror.py`: edit the cards, not this file.

## AudioService

### Flaky unit tests: the AudioService sync suite contends on a fixed UDP port

Kind: Bug · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086639)

**Found:** 2026-07-27, caught by `premerge.py`; pinned to the exact cases by looping the suite and keeping the failing logs. Fails roughly **1 run in 10**.

Two cases in `test/unit/core/unit_AudioService_sync.cpp` fail intermittently, both around the socket rather than the audio logic:

- `:125` *"…drives frame_, then holds it and reports listening"* — `CHECK(strcmp(status(a), "listening") == 0)`. The receive path **binds** the fixed port `kTestSyncPort` (21988); when a previous run's socket is still in `TIME_WAIT` the bind fails and the service never reaches "listening".
- `:95` *"Local+send: lazy-opens once and reports sending"* — `CHECK(strcmp(status(a), "sending") == 0)` plus `syncOpenForTest()`. This one **connects** to a broadcast address rather than binding, so it is a *different* failure mode on the same suite.

**An attempted fix that did not hold (recorded so it is not retried blind).** Deriving the port per run (from the steady clock) instead of the fixed constant cut the receive-path failures but left the send-path case failing ~3 in 20 — because that case never binds the test port, so the port was never its problem. The real fix has to address the send path's socket open on its own terms, not just port choice.

**Why it matters:** the gate scripts run the suite on every commit and merge, so a 1-in-10 flake interrupts gate lists often enough to train the reader to re-run instead of investigate — the habit that hides a real regression later.

**Fix (do not paper over it):** treat the two cases separately. For the receive path, bind an **ephemeral** port (bind 0, read the assigned port back) so two runs can never contend. For the send path, first determine *why* the lazy open fails under repetition: whether the broadcast connect is refused or the latch is left set by an earlier case in the same process. A retry wrapper or a sleep is explicitly the wrong fix: it hides the next genuine failure too. `unit_NetworkReceiveEffect*` (a real UDP round trip on localhost) and `unit_SineEffect` (timing) flake the same way, about 1 full run in 3, and deserve the same treatment while the fix is fresh.

**The quiet-packet case waits on the wall clock** (2026-09-06). `test/unit/core/unit_AudioService_sync.cpp` drives the quiet-packet case with `platform::delayMs(1)`
inside a 100-iteration polling loop, so the assertion that `level` reaches zero depends on real
elapsed time and on loopback UDP delivering within that window. It passes today and has not been
seen to flake, but it is the shape that produces a rare CI failure nobody can reproduce, and it
spends real milliseconds in a suite that otherwise runs on a test clock.

The fix is the deterministic UDP seam plus `platform::setTestNowMs`, the same pattern every other
timing test here uses: feed the packet, advance the clock explicitly, assert. Raised by CodeRabbit
on PR #96 and deliberately not taken in that pass: it is pre-existing rather than part of that
diff, and swapping a test's transport is a change that wants its own verification.

## Across modules

### Headers link the docs by page, not by host (2026-10-21, from the PR 150 review)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086721)

Seventeen headers under `src/` carry absolute `https://moonmodules.org/MoonLight/...` links, so a docs move edits firmware sources and marks every firmware stale, as PR 150's move from `how-to/` to `reference/` did. A header names the page site-relative (`reference/hardware/panel-cards.md#...`) and `gen_help.py` resolves the host once, so the next docs move touches no source file.

### Remove the tests' own clock resets, now the listener's job (2026-10-25)

Kind: Restructure · Platform: desktop · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086775)

The doctest listener in `test/unit/test_main.cpp` puts the test clock back on the real one before every test case and subcase rerun, so the 90 resets in `test/unit` (`setTestNowMs(0)` in a guard struct or at a test's end) do the same job twice. Remove them: a guard struct and its uses go, a guard that also restores the heap, the WiFi scan, the palette or the boot record keeps that half, and a bare reset in the middle of a test stays wherever the rest of that test reads the real clock. About 150 lines, proven by the full suite.

### Three decisions left open by the code ratchets (2026-10-25, from the Code ratchets plan)

Kind: Investigation · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086853)

- **Name spread's limit**, 10 files beyond a type's own (`MAX_SPREAD` in `check_code.py`), which no tool defaults: confirm or move. Recommended: confirm, since the count only ratchets down.
- **A gate runner for agentless development**: the gate lists are run by an agent today, and a local-model or agentless workflow wants one runnable command per event. `precommit.py` went on 2026-10-01; whether one returns, and in what shape.
- **Undocumented public declarations**: a generated page shows an empty entry for each, which argues for a docgen rule, while cut-first argues against comments for their own sake. The middle way counts only public declarations in headers that generate a page.

### A catalog entry extends another (2026-10-18)

Kind: Feature · Trigger: at the first two entries that share a base · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287086933)

A shield is a carrier board plus its own pins, as a Serg shield is a D1-Mini32 with more wired. An optional `extends: "<entry name>"` in the [device catalog](../../../mooninstaller/README.md#the-device-catalog) lets an entry take another's `state`: levels nest (MCU, carrier, device), a child overrides its parent at the same module and control, and modules merge by name.
**Build it** at the first two entries that share a base, as a pass in the installer and MoonDeck before they send the document, with `check_devices` resolving it too.

### The prose sweep, now counted and ratcheted (2026-09-28)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087002)

The em-dashes dominate and are the mechanical part: a comma, a colon or a full stop replaces each,
and the same sweep already ran once over `src/**/*.h` when `CComments` was added. The rest is
judgment per line. Do it as its own change with its own review, a directory at a time, rather than
inside a branch about something else: a blanket find-and-replace over comments is how a code
identifier gets rewritten by accident.

### `check_docgen` reads declarations from the clang AST (2026-10-15)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087063)

The "public member has no `///`" rule reads a declaration's shape with a regex and tracks access by counting braces. It missed 25 functions carrying `MM_NONBLOCKING` until the pattern learned that macros follow a parameter list, and its comments list the other shapes it once misread. `check_clang_query.py` and `gen_help.py` already parse every header with libclang, which knows each declaration's access, kind and attached doc comment. Moving the rule onto that parse removes the brace counting, the depth stack and the shape patterns, and it reaches `.cpp` files, which the regex cannot read.

### An example: MoonLight in a pod beside the music server (belrik, 2026-10-21)

Kind: Feature · Platform: Pi · Stage: Document and commit · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087152)

belrik runs MoonLight in a Kubernetes pod with PulseAudio, squeezelite and sendspin in containers of their own, playing 96 kHz / 24-bit files through RAOP, each part upgraded on its own.
That is audio-reactive light fed from a music server, and the reference docs do not yet show how containers share a PulseAudio server.
An example page under Reference → Examples, built from belrik's manifest, shows it; asked for on Discord.

### Retire the OTA fallback repository, once a device has another recovery route

Kind: Restructure · Stage: Release · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087252)

`FirmwareUpdateModule.h` names two: `kReleaseRepo` is tried first and `kFallbackRepo` catches a device whose firmware predates the move. It is tempting to call the second dead code after the rename, and it is not: `platform_esp32_ota.cpp:195` retries the alternate address whenever `esp_https_ota_begin` FAILS, which covers DNS, TLS, an outage and a variant whose asset is missing, rather than only a repository that has moved. So a v6.0.0 device on the happy path never reaches it and a v6.0.0 device whose first request fails still does. Removing it deletes that second chance for every board, which wants a replacement rather than a deletion: a configurable update URL, or a retry against the same address, would give a device somewhere to go. The one cost of keeping it is a wasted request on the failure path, which is already the slow path.

### macOS code-signing (Developer ID)

Kind: Feature · Platform: desktop · Stage: Release · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087327)

the release `.dmg` is now ad-hoc signed, which turns Gatekeeper's outright refusal into the "unidentified developer" prompt a user can accept via right-click Open. A paid Developer ID certificate plus notarization would drop that prompt too.

### Windows code-signing

Kind: Feature · Platform: desktop · Stage: Release · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087406)

drops the SmartScreen warning on first run of `MoonLight.exe`. Same shape as macOS signing; needs an EV / OV code-signing certificate (Microsoft Trusted Signing is the cheapest current option). Until then, the README notes the SmartScreen prompt.

### Publish SHA-256 sums with release assets

Kind: Feature · Stage: Release · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087501)

the Defender false-positive guidance can only
say "re-download over HTTPS" today; a checksums file per release lets a user verify a
quarantined installer before restoring it. One sha256sum step in the release staging.

### Installer UX polish

Kind: Feature · Stage: Install · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087561)

clear "Pre-release (beta)" warning on RC/latest picks, yank-by-asset-tag instead of yank-by-release-deletion.

### Offer MoonLight as a library

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287087985)

a downstream sketch where another firmware/app consumes the light pipeline (or a subset) as an embeddable dependency rather than running the whole binary. `library.json` is already a PlatformIO *library* manifest, so the seed exists. When this is designed, give it a small public **identity surface**: one runtime constant the consumer reads (a `kProjectName`, likely a `ProjectInfo` bundle of name + version + url) that the network wire-strings (ArtNet/E1.31 source-name + CID), the UI banner, and any "About" string all *derive from*: the one place a consumer queries "what am I embedding." This is the genuine home for the name-centralisation that the rename ([the MoonLight plan](https://github.com/MoonModules/MoonLight/issues/193)) deliberately *didn't* do: the rename is a one-time sweep (a constant would just split it), but a library consumer references the identity ongoing and widely, which is the test a constant must pass. Build it *then*, against the real library API, not speculatively now.

### GCC Release-mode truncation warnings (17, non-blocking)

Kind: Bug · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287088042)

`uv run moondeck/build/build_desktop.py --gcc` reproduces CI's compiler (CI builds **Debug**, and that is clean). Building the same tree **Release** with GCC surfaces **17 further `-Wformat-truncation` / `-Wstringop-truncation` / `-Wstringop-overflow` warnings** in `Control.cpp`, `HttpServerModule.cpp`, `JsonUtil.h`, `MqttModule.cpp` — Release inlines more, so GCC can prove more about buffer sizes and gets stricter.

They are **not** CI failures (CI is Debug) and each one inspected so far is a false positive of the same shape: a bounds check exists, but GCC cannot prove the degenerate case (e.g. `HttpServerModule.cpp:2400` warns "writing 1 byte into a region of size 0" on a line *guarded* by `if (wsLen + headerLen > sizeof(hdr)) return false;`). Still worth clearing: each is either a genuinely unprovable bound (fix the code) or a bound the code knows but does not state (make it explicit). Doing it in one pass beats letting them mask a real one later.

Not done with the multi-destination/tab-UI merge because 17 warnings across four core files is its own change, not a tail on someone else's.

**GCC below 16 needs four warnings demoted, and nothing exercises those versions.** (the demotion and its reasons are in `CMakeLists.txt`). It is a suppression, not an understanding: nobody routinely compiles with 12-15, so a REAL instance of one of these on those versions is now a warning nobody reads. Revisit when the runner's default GCC reaches 16, at which point the whole block can be deleted.

### British spellings and em-dashes predate the prose gate (319 files)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287128769)

`check_prose.py` reports on ADDED lines only, so the American-spelling rule has been enforced from
the day it landed forward, and everything written before it was never swept. 118 files still carry
`colour`, `centre`, `behaviour`, `recognise`, `initialise` and friends, in comments and in a few
identifiers.

**Em-dashes are the same story and the larger half**: 10,326 of them across 319 files under `docs/`,
against the same rule and missed for the same reason. They are worse than the spellings to sweep
mechanically, because the right replacement depends on the sentence (a colon where it explains, a
comma for an aside, a full stop between two clauses), so a blind substitution produces prose nobody
proofread. Count them per file and do the biggest offenders by hand.

A sweep is its own change ([documentation-standards](../../contributing/documentation-standards.md#writing)), with the gate run over the whole tree afterwards rather than over a diff.

Identifiers first and separately: a rename changes an API, where a comment does not. `centre()` in
the shipped `crosshair.mle` was one and is already fixed, since a shipped teaching script is the
highest-value case.

### Network round-trip test: drop and reorder measurement

Kind: Feature · Platform: desktop · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287128844)

`moondeck/scenario/run_network_roundtrip.py` measures desktop→device→desktop **latency and jitter** per protocol (ArtNet/E1.31/DDP) by timing how long a sent color takes to appear in the device's preview stream. It deliberately does **not** measure per-frame **drops or reorder**, because the path can't track individual frames cleanly: `NetworkSendDriver` re-clocks at its own fps (decoupled from receive) and `NetworkReceiveEffect` holds-last-frame, so frames don't pass through 1:1 — a sequence number embedded in frame N may be re-sent 0, 1, or several times downstream. The min/median/max spread the test already reports *is* the jitter signal (it surfaced multi-second outliers on the classic ESP32). To measure true drop/reorder, the firmware would need a sequence-faithful echo path (e.g. a `NetworkReceiveEffect` echo mode that re-emits each received frame 1:1 back to the sender, bypassing the fps re-clock), then the desktop could match sent↔received sequence numbers. The test's docstring lists this under "extend later" alongside per-frame sequence matching and the device→device chain.

### MoonDeck doc-asset endpoint hardening

Kind: Restructure · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287128928)

`moondeck/moondeck.py::_serve_doc_asset` accepts any ROOT-relative path and serves the file. Path traversal *is* blocked (`asset_path.relative_to(ROOT.resolve())`), but inside the repo any file is served — including local-only artefacts like `moondeck/build/wifi_credentials.json` if present. MoonDeck binds to all interfaces by design (the existing comment in `main()` explicitly enables LAN reach), so anyone on the LAN can hit the endpoint.

Two improvements when this matters:
- **Subdirectory whitelist** — only serve under `docs/` (and image asset paths the markdown renderer needs). Reject `moondeck/build/wifi_credentials.json` etc. with 403.
- **Extension whitelist** — only image / CSS / JS mime types via a small allowlist.
- **Optional bind-to-localhost flag** — `--bind 127.0.0.1` for users who don't want LAN reachability. Default stays "" (all interfaces) since the LAN-reach is the documented design.

Not blocking — MoonDeck is a developer tool, not a production server. Pick this up when MoonDeck is in scope for hardening.

### A tagged release does not reach the web installer until the next main deploy

Kind: Bug · Stage: Release · Process: release · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287128996)

`deploy-pages` in `.github/workflows/release.yml` is gated `if: github.ref == 'refs/heads/main'`, because the `github-pages` environment's protection rule only allows main. The installer's release list is **staged into the Pages site at deploy time** (`install.js` self-hosts the last 5 stable + 5 prerelease releases; the release-asset URLs redirect to a host that sends no CORS header, so the browser cannot read them from the Pages origin). Together those mean **pushing a `vX.Y.Z` tag publishes the release but never updates the installer** — the new version reaches the picker only when something later pushes to main.

Hit on v4.0.0 (2026-08-24): the release published at 16:15:59, a main deploy ran at 16:16 and enumerated releases *before* it existed, and the installer offered v3.0.0 as newest for hours. Re-running the workflow with the tag fixed it, and that manual re-run is the current workaround.

Note the device's own OTA picker is unaffected — it reads `api.github.com` live ([app.js](../../src/ui/app.js) `RELEASES_API`), which is why a device could offer v4.0.0 while the installer could not. Two independent paths to the same release list.

`restage-pages-for-tag` in the same workflow already covers this: on a `v*` tag it re-invokes the workflow on `main` with `tag=<the tag>`, so `deploy-pages` runs under the allowed ref and the manifests land in `releases/<tag>/`. The open item is narrower than the symptom suggested — whether the dispatched run can still lose the race, since it is a *second* run whose release enumeration happens later, and whether relaxing the `github-pages` environment's branch policy (needs repo-admin) would remove the indirection entirely.

### CI: pin GitHub Actions to commit SHAs (supply-chain hardening)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129085)

`.github/workflows/release.yml` references all 9 action types by mutable `@vN` tag (`actions/checkout@v4`, `astral-sh/setup-uv@v3`, `softprops/action-gh-release@v2`, `espressif/esp-idf-ci-action@v1`, …). A mutable tag can be force-moved to malicious code by a compromised publisher; pinning each `uses:` to a full commit SHA (with a `# vN` trailing comment) removes that vector. **Not done (this item):** SHA-pinning, because it carries an ongoing cost — pinned SHAs go stale and miss security patches, so it only pays for itself **alongside Dependabot** (or a Renovate config) to auto-bump them. Pick this up as a deliberate "CI hardening + Dependabot" pass, not piecemeal. Low risk today: every action pinned is a first-party `actions/*` or a well-known publisher (astral, espressif, softprops), not an obscure third-party action.

### An offline test run takes 16 minutes instead of one (2026-10-03)

Kind: Bug · Platform: desktop · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129170)

With the host's network down, `test_desktop` ran 16 minutes and the scenarios stalled past the gate's hour; online, no test takes over 4 seconds, so something waits out network timeouts.
Next time the machine is offline, `build/macos/test/mm_tests --duration=true` names the test, which then gets a seam like `setTestJoinFails`.

### The restructured scenarios lost four things the archive asserted (2026-09-29)

Kind: Feature · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129247)

What did not survive the move of the 27 archived scenarios to the per-module set, and nothing else in the tree covers:

**Every performance contract and bound is gone**, measured rather than estimated: `contract` 40 to 0, `bounds` 20 to 0.
Both mechanisms are still implemented in the runner (`scenario_runner.cpp` enforces a `tick_us` ceiling and a `free_heap` floor, and the `bounds.fps` gate below it).
The new set records `observed` numbers, which is a trend rather than an assertion, so nothing fails on a tick or heap regression.
One document still states otherwise and should be corrected when this is: `docs/reference/performance.md` says each scenario step carries an enforced contract.
The `device/` set carries `observed` blocks from a real ESP32-S3 as of 2026-09-29, which is the window a contract would be set from; none is set yet, since one run per step is not a baseline.

**The buffer checks run ONCE at the end of a scenario**, already filed under per-step assertions below, so a resize can still only assert that the control changed.
Nothing can say the buffer grew to 1024 lights at 32x32 and shrank back, which is the claim the scenario's own name makes.

**`HttpServerModule` has no scenario**, though it is a scheduler root on every networked target and the transport the live tier itself runs over.

What no unit test can reproduce is the cross-board comparative number set the peripheral sweep produced.

**A measurement taken after a failed allocation is recorded as though the work happened (2026-09-30).** `scenario_Fluid_solver` on a classic ESP32 reads 107 us at 64x64 where an S3 takes 71,441 us, which no float-heavy solver does: the classic is 670x faster only because its grids never allocated. The tell is in the same block, where free heap RISES from 25,760 to 53,728 bytes as the tick collapses, so memory is being handed back while the effect ticks dark. `cube-20-d` reads the same way at 97 us.
Two halves, and neither exists today. `FluidEffect::prepare` refuses a grid it cannot allocate and leaves the effect ticking harmlessly, which is the right behaviour, but it publishes no status, so nothing downstream can tell a refusal from a cheap frame. And `run_live_scenario.py` records whatever tick it measures without asking whether the module producing it is in a state worth measuring.
The fix is a status on the effect (the shape `ParallelLedDriver` already uses for a bus that will not init) plus a runner check that labels or drops the sample. It needs a classic on the bench to verify, since that is the only board small enough to fail the allocation. Until then the affected rows are the classic's `grid-64-h`, `all-jets`, `long-persistence`, `cube-20-h` and `cube-20-d`, and they understate the cost rather than overstating it, so no contract set from them would be too lax.

### The static set runs on every supported device before the rename (2026-09-29)

Kind: Feature · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129341)

The scenarios are the pre-cutover hardware check: a set that passes in-process proves the logic, and the same set against each board proves the platform layer under it.
That is the point of getting the static set solid first, and it is what the device sweep on cutover day reads from.

Two of the three things it needed are now in place: the light scenarios render, and eight scenarios are marked `live_only` so a sweep asks a board something a laptop cannot answer.
What remains is a runner that takes the device list rather than one host, so the answer is a matrix across the chips we ship rather than a pass on whichever board was plugged in.
The first S3 run on 2026-09-29 found two defects the set had been hiding: a `channelsPerLight` prop that stopped being a control, and peripheral names renamed out from under the scenarios, where `optional: true` turned every wrong name into a silent skip and still reported a pass.

`moondeck/moondeck.json` carries 24 registered devices and `run_live_scenario.py` takes a `--host`, so the missing piece is the loop and the per-device report, not the transport.

### A scenario that runs until something breaks (2026-09-29)

Kind: Feature · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129420)

A static scenario proves a sequence works once. What it cannot find is the fault that needs a thousand random module additions, or a leak that takes an hour: the runner sets up, asserts and exits, so the long tail of lifecycle bugs has no test that reaches it.
A fuzz mode would run until killed on a device left on a bench, doing randomly chosen legal things and stopping the moment one fails.

The runner already carries most of it: a step vocabulary, module add, remove and replace, control setting, measurement, and per-step failure reporting.
What it needs is a mode that generates steps rather than reading them, and four things that make a failure worth acting on.

A **seed**, so a run is reproducible: a failure reprints the seed and the step index, and the same seed replays the same sequence.
A **legal-move set** the scenario names rather than the generator guessing, since a random string in a filepath control proves nothing about the pipeline.
**Halt on the first failure**, keeping the device in its broken state for inspection rather than tidying it away.
And a **floor under heap and tick**, because the faults this is for announce themselves as a slow drift rather than an assertion.

Worth most where the gaps above are: random add and remove under a live pipeline is exactly what the archived `remove_module` steps covered, and a long run on a Raspberry Pi finds the leak a thirty-second scenario cannot.
Build it after the static set is solid, since a fuzz run is only as trustworthy as the assertions it fires.


- **Memory degradation cascade** — the output-buffer *allocation* decision (no buffer for a lone identity layer; a buffer for ≥2 layers or any LUT layer) is unit-pinned (`unit_Layers_container` "Drivers allocates the output buffer only when…"), and LUT-vs-identity is pinned by `unit_Layer_sparse_mapping`. What's **not** pinned is the *low-heap* half of [architecture.md § Degradation cascade](../../reference/hardware/firmware-variants.md#degradation-cascade): under heap pressure the LUT + driver buffer are skipped *together* (`lutSkipped()` true, forced 1:1), and below that the layer buffer *reduces dimensions* (halving to a 8×8 floor) rather than failing. The hook exists — `unit_BlendMap` already uses `platform::setTestMaxAllocBlock` to force allocation failure for the paging test — so a test could cap the block size and assert: (1) LUT+output buffer both skip and `lutSkipped()` flips, (2) the layer buffer shrinks to fit and never goes null. Pre-existing gap (predates multi-layer); the *happy-path* allocation contract is covered, only the OOM-degrade branch isn't.
- **Per-step assertions in scenarios (a framework gap, not a scenario gap).** A scenario step can
  assert TIMING and HEAP (`bounds`, `contract`) and the run asserts the final buffer, but it cannot
  say "after this step the fixture has 24 lights", "this module's status reports a compile error",
  or "the rendered output changed". So a migrated script step proves the pipeline still ticks, not
  that the edit did what it claims — the MoonLive scenarios walk break-and-recover cycles whose
  most interesting states go unasserted. Wants a small vocabulary (`expect_lights`,
  `expect_status`, maybe `expect_pixel`) added to **both** runners together: the desktop/live split
  is exactly what let `write_file` exist on one and silently no-op on the other. Raised by
  CodeRabbit against scenario_MoonLive_pipeline and scenario_MoonLiveEffect_controls; the sites are
  listed there, but the vocabulary has to exist first.
- **UI page load time** — scenario step measuring HTTP response time for `/`, `/api/state`, `/api/system` via the live runner. Verifies acceptable load time on ESP32.
- **Module teardown memory** — scenario that tears down all modules and verifies heap returns to pre-setup baseline. Confirms no lifecycle leaks.
- **`syncVisibleControls` has no test in `test/js`.** `app.js`'s conditional-control DOM logic (`syncVisibleControls` reconciles which control rows are rendered when a `hidden` flag flips) was the source of a real re-render-loop freeze (Network static-IP toggle) caught only on hardware. A `jsdom` test that builds a card, flips a control's `hidden`, runs the reconcile, and asserts the right rows appear/disappear (and that it converges: the unchanged→no-op fast path) would have caught it. It needs an `app.js` seam first: extracting `reconcileControlRows` into a served file, wired through `embed_ui.cmake` + the two HttpServerModule routes like the other UI .js.
- **Browser-level Improv automation** (deferred) — `moondeck/build/improv_smoke_test.py` (added 2026-06-03) exercises the device-side Improv listener over plain serial; what's missing is the browser-side equivalent — Playwright driving Chrome's Web Serial, clicking through ESP Web Tools' install modal, filling the WiFi creds form, asserting `PROVISIONED`. Catches "ESP Web Tools changed its Improv handling in a way that broke our manifest format" failures the serial-only smoke test can't see. Hard to set up reliably (headless Chrome with Web Serial is finicky, needs a wired ESP32 in CI). Pick this up if a regression in the browser flow ever escapes the manual dev-environment test (preview_installer flash-ready mode at <http://localhost:8000/>).

### Live full-suite run leaks state between scenarios

Kind: Bug · Stage: Test · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129508)

`run_live_scenario.py --module all` runs scenarios in sequence against one device, and they share the live tree. Two scenario styles don't compose:
- **Canvas-preparing scenarios** (`scenario_perf_light`, `scenario_perf_full`) `clear_children` the containers and rebuild, then their cleanup leaves the tree **bare**.
- **Canonical-tree-assuming scenarios** (`scenario_GridLayout_resize`) are `mutate` scenarios that expect the boot tree (Grid / Noise / Multiply) to already exist and only tweak it.

Run a bare-leaving scenario before a tree-assuming one and the latter fails pre-flight ("references ids neither on the device nor added by an earlier step"). Each passes **in-process** (fresh tree per scenario — the authoritative gate) and **live individually** (after a clean boot); only the chained live run trips. Not a product bug — a consequence of the "scenarios own their state, no restore" model the canvas-preparing scenarios follow, which the older ones predate.

Fix options: (a) make every live mutate scenario clear+rebuild its own canvas (consistent with the newer ones) so order never matters; or (b) have the live runner reboot / restore the canonical tree between scenarios. (a) is the cleaner long-term shape. Until then, the in-process suite is the gate; live full-suite runs need a clean boot per scenario, or run scenarios individually.

### Promote the one-line // cap to an error, once the 604 .cpp blocks are homed (2026-09-21)

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129590)

`MAX_CODE_COMMENT = 1` is already the rule in both kinds of file, and already an ERROR in a header. It stays a warning in a `.cpp` only because `_blocks()` stages the sweep by file kind, and that function names its own removal: a rule leaves the staging as the tree meets it, which no-hard-wrap already did.

**The shape of what is left.** 604 blocks across 92 files, 2081 comment lines. By area: 239 in `test/`, 210 in `src/`, 155 under `src/platform/`. By length: 81 are two lines, 321 are three, 124 are four, so 526 of 604 are three lines or fewer and the tail is one 15-line block, two of ten and a handful between.

It concentrates, which is what makes it approachable. `HttpServerModule.cpp` alone holds 100 and the top ten files hold 312, over half; the other 82 files average under four each.

**Where the depth goes is the blocker.** Of the 92 files only 19 already carry an `@technical` appendix. 68 have a `///` lead with no appendix and 5 have neither, so most files need the appendix written before a single block can move.

**The fix is not to split them.** Chopping one comment into two shorter lines leaves the text identical and satisfies nothing, which is why the cap counts a block rather than a line. Each one is depth that belongs in the file lead's appendix with an `@xref` back, or prose that says less.

**Suggested order.** `HttpServerModule.cpp` first: 100 blocks, 444 comment lines, and it already has the appendix. Then the other nine heavy files for 212 more. Then `test/` as its own sweep, because the argument that a test comment explains why a test EXISTS is strongest there and may deserve a different budget rather than an appendix. Promote once at zero.

**Cost.** Not a scripted pass. Each block is a judgement between depth that belongs in an appendix, an explanation that should be shorter, and history git already holds. About 35 done by hand in one session is the measured rate, so the first file is a session of its own and the whole sweep is several days.

### ESP32 clang/LLVM toolchain: extend the clang checks to src/platform/esp32/

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129673)

Espressif ships an xtensa LLVM (their fork), but the installed `esp-clangd` package contains
**only `clangd`** — no `clang++` driver — so a clang analysis pass over ESP32 sources needs their
full LLVM installed separately.

What it would buy: `-Wfunction-effects` (and clang-tidy, clang-query) over `src/platform/esp32/`
— 20 translation units, the only code the desktop build excludes. Everything else, including the
LED drivers, already compiles on desktop and is already checked.

What it costs: a second ~1 GB toolchain, maintained purely for analysis — the firmware would
still be built by GCC, so the analysing compiler is not the shipping compiler. That is a real
"one rule, one owner" tension, and the reason this is a decision rather than an obvious yes.

Worth revisiting when either the coverage gap bites (a hot-path bug traced to the platform layer
that the desktop check could not see) or Espressif's LLVM becomes the default toolchain.

### clang-tidy: triage the remaining 30 findings

Kind: Restructure · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129754)

`.clang-tidy` runs `*` minus a documented disable list and reaches zero on everything except the
path-sensitive `clang-analyzer-*` family: **30 findings**, led by
`bugprone-unchecked-string-to-number-conversion` (9), `security.insecureAPI.strcpy` (5),
`core.NonNullParamChecker` (4), `deadcode.DeadStores` (3) and `unix.cstring.NullArg` (3). Roughly
half sit in test code (`strcpy` into fixed local buffers, a deliberate divide-by-zero probe).

The nine `unchecked-string-to-number-conversion` sites were read individually and are all safe:
HueDriver guards on `if (id > 0)`, JsonUtil documents 0-means-absent, MqttModule uses a `v = -1`
sentinel and clamps. They stay reported rather than suppressed — a report shows the real situation,
and a `NOLINT` would hide a genuine class of bug for the next person who writes one of these.

### Pin the preview cancel over loopback (test)

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129830)

`cancelBufferedSend` closes a preview client left mid-message, so the next frame is never read as the rest of this one, and nothing pins it. The loopback `WsClient` in `unit_HttpServerModule_subscribe.cpp` already drives `/wsp` and reads a frame whole; a client that stops reading mid-frame, then a cancel, then a check that its socket closed while a client that read the whole frame stays open, covers it.

### MoonDeck scripts crash on Windows whenever their output is redirected (2026-08-22)

Kind: Bug · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287129942)

62 of the ~64 scripts print `→ ✓ ⚠ —` or box-drawing characters. A REDIRECTED Windows stdout takes `locale.getpreferredencoding()`, cp1252, and the first such character raises UnicodeEncodeError, *after* the real work has succeeded: `collect_kpi.py` measures everything, writes the metrics, then dies printing the summary arrow. An ATTACHED console has been UTF-8 since Python 3.6 (PEP 528), measured on this bench: `utf-8 isatty=True` from a console against `cp1252` through a pipe. So the entry's original framing was backwards. A human at a Windows terminal is the one case that works, and every redirected path fails: a pipe, a tee, a CI log, an agent's captured output. The gate runner that handed children `PYTHONIOENCODING=utf-8` is gone, which is why this surfaces now.

The READ side breaks too, and that half is worse because `PYTHONIOENCODING` does not mask it: `Path.read_text()` and `open()` without an encoding decode as cp1252. A sweep found 43 unqualified `read_text`/`write_text` plus 47 bare `open()` across `moondeck/`.

One Windows day (2026-09-29) hit it FOUR times in four different files, each fixed separately at its own call site: `_test_metadata.py` stopped the test runner starting, `test_desktop.py` would have killed a run on one non-ASCII byte from the test binary, `run_live_scenario.py` printed an arrow per step and so recorded a scenario the device PASSED as FAILED, and `preview_installer.py` died writing a staged page, leaving the web installer preview unusable on Windows. Four site fixes in one day is the argument for one home, made by the defect itself.

The preamble is now copied into five scripts (`setup_esp_idf.py`, `ci/verify_version.py`, `docs/build_docs.py`, `scenario/run_live_scenario.py`, `scenario/run_scenario.py`), each carrying its own comment explaining the same fact. Per-script `sys.stdout.reconfigure()` is the wrong shape at 62 files: every new script would have to remember, and the one that forgets fails in the field. It wants ONE home, and the two candidates do not cover the same ground. `PYTHONUTF8=1` in whatever env MoonDeck's front ends already establish settles BOTH halves, because UTF-8 mode changes the default for `open()` and `Path.read_text()` as well as for stdio. A shared `moondeck/_stdio.py` imported by the entry points settles only the stdio half, since `reconfigure()` touches the standard streams and nothing else, so it leaves every unqualified `read_text`/`open` exactly as broken and those sites still need `encoding="utf-8"` written out. Pick when someone next runs a check by hand and it dies on a tick mark.

**`run_scenario.py` was the fifth site, fixed 2026-09-30 ahead of a Windows run.** Its sibling `run_live_scenario.py` got the preamble on the Windows day; this one did not, and it relays the output of `mm_scenarios`, which prints an arrow and an em-dash of its own (`test/scenario_runner.cpp` carries 29 non-ASCII characters, several inside `printf`). Nothing had caught it because the in-process tier had only ever run on macOS, where an attached console and a pipe are both UTF-8. Left alone it would have read as a scenario failure rather than an encoding one, which is the misdiagnosis the live runner's fix was written to prevent. Five sites in two days is the count the one-home argument above should now be weighed against.

### A Windows scenario run recorded tick averages 250x its own repeat, cause unknown (2026-09-29)

Kind: Investigation · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130012)

A Windows run of `scenario_Layouts_resize_reallocates_live` recorded 245 ms, 564 ms and 458 ms for its three measure steps, against 41 to 89 us for the same steps on macOS. `tickTimeUs` is a one-second average (`src/core/module/Scheduler.cpp:127`), so those figures describe a second containing roughly one tick, which reads as a blocking call rather than slow rendering. The blocks were reverted rather than committed, since an observation filed at 245 ms teaches a future reader that Windows costs that.

An immediate repeat on the same machine and binary did not reproduce it. The three steps came back at 913 us, 1097 us and 768 us, and the tick log showed `PanelCard:0us`, `Network:1us` and `Drivers:30us` throughout, with the tick dominated by `MoonLive:3000us`. Nothing was blocking during the repeat, so the first reading stands unexplained. The candidate that fits the shape is `ethBindRawInterface()` calling `pcap_findalldevs` (`src/platform/desktop/platform_desktop.cpp:1299`), measured separately at about 2.3 seconds on Windows against milliseconds on macOS and reached through driver `prepare()` on a resize, but the repeat neither confirms nor kills it: no raw interface was bound on either run.

The enumeration cost is real and independently measured, so caching it or keeping it off any path a resize can reach earns its place on its own.

**The second half of this is already answered, from the other side of the same day (2026-09-29).** The ask was for a max-tick beside the average, to tell one blocked tick from a uniformly slow second. The observation block records `min`, `max`, `p50`, `p95`, `n` and the raw `samples` per target, so a repeat of this run reports its own worst frame rather than only its mean: a single 245 ms tick inside a second of good ones now shows as a `max` far above the `p50`, which is the distinction that was missing.

**Re-measure before reading anything into the old figures.** `scenario_Layouts_resize_reallocates_live` was rewritten the same day (59 lines changed): it now builds a full pipeline and asserts with `expect_control` where it previously only measured, so the Windows numbers above describe a scenario that no longer exists in that form. Whatever a Windows run reports next is the figure to reason from, and it will carry the spread this entry wanted.

### build_esp32.py reports success without building, under Git Bash (2026-09-28)

Kind: Bug · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130100)

Run from Git Bash it prints `MSys/Mingw is no longer supported`, stops after `Setting target to esp32s3...`, and **exits 0** ten seconds later with no binary. Run from PowerShell the same command builds correctly. A green exit code for a build that never ran is the worst shape a failure can take, and on a runner it would publish a stale binary while every gate stayed green. Either detect the MSYS environment and fail loudly, or re-exec through the shell the IDF tools expect.

### build_desktop.py cannot relink while the app is running (2026-09-28)

Kind: Bug · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130177)

Windows holds an executable open while it runs, so a rebuild fails with `LNK1104: cannot open file ... MoonLight.exe` where POSIX would replace it. `run_desktop.py` already stops a running instance before launching; `build_desktop.py` could reuse that and remove a papercut that costs a confusing error every time someone edits code with the app open.

### disasm.py cannot run on Windows (2026-08-21)

Kind: Bug · Platform: desktop · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130264)

The tool shells out to `c++` and `objdump` to build the per-ISA emitter and disassemble its bytes, and a Windows machine carrying only MSVC has neither. That holds for every ISA, not just the new `x86_64` one — but x86-64 is where it bites, because the host it disassembles is now a Windows desktop and the tool is what turns "the script did nothing" into an answer. The x86-64 backend was brought up without it, using byte-pinning tests plus WinDbg on the emitted image; that worked, and was slower than reading the instructions would have been.

Closing it is a compiler/disassembler pair behind the two `subprocess.run` calls: `cl.exe` for the build, and for the disassembly either LLVM's `llvm-objdump` (which understands `-b binary` the way the tool already expects, and ships with the VS "C++ Clang tools" component) or a `.obj` wrapper around `dumpbin /disasm`, which does not. Prefer the former — the flags are already right, so it is a lookup rather than a second code path.

### A preview GIF for every MoonLive script (2026-10-19)

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130339)

The 140 compiled effects each show a GIF on their catalog card (`docs/assets/light/effects/<Type>.gif`, 25 MB in all); the 49 MoonLive effect scripts show none.
The pictures belong in `docs/assets/moonlive/effects/<subfolder>/<name>.gif`, mirroring the script subfolders, never in `moonlive/` itself, which the build globs and the UI fetches file by file.
One MoonDeck script makes them all: `screenshot_modules.py` learns to step through the MoonLive catalog, capturing from the preview socket the way the hpwit GIFs of 2026-10-18 were made.
At 0.5 to 1 MB each that is 25 to 50 MB more, so a smaller format (128 px, 2 s) is part of the decision.
**Decide first** where they show: a MoonLive scripts page with a card per script grouped by subfolder, or the script picker previewing one before its download.

### sigrok/fx2lafw cross-check + MoonDeck LED driver test script

Kind: Feature · [Card](https://github.com/users/MoonModules/projects/5?pane=issue&itemId=287130440)

- **sigrok/fx2lafw cross-check + MoonDeck "LED driver test" Python script** — the independent-clock proof and the run-from-MoonDeck flow ([the LED-driver analysis](../../design/leddriver.md)). The on-board RMT-RX loopback (shipped) is the cheap CI correctness gate but a *compromised witness* for WiFi-induced flicker — the RX capture runs on the same ESP32 whose WiFi causes the glitch. The real flicker test is a **sustained capture (seconds) with WiFi associated + a packet flood**, decoding every frame for a byte-slip or reset-gap deviation; it validates the SHIPPED render↔encode split's WiFi isolation (drivers tick on core 1; WiFi lives on core 0). A DSLogic Plus (100 MS/s) upgrade is reactive — only if a flicker reproduces that 24 MS/s can't resolve.
