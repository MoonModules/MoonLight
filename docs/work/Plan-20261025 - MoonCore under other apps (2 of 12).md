# Plan: MoonCore under other apps

Plan of 2026-10-25, a Restructure, for approval. Another repository takes MoonLight as a dependency, links MoonCore alone and builds its own app on it: its own modules, its own `main.cpp`, no light domain. MoonLight becomes the first app on MoonCore rather than the only one, and [FastLED-MM](https://github.com/MoonModules/FastLED-MM) the second, the consumer that proves it. Beyond this plan, the same core is an alternative to ESPHome and Tasmota, and a Home Assistant front end.

## What exists

- **Core compiles without light** ([Code ratchets](https://github.com/MoonModules/MoonLight/issues/197), step 12): `mm_core` compiles through an include root without `light/`, every core header on its own, and `relative include path` keeps `..` from reaching past it.
- **Core links without light.** On the desktop, `libmm_core.a` and `libmm_platform.a` need no `mm::` symbol outside themselves. The app supplies `main.cpp` and the type registry (`module_types.cpp`), both outside core.
- **The root CMakeLists assumes it is the top level.** It always builds the MoonLight app and the tests, and names paths through `CMAKE_SOURCE_DIR` (23 times, 7 more in `test/`) and the include root through `CMAKE_BINARY_DIR`. Under `add_subdirectory` or `FetchContent` those point into the consumer's tree.
- **`mm_core` builds three MoonLight inputs**: the web UI (`ui_embed`, from `src/ui`), the version (`build_info_gen`, from `library.json`) and the script catalog (`moonlive_catalog`, from `moonlive/`).
- **Core's source list is written three times**: the root CMakeLists (26 lines naming `src/core/`), `esp32/main/CMakeLists.txt` (22) and `teensy/CMakeLists.txt` (25). The ESP32 component holds core, the platform, light and the app as one.
- **The ESP32 platform includes light**: `platform_esp32_h264.cpp` takes `light/util/MpegTs.h` (shared with `HlsDriver`), `platform_esp32_hub75.cpp` takes `light/drivers/Hub75Slots.h` (shared with `Hub75Driver`). The desktop platform includes none.
- **Light behavior inside core.** 15 core files name a light type. Most are the interfaces the architecture asks for (`LightOutput`, read by MQTT and MoonStats). Three are behavior:
  - the WLED shim, `HttpServerWled.cpp`, 11 light names;
  - the preset roles in `ControlModule.h`, `kCapturable`: `Layouts`, `Effects`, `Drivers`, `Services`;
  - MoonLive's light script kinds in `MoonLiveScriptFile.h` (`.mle`, `.mll`, `.mlm`, `.mlp`, beside core's own `.mls`), and the generated `script_catalog.h`, which lists MoonLight's scripts. The light kinds' modules and builtins already live in `src/light/moonlive/`.
- **The UI names light types**: 69 literals in `src/ui`, counted by `check_code`'s `module type in the UI`. The 3D view is core's; the preview frames it shows come from `PreviewDriver`, a light driver.
- No install or export rules, so `find_package` has nothing to find.
- **FastLED-MM** (2026-04 to 2026-05) is a PlatformIO and Arduino project on projectMM, MoonLight's predecessor, with ArduinoJson and ESPAsyncWebServer. A sketch is an effect module filling a `CRGB leds[]` and a driver module calling `FastLED.show()`; it shows a live preview and takes Art-Net. Its README says it is on hold until MoonLight ships as a library. [how-moonlight-works.md](../explanation/how-moonlight-works.md) names it as the second domain.

## End states

| | End state | Gains | Loses |
|---|---|---|---|
| A | **A CMake library on the desktop.** `FetchContent` or `add_subdirectory` of MoonLight gives `MoonModules::core` and `MoonModules::platform`; the app and tests build only at the top level; the UI, version and script catalog are the app's inputs. | Small, all CMake. A desktop or Raspberry Pi app on MoonCore works. | No firmware app: an ESP32 or Teensy consumer copies the source list. |
| B | **A, plus an ESP-IDF component and a Teensy fragment**, core's sources listed once. | A firmware app on every target MoonLight runs on. The three source lists become one. | A consumer still carries the WLED routes, the light preset roles and the light script kinds. |
| **C** | **B, with the light behavior out of core.** The WLED shim moves to light, the preset roles and MoonLive's script kinds are registered by the domain, and the UI reads what it shows from the modules. | Core is domain-neutral in behavior, not only in includes: FastLED-MM carries no WLED routes, no light script kinds, no light scripts and no light presets. | The largest: the UI part is the `module type in the UI` sweep, 69 literals. |
| D | **A separate MoonCore repository**, which MoonLight takes as a dependency. | Ownership is visible in the repository layout. | Every core change is two pull requests and two CI runs, and CLAUDE.md's one source tree splits. |

**Picked: C** (the product owner, 2026-10-25). A and B are its first two phases, each leaving the tree releasable.

## Design

### Phase 1: the library (A)

- **One source list.** `src/core/sources.cmake` sets `MM_CORE_SOURCES`, paths relative to its own directory. The root CMakeLists, `esp32/main` and `teensy` read it rather than list the files. A platform's sources stay in that platform's build, since only it compiles them.
- **Paths through `PROJECT_SOURCE_DIR` and `PROJECT_BINARY_DIR`**, so MoonLight builds the same under a consumer.
- **`if(PROJECT_IS_TOP_LEVEL)`** around the MoonLight executable, `enable_testing()` and `add_subdirectory(test)`. A consumer gets the two libraries and nothing else.
- **`MoonModules::core` and `MoonModules::platform`** as `ALIAS` targets, CMake's naming for a target a consumer links, after the organization.
- **The three inputs become cache variables with MoonLight's as the default**: `MM_UI_DIR` (`src/ui`), `MM_VERSION_FILE` (`library.json`), `MM_SCRIPT_DIR` (`moonlive/`). A consumer sets its own; an empty `MM_SCRIPT_DIR` gives an empty catalog.
- The include root stays as step 12 built it, under `PROJECT_BINARY_DIR`.
- **The pin**: `test/consumer/` is a minimal app, one `CMakeLists.txt` that adds MoonLight with `add_subdirectory` and a `main.cpp` with one core module and the HTTP server. A CTest step configures and builds it, so a change that makes MoonCore unusable alone fails a test. It stays the in-repo gate; FastLED-MM is the real app outside it.

### Phase 2: the firmware components (B)

- **First**: `MpegTs.h` is a container format and `Hub75Slots.h` a panel's slot table. `MpegTs.h` moves to `core/util` (a byte format, domain-neutral), `Hub75Slots.h`'s bus contract to `platform/` beside the HUB75 seam, the encoder staying in light. `check_code`'s platform rule then also counts a platform file including light.
- **ESP-IDF**: `esp32/components/mooncore/CMakeLists.txt` registers `MM_CORE_SOURCES` with the ESP32 platform sources, and `esp32/main` keeps light and the app with `REQUIRES mooncore`. Its include directories are the core root, not `src/`, so the IDF build holds the boundary too. A consumer lists the component through `EXTRA_COMPONENT_DIRS`, the IDF's way to take a component from another tree.
- **Teensy**: `teensy/CMakeLists.txt` reads `sources.cmake` the same way; the Teensy build has one component, so no boundary changes there.

### Phase 3: light behavior out of core (C)

Each item is Dependency Inversion, the move `check_code` already names for a crossing: core declares a registry, the light domain registers into it at the start, as module types register today.

- **The WLED shim moves to `src/light/`.** Core's HTTP server gains a route registration (a path, a method, a handler), and light registers the WLED routes and the WebSocket frame through it. A FastLED-MM device then answers no WLED app, which is right: it is not a light rig in WLED's sense unless it says so.
- **Preset roles are registered.** `kCapturable` becomes a list the app fills: MoonLight registers `Layouts`, `Effects`, `Drivers` and `Services`, the pure-look role marked as such. A preset in another app captures that app's containers.
- **Script kinds are registered.** Core's MoonLive keeps the compiler, the IR, the backends and its own kind, `.mls` services. A domain registers each script kind: the extension, the module type a script of it becomes, and its builtins table. Light registers `.mle`, `.mll`, `.mlm` and `.mlp` from `src/light/moonlive/`, where their modules and builtins already are. The script catalog is generated from the app's `MM_SCRIPT_DIR`, and the name check (`script name must end in …`) reads the registered kinds. At C, core holds no light script kind and the catalog of a FastLED-MM build holds no MoonLight script.
- **The UI reads what it shows from the modules.** The 69 literals each become a declaration on the module (a control, a role, a flag), the move the `module type in the UI` rule names. The rule then reads 0 and stays as the ratchet.
- **The preview frame becomes core's.** The 3D view is core's, and every domain with pixels wants it fed. The frame's send (the WebSocket channel, the size and the pixel format) moves to a core service that a domain feeds; `PreviewDriver` keeps what is light's, the layout's coordinates.

### The first consumer: FastLED-MM on MoonCore

FastLED-MM moves from projectMM to MoonCore, as a new major version in its own repository, an ESP-IDF project rather than PlatformIO and Arduino, since MoonCore is native ESP-IDF.

- **Its modules**: `WaveRainbow2D` as an effect module filling a `CRGB` buffer it takes in `prepare()`, and `FastLEDDriver` calling `FastLED.show()` through FastLED's Channels API. Its own `module_types.cpp` registers them and its containers, and registers them as its preset roles.
- **What it gets from MoonCore**: WiFi and the access point, the web UI and the 3D preview fed by its driver, live controls over WebSocket and REST, persistent state, MQTT, the module timing, MoonLive services, OTA and the web installer.
- **What it does not get**: layouts, modifiers, the light drivers, the WLED shim, the light script kinds. Art-Net, which the projectMM version had, is light's today; it comes back when FastLED-MM needs it, through the core seam that phase 3 shows the shape of.
- **Its build**: `idf.py` with MoonLight as a git submodule or `FetchContent`, the `mooncore` component through `EXTRA_COMPONENT_DIRS`, and FastLED as a component.
- **The product owner owns that repository and its git operations**; the agent writes the port on a branch there when the product owner says so.

### The direction beyond it: MoonCore as an alternative to ESPHome and Tasmota

FastLED-MM proves a second domain; a home-automation firmware is the larger one. ESPHome and Tasmota are the two firmwares a smart-home ESP32 runs today, and they configure at opposite ends: ESPHome compiles a firmware per device from YAML, so a change is a rebuild and a flash; Tasmota is one prebuilt firmware configured at runtime through templates, console commands, Rules and Berry scripts. MoonCore sits on Tasmota's side, with what neither has:

| | ESPHome | Tasmota | MoonCore at C |
|---|---|---|---|
| Configuration | YAML, compiled per device | runtime, templates and commands | runtime: modules added, removed and set live, every change on the next frame |
| A board's setup | the YAML | a template | a `deviceModels.json` entry, applied from the web installer or the device |
| Scripting | lambdas in C++, compiled in | Rules, Berry (interpreted) | MoonLive, compiled on the device to native code |
| Home Assistant | native API, MQTT | MQTT discovery | MQTT discovery |
| Install and update | web installer, OTA | web installer, OTA | web installer, OTA, MoonBase recovery |
| Sensors and actuators | hundreds of components | a large driver set | the input mapping plan's set: buttons, encoders, analog, I2C, PIR |

**What C gives this direction**: a core with no light behavior is the firmware a relay, a sensor node or a plug runs, and the second app is then as small as FastLED-MM.

**What it does not give, each its own plan after C**:
- **Entities from modules.** Home Assistant discovery publishes a light today, through `LightOutput`. A switch, a sensor, a number or a select per control, declared by the module, is what an ESPHome device shows in Home Assistant.
- **Peripheral breadth.** [Input mapping](Plan-20260901%20-%20Input%20mapping%20and%20scripted%20sensors%20(7%20of%2012).md) holds the sensors; relays and PWM outputs as modules join it.
- **Taking over a device.** MoonBase installs over a running Tasmota or ESPHome; the backlog names what each still needs (Tasmota's settings reader, ESPHome's compiled-in WiFi).
- **A second consumer for it**: a relay and sensor app, on a board still to pick.

### Other apps on MoonCore

Each is its own repository on `MoonModules::core`, as FastLED-MM is, and each its own plan after C.

| App | What it is | What it needs from core beyond C |
|---|---|---|
| **FastLED-MM** | FastLED effects and drivers behind MoonCore's UI | nothing: this plan's consumer |
| **A relay and sensor app** | the ESPHome and Tasmota direction above | entities from modules, relays and PWM outputs as modules |
| **A Home Assistant front end** | Home Assistant's entities shown and controlled in MoonCore's UI, in a browser or on a board with a display as a wall panel. It connects to Home Assistant's WebSocket API (`/api/websocket`, a long-lived access token), reads the entities with `get_states` and follows them with `subscribe_entities`, and sets one with `call_service`. Each entity becomes a module with its controls, so a light is a slider and a color, a switch a toggle, a sensor a value, and a change from either side applies live. | an outbound WebSocket client: core has the server and the outbound TCP connection MQTT uses, not the client's upgrade and framing (RFC 6455) |

The relay app and the front end are the two directions of one integration: the first is a device Home Assistant controls, the second a panel that controls Home Assistant.

## Estimate

| Step | Work |
|---|---|
| 1. `sources.cmake`, read by the three builds | 0.5 day |
| 2. Top-level guard, `PROJECT_*` paths, the alias targets, the three inputs | 1 day |
| 3. The consumer app and its CTest step | 0.5 day |
| 4. `MpegTs.h` and `Hub75Slots.h` out of light; the platform rule counts platform-to-light | 0.5 day |
| 5. The `mooncore` IDF component; `esp32/main` requires it; the ESP32 consumer in CI | 1.5 days, plus a board build per shipped variant |
| 6. The WLED shim to light behind a route registration | 1 day |
| 7. Preset roles registered | 0.5 day |
| 8. Script kinds registered; the catalog from the app's script directory | 1 day |
| 9. The preview frame as a core service | 1 day |
| 10. The UI's 69 type literals as module declarations | 2 to 3 days |
| 11. FastLED-MM on MoonCore, on a board | 2 days, plus bench time |
| 12. Docs | 0.5 day |
| **Total** | **about 13 days** |

## Steps

Phase 1, the library:

1. ✅ Core's sources listed once in `src/core/sources.cmake`; the desktop, ESP32 and Teensy builds read it, and each builds unchanged (2026-10-25).
2. ⏳ The root CMakeLists builds as a subdirectory: `PROJECT_*` paths, the app and tests only at the top level, `MoonModules::core` and `MoonModules::platform`, and `MM_UI_DIR`, `MM_VERSION_FILE`, `MM_SCRIPT_DIR` with MoonLight's defaults.
3. ⏳ `test/consumer/`, a core-only app, configured and built by a test; a light include added to it fails.

Phase 2, the firmware components:

4. ✅ `MpegTs.h` to `core/util`, and `Hub75Slots.h`'s bus contract (`Hub75Layout`, `Hub75Geometry`) to `platform/Hub75Bus.h`, the encoder staying in light (2026-10-25). No platform file includes light; the desktop build holds it already, `mm_platform` compiling through the core root, and step 5 holds it on the ESP32 the same way, so no text rule counts it.
5. ⏳ The `mooncore` IDF component with the core include root; `esp32/main` requires it; every shipped ESP32 variant builds, and the ESP32 consumer builds in CI. Flashed on a board only on the product owner's go.

Phase 3, light behavior out of core:

6. ⏳ Core's HTTP server takes registered routes; the WLED shim moves to `src/light/` and registers its routes and frame; the WLED tests pass unchanged, and the consumer answers no WLED route.
7. ⏳ Preset roles registered by the app; MoonLight's presets apply as before, and a consumer's preset captures its own containers.
8. ⏳ Script kinds registered by the domain; light registers its four; the catalog comes from `MM_SCRIPT_DIR`; the consumer's build holds no light kind and no MoonLight script.
9. ⏳ The preview frame's send as a core service; `PreviewDriver` feeds it; the consumer's driver feeds it too.
10. ⏳ `module type in the UI` at 0: each literal a declaration on its module.
11. ⏳ FastLED-MM on MoonCore: its two modules, its registry, its IDF build; a 16x16 panel lit and previewed on an esp32dev and an ESP32-S3, the product owner's eyes as the measurement.
12. ⏳ The build-system page describes the library, its three inputs and the IDF component; a MoonWalk process "Build an app on MoonCore", with FastLED-MM as its example; the architecture's MoonCore page and how-moonlight-works.md link it.

## Decisions

- **End state C** (the product owner, 2026-10-25), built in three phases, A and B first.
- **`MoonModules::core` and `MoonModules::platform`** as the targets (the product owner, 2026-10-25).
- **FastLED-MM as the first consumer** (the product owner, 2026-10-25, "maybe"; recommended here): it exists, it is MoonModules' own, its domain is LEDs without MoonLight's light domain, which tests the boundary hardest, and its README already waits for this.
- **Script kinds through a registry, the domain registering its own** (recommended; the product owner asked for the best approach, with no MoonLive light scripts in core at C). It is the self-registration module types already use, so a third domain adds kinds without touching core.
- **`add_subdirectory` and `FetchContent`, not an installed package.** A firmware build compiles from source anyway, and an `install(EXPORT)` package is a second way to consume the same code. It waits for a consumer that needs a prebuilt library.

## Open questions

- **FastLED on ESP-IDF v6 without Arduino**: probed 2026-10-25 with FastLED-MM's pinned master (`c83f7632`, 2026-04-11) on IDF v6.1 for the S3. Not as shipped: its component `REQUIRES arduino-esp32`, its source glob matches nothing (the sources are the unity files in `src/fl/build/`), and its platform detection needs `ESP32` defined. With those three fixed it compiles its own sources and stops on IDF 6 header drift (`SOC_GPIO_VALID_OUTPUT_GPIO_MASK`, the moved I2S headers) and one `-Werror=type-limits`. The RMT driver and the Channels API are untried. A newer FastLED commit, or patches upstream, is step 11's first task.
- **The IDF component's name**: `mooncore`, or `moonmodules_core` to match the CMake targets? **Recommended (2026-10-25):** `moonmodules_core`, matching `MoonModules::core`, and `moonmodules/core` if it is ever published to the component registry.
- **Art-Net for FastLED-MM**: back in step 11, which needs the packet and the receiver in core, or after this plan? **Recommended (2026-10-25):** after this plan, so step 11 proves the boundary with the least in core.
- **The ESPHome and Tasmota direction**: its first plan after C, entities from modules or the relay and sensor app? Entities from modules is the one that app needs too. **Recommended (2026-10-25):** entities from modules.

## What it removes

- Two of the three copies of core's source list.
- The ESP32 platform's two light includes.
- The assumption that MoonLight is the top-level project, in `CMAKE_SOURCE_DIR` and `CMAKE_BINARY_DIR` throughout the build.
- The light behavior in core: the WLED shim, `kCapturable`'s light names, the light script kinds and MoonLight's scripts in core's catalog.
- The 69 light type literals in `src/ui`.
- FastLED-MM's dependency on projectMM, ArduinoJson and ESPAsyncWebServer.
