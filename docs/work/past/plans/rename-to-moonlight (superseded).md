# Rename MoonLight → MoonLight (phased)

> **Superseded on 2026-09-22** by [Plan-20260922 - MoonLight, from v5.0.0 to the rename](../../present/Plan-20260922%20-%20MoonLight,%20from%20v5.0.0%20to%20the%20rename.md), which consolidates the five MoonLight files into one. Kept for the reasoning behind decisions already taken.

The project will be renamed **MoonLight → MoonLight**, and two repos swap names at the same time:

| Repo today | Becomes | What it is |
|---|---|---|
| `github.com/MoonModules/projectMM` | `github.com/ewowi/MoonLight` | the **predecessor** MoonLight (this project's prior art) moves to a personal repo |
| `github.com/MoonModules/projectMM` | `github.com/MoonModules/projectMM` | **this project** takes over the `MoonModules/projectMM` name |

The hard part is the **name collision**: while the move is in progress, `MoonModules/projectMM` means *two* different things, and the same word "MoonLight" appears in this repo as both (a) the future product name and (b) prior-art prose/links pointing at the predecessor. Sequencing exists to make that collision a non-event.

## Blast radius (measured)

- **`MoonLight` → `MoonLight`:** ~579 references across ~135 files. Concentrated in `docs/` (history/plans, moonmodules specs, install), then `src/` (core, light/effects, ui), `moondeck/build`, and `test/`.
- **Predecessor links to repoint** (`github.com/MoonModules/projectMM` → `github.com/ewowi/MoonLight`): ~23 URLs, all in `docs/` prior-art / history sections.
- **`MoonLight` as prior-art prose** (NOT the new product name): ~25 mentions in `docs/history` + light specs — these must NOT be swept into the rename; they describe the predecessor.
- **`MoonLive`** (the on-device scripting engine, 29 files): a *different* name, NOT part of this rename. Any sweep must exclude it.

### High-stakes, externally-visible references (these gate the switch)

These are the ones that break running devices, OTA, or the installer if mistimed — they change **at the switch**, not before:

- **Binary name** `MoonLight.bin` (`library.json`, `moondeck/build/flash_esp32.py`, `generate_manifest.py`, `package_desktop.py`) — renaming changes OTA asset names and the mooninstaller manifest; old + new must line up with the release that ships under the new repo.
- **OTA download URLs** `github.com/MoonModules/projectMM/releases/...` (`docs/install/install.js`, `FirmwareUpdateModule`) — deployed devices fetch updates from here; the URL only resolves to the new repo after the repo rename.
- **mDNS / device identity** the `MM-XXXX` hostname prefix (`SystemModule.h`) — devices on the LAN announce as `MM-….local`. Changing the prefix (e.g. to `ML-`) renames every device's network identity; deliberately deferred (own decision: keep `MM-` or move to `ML-`).
- **Repo URL** in docs/READMEs/`package_desktop.py` source links.

## The predecessor move: temporary fork, then transfer at the switch

The predecessor (`MoonModules/projectMM`) has to **vacate** its name so this project can take it. The mechanism matters:

- A **fork** does *not* free the name — the original stays put. But a fork *does* give our to-be-changed links a **valid target right now**.
- A **transfer** frees the name and redirects old URLs — but transferring *now* would invalidate every public reference to `MoonModules/projectMM` before this project is ready to receive the name.

So the plan uses the fork as **scaffolding**, deferring the real transfer to the switch:

1. **Now:** fork `MoonModules/projectMM` → `ewowi/MoonLight` (a temporary copy). Our ~23 deep permalinks (`/blob/main/src/MoonLight/Layers/PhysicalLayer.h`, etc.) resolve against the fork's identical tree.
2. **Now:** repoint our links `MoonModules/projectMM` → `ewowi/MoonLight`. Only the owner segment changes; the in-repo `src/MoonLight/...` path is the predecessor's own structure and stays. Links are valid immediately against the fork.
3. **During the switch:** **delete the fork, then transfer** the canonical `MoonModules/projectMM` → `ewowi/MoonLight`. GitHub refuses a transfer onto an existing repo, so delete-first is required (and correct). The canonical repo lands at the address our links already point to; GitHub's redirect covers the brief 404 window between delete and transfer.

**Watch-items for this approach:**
- **Permalink rot is a non-risk here** — the fork is scaffolding we don't develop on, so its `main` stays put and the `/blob/main/` links hold until the switch. (SHA-pinning would only get undone when the canonical repo lands, so it's not worth doing for a temporary fork. If the fork's `main` ever *does* move a cited file before the switch, the worst case is a prior-art link one commit off — self-corrects at the switch.)
- **The delete→transfer window** briefly 404s `ewowi/MoonLight` — but you own both repos, so it's two back-to-back actions (seconds apart), not a coordinated handoff. Do them in immediate succession and the exposure is negligible.

## Guiding rules

- **GitHub auto-redirects help, but aren't forever.** A renamed/transferred repo serves redirects from the old path, so old release URLs and clones keep working for a while — but a *new* repo can later claim the freed `MoonModules/projectMM` name and break them. Treat redirects as a grace window, not a permanent crutch: repoint links during the rename, don't rely on them after.
- **One mechanical sweep, reviewed, not 135 hand-edits.** The bulk `MoonLight → MoonLight` change is a scripted find-replace with an explicit exclude list (`MoonLive`, predecessor-MoonLight prose, the `MM-` mDNS prefix until decided), landed as its own commit so the diff is auditable and the gates run clean on it.
- **Predecessor links repoint independently.** The `MoonModules/projectMM → ewowi/MoonLight` URL fix is a *separate* small sweep from the `projectMM → MoonLight` rename — different intent, different commit, so neither hides the other.

---

## Phase 1 — what we can do NOW (no external dependency, no collision)

Decoupling and groundwork that's safe while both repos still hold their current names.

1. **Fork the predecessor, then repoint our links** (see *The predecessor move* above). ✅ **Done:** the fork exists at [`ewowi/MoonLight`](https://github.com/ewowi/MoonLight), and the ~22 prior-art permalinks in `docs/` (everything except this plan's own descriptive references) now point at `ewowi/MoonLight`. This clears the name collision so the later `projectMM → MoonLight` sweep can't confuse the two. The deep permalinks stay on `/blob/main/`; since the fork is scaffolding nobody develops on, `main` won't move and the links hold until the switch — no SHA-pinning needed (it would only be undone at the switch).
2. **Decide the open naming questions** — ✅ **decided: change nothing now; every user-facing identifier stays `MoonLight`/`MM-` until the switch, then flips in one sweep.** These are externally-visible (devices, OTA assets, installer manifest), so changing them early would (a) break the "tell MoonLight apart from predecessor-MoonLight on the same bench" distinction during the transition, and (b) desync the names from the repo before the cutover. The decisions:
   - **mDNS hostname prefix:** keep `MM-` until the switch — deliberately, to distinguish MoonLight devices from current MoonLight (`ML-`) boards on the same network during the transition. (Whether the post-switch prefix becomes `ML-` is a switch-time decision.)
   - **Binary/firmware basename:** keep `MoonLight.bin` until the switch (cascades to OTA asset names + installer manifest, which must line up with the first release under the new repo).
   - **PlatformIO/`library.json` `name`:** keep `MoonLight` until the switch.
   - **npm/pip/package identifiers:** none exist (no `package.json`, no `pyproject.toml`/`setup.py`) — nothing to rename.
3. **Centralise the name** — ✅ **investigated, decided: skip *for the rename*; the sweep (step 4) covers it. The constant belongs to the library direction, not here (see note).** Centralising *to shrink the switch* doesn't pay: the centralisable runtime-identity is tiny (3 C++ network wire-strings — ArtNet/E1.31 source-name + CID — plus the boot printf), those are fixed-length protocol fields whose `memcpy(…, 9)`-style copies need rewriting *regardless*, and a constant added for a one-time event sits unused afterward — the single-purpose abstraction *Default to subtraction* / *Core grows slower than the domain* say not to add. The mechanical sweep (step 4) flips every literal — wire strings, the golden-vector tests that assert them, CMake target, UI HTML, repo URL — in **one auditable commit** where the assertion and the wire bytes move in lockstep (better than a constant, which would split them). So no constant; the sweep is the centralisation. *(Step-2 decisions stand: no value changes until the switch.)*

   > **Could we reuse `library.json`'s `name` now where a literal sits (subtraction, not a new constant)?** Surveyed `moondeck/` for it — verdict: **no genuine low-hanging fruit.** ~95% of `MoonLight` literals there are the **binary name** (`build/…/MoonLight`, `.bin`, `.exe`, `.log`, `pkill MoonLight`, crash `.ips`) which must track the **CMake target**, not `library.json` (wiring them to the product name would break the path to the file on disk); plus one **wire literal** (`_net_probe.py` ArtNet source-name, must byte-match the device) and ~15 prose/docstrings. The only product-name candidates — `generate_manifest.py`'s manifest `name`/`home_assistant_domain` — must *stay* `MoonLight` today (Step 2), don't currently read `library.json`, and flip alongside `library.json` in the sweep anyway, so wiring them is new plumbing for zero present benefit. The principle (reuse an existing source of truth over a hardcoded literal) is right; it just has no payoff here because the literals are either binary-coupled or static-until-the-switch. (The real home for product-identity reuse is still the library API — see the box above.)
   >
   > **The constant has a real future home: MoonLight/MoonLight as a library.** When the project is offered as an embeddable library, a consumer will want one runtime identity to read (an "About"/banner string, the protocol source-name they can query) — *that* is the ongoing, widely-referenced use a `kProjectName` constant genuinely earns (the test the rename failed). But build it **then**, against a real library API surface (it may want to be a small `ProjectInfo` — name + version + url — not a bare string) — not speculatively now. Tracked as a seed in [backlog-core](../../future/backlog-core.md); when the library work starts, introduce the identity constant as part of its public API and let the wire-strings + UI derive from it.
4. **Author the mechanical sweep script** — ✅ **Done:** [`moondeck/rename/rename_to_moonlight.py`](../../moondeck/rename/rename_to_moonlight.py), dry-run by default (`--apply` writes; reserved for switch-day Phase 3.3, *after* the repo rename). What the dry-run against today's tree established: replaces two tokens (`ProjectMM` the enum, then `MoonLight`) — a plain token swap is correct for *every* form (repo URL, host path, `MoonLight.bin`, product name, `deviceName` slug) since `MoonLight` is never a substring of another token; file list comes from `git ls-files` so build output (`build/`, `esp32/build/`) is excluded without a brittle blocklist; `docs/history` (era record) + the rename doc itself are content-excluded. Verified: **542 hits across 113 files**, and `MoonLive` / predecessor `MoonLight` / `namespace mm` are provably never touched (0 files where their count changes). The enum rename is safe — device classification keys on the `"modules"` marker, not the label string. The script de-risks switch-day; it is NOT run with `--apply` until then.
5. **Prep MoonDeck / `moondeck.json` / bench registry** — ✅ **investigated; nothing to change now, two things flagged for switch-day.** (a) **The functional chain stays `MoonLight` until the switch (and flips together in the sweep):** `moondeck_config.json`'s `process_name: "MoonLight"` ↔ the CMake binary `MoonLight` ↔ the `build/<host>/MoonLight` run/log path ↔ `pkill MoonLight`. These are tracked files the sweep rewrites in one pass, so they stay consistent — changing `process_name` early would break MoonDeck's process detection against today's binary, so don't. (b) **The sweep cannot reach the gitignored bench registry** `moondeck/moondeck.json` (it's private, per [[bench-setup]]; the sweep uses `git ls-files`). Its `"board": "MoonLight testbench …"` values reference catalog `name`s that *do* flip — so after the switch they'd mismatch only on your bench. **Switch-day local-tooling note: hand-update `moondeck/moondeck.json` board names** (and re-provision bench devices if you want the new mDNS identity) — the sweep covers tracked files only. The MoonDeck prose (`MoonDeck.md`, code comments) flips in the normal sweep.

## Phase 2 — the COMING TIME (staged, still pre-switch)

Work that narrows the switch to a near-mechanical flip.

1. **Dry-run the sweep on a throwaway branch**, run the full gate set on it (build all ESP32 variants, ctest, scenarios, check_devices, check_specs), and fix whatever the sweep gets wrong (false hits on `MoonLive`, predecessor prose, doc anchors, the fixed-length wire-protocol fields + their golden-vector tests). Throw the branch away — the point is to harden the script, not to merge early. (Per Phase 1.3, the sweep *is* the centralisation — there's no separate constant-introduction step.)
2. **Predecessor move is a single-owner action — no coordination needed.** The same account (ewowi) owns both `MoonModules/projectMM` (source) and `ewowi/MoonLight` (destination), so the delete-fork-then-transfer at the switch is unilateral and instant — no waiting on another party, no "confirm the account is ready." The only pre-switch task is **keeping the temporary fork current** (or freezing the predecessor's `main`) so our deep links don't rot until then — and since you don't develop on the fork, that's automatic.
3. **Stage the OTA story**: cut at least one release under the *current* name so there's a known-good baseline, and decide how in-field devices migrate their update URL (rely on GitHub's redirect for the grace window; ship a firmware whose `FirmwareUpdateModule` points at the new repo so the *next* update onward is self-hosted on the new name).
4. **Draft the user-facing comms** (README banner, release note, installer copy) so the rename is announced, not discovered.

## Phase 3 — DURING the switch (the cutover, ideally one short window)

Ordered so the collision never materialises. Each step is small; the sequence is the product.

1. **Predecessor vacates the name:** **delete the temporary `ewowi/MoonLight` fork**, then **transfer** the canonical `MoonModules/projectMM` → `ewowi/MoonLight` (delete-first is required — GitHub won't transfer onto an existing repo). The canonical repo now sits where our links already point; `MoonModules/projectMM` is free.
2. **This repo takes the name:** `MoonModules/projectMM` → `MoonModules/projectMM`. GitHub redirects `MoonLight` URLs for the grace window.
3. **Run the mechanical sweep** (`MoonLight → MoonLight`, the hardened script from Phase 2) on a branch off the just-renamed repo — one auditable commit. Run the **full gate set** (every ESP32 variant, ctest, scenarios, check_devices, check_specs, host tests). This is where the binary name, OTA URLs, repo URLs, and `library.json` flip to MoonLight.
4. **Apply the deferred identity changes** decided in Phase 1.2 (mDNS prefix, binary basename) in the *same* sweep commit if chosen, so device identity and asset names change once, together.
5. **Cut the first MoonLight release** under the new repo: tag, build all firmwares, publish assets under the new names, regenerate the installer manifest. Verify the web installer flashes from the new URLs and a device OTA-updates from the new repo.
6. **Bench-verify on real hardware** (S31 + S3 + P4 at minimum): flash from the new installer, confirm mDNS announces the new identity, confirm OTA pulls the new release.
7. **Flip the user-facing comms** (README, release notes, installer copy) and announce.

### Post-switch cleanup
- Sweep for any `MoonLight` the script missed (grep should return zero outside `docs/history`, which legitimately records the old name).
- Leave a redirect note / tombstone where useful; don't *rely* on GitHub's redirect long-term (a future repo could reclaim `MoonModules/projectMM`).
- Update this backlog item's outcome and, once the lesson is absorbed, delete it (per *Mandatory subtraction*).

## Feature gaps to close before the switch (MoSCoW)

Taking the **MoonLight** name sets an expectation: someone arriving from the predecessor (60+ effects, 11 driver types, memory-optimised mapping) should not find the new MoonLight a *downgrade*. Today MoonLight has ~20 effects, 4 LED-driver types, 4 layouts, 6 modifiers. The gap is real; the question is which parts must close *before* the rename so the name isn't oversold, vs. which can land after under the new name.

This is parity-to-take-the-name, not parity-for-parity's-sake — MoonLight's architecture (live reconfiguration, robustness, the generic module/UI) is already ahead in places the count doesn't show. Prioritise what a predecessor user would *miss*, not raw feature count.

**Live scripting is not a gap — [MoonLive](../../../explanation/architecture/moonlive.md) overrules it.** The predecessor's on-device scripting was an *interpreter* lineage; MoonLive is a **native-codegen compiler** (source → typed IR → real machine code, called by function pointer at near-100% native speed in the hot path) — the architecture's named *standout*. So live scripting is a MoonLight **advantage to lead with**, not a parity item to close; it is deliberately absent from the MoSCoW below.

These are pointers to existing backlog items; the rename doesn't create new work so much as set a **bar** for which items gate it. Each links to its detailed entry rather than restating it.

### Must — the rename is a downgrade without these
- **Effect breadth at a credible fraction of 60+** — not all 60, but enough that the library doesn't feel thin. Today's ~20 cover the common families (noise, fire, plasma, particles, audio); a Must is closing the obvious *category* gaps a predecessor user expects (see Should), not matching the count. (MoonLive softens even this: a user can *author* a missing effect on-device rather than wait for a built-in.) **This gate is executed by the staged MoonLight migration** — its plan ([`Plan-20260630 - MoonLight migration (multi-stage)`](Plan-20260630%20-%20MoonLight%20migration%20(multi-stage,%20superseded).md)) brings the predecessor's effects / modifiers / layouts across in batches on a shared palette + primitive foundation; the rename's bar is "enough batches landed to not feel thin," not "all stages done."
- **Mapping / layout parity for real fixtures** — the predecessor's "memory-optimised mapping" across non-trivial fixtures (matrices, rings, cubes, custom). MoonLight has Grid/Sphere/Wheel + modifiers; a Must is that a user's existing physical layout from the predecessor has a path here.
- **OTA continuity for in-field devices** (also in Phase 2/3) — a predecessor user's deployed devices must keep updating across the rename, not brick on a dead URL.

### Should — expected, but can trail slightly under the new name
- **More LED driver types toward the 11** — MoonLight has RMT, LCD, Parlio, NetworkSend. Gaps a predecessor user may rely on: 16-lane I2S parallel (classic ESP32), shift-register expanders, additional protocols. ([backlog-light](../../future/backlog-light.md))
- **Moving-head / DMX fixture model** ([backlog-light § Fixture model](../../future/backlog-light.md)) — if predecessor users drive moving heads, this is a felt gap; long-term there, so likely Should/Could.
- **E1.31 multicast receive, async ArtNet** ([backlog-core](../../future/backlog-core.md)) — network-output completeness a show operator expects.
- **Audio-reactive follow-ups** ([backlog-light § Audio-reactive](../../future/backlog-light.md)) — MoonLight has the audio pipeline; closing the effect/feature follow-ups keeps audio parity.

### Could — nice for the launch, not blocking
- **z-axis variation in 2D effects**, **full-density interpolated preview**, **RGBW preview end-to-end** ([backlog-light](../../future/backlog-light.md)) — polish that makes the new MoonLight feel finished.
- **Runtime board presets**, **per-layout coordinate offset** ([backlog-core](../../future/backlog-core.md)) — usability wins, independent of parity.
- **Sensor input breadth** (IMU/line-in beyond the mic) — extends the platform, not core to the predecessor's identity.

### Won't (this rename) — explicitly out of scope for the switch
- **100% effect-count parity** — chasing all 60+ before the rename would block it indefinitely; close categories, not the literal count.
- **Raspberry Pi 5 sensor input**, **fixture model for beams** ([backlog](../../future/backlog-light.md)) — post-1.0, land under the new name.
- **Renaming MoonLive** or any non-`MoonLight` identifier — out of scope (see blast radius).

**The gating question for the product owner:** which of the Musts must be *shipped* vs. *credibly announced as in-progress* at switch time? With live scripting off the list (MoonLive overrules it — and is a *lead* feature, not a gap), the remaining Musts are lighter: effect breadth and mapping/layout parity are incremental, and MoonLive's author-it-yourself path further softens the effect gap. The likely real gate is just "enough effects + a clean migration path for existing layouts" — a much shorter pole than the predecessor's headline capability would have implied.

## Risks / watch-items

- **`MoonLive` false positives** — the sweep must exclude it (substring of neither, but adjacent in prose); verify the exclude list catches `MoonLiveEffect`, `MoonLive.cpp`, etc.
- **Predecessor-prose false positives** — `docs/history` + light specs cite "MoonLight" as prior art; those stay pointing at `ewowi/MoonLight` and must not become self-references.
- **In-field OTA continuity** — the window between the repo rename and the first new-name release is covered by GitHub redirects; a firmware already in the field keeps updating only as long as that holds. Shipping a redirect-aware `FirmwareUpdateModule` before the switch shortens the exposure.
- **`docs/history` is exempt** — it records the MoonLight era by name and should *keep* saying MoonLight where it's describing that history (present-tense rule's history exception). The sweep must not rewrite history entries into a false "always was MoonLight" narrative.
