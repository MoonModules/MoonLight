idf_component_register(
    SRCS
        "main.cpp"
        "../../src/main.cpp"
        # The shared type registry, compiled beside main.cpp here and beside the scenario runner on
        # desktop, so one list serves both: src/module_types.cpp carries the reason.
        "../../src/module_types.cpp"
        "../../src/core/module/Control.cpp"
        "../../src/core/util/JsonUtil.cpp"
        "../../src/core/util/ScratchBuffer.cpp"
        "../../src/core/system/HttpServerModule.cpp"
        "../../src/core/system/FilesystemModule.cpp"
        "../../src/core/system/FileManagerModule.cpp"
        "../../src/core/system/MqttModule.cpp"
        "../../src/core/module/Scheduler.cpp"
        "../../src/core/util/sha256.cpp"
        "../../src/core/moonlive/MoonLive.cpp"
        "../../src/core/moonlive/MoonLiveCompiler.cpp"
        "../../src/core/moonlive/MoonLiveSpill.cpp"
        "../../src/platform/esp32/platform_esp32.cpp"
        "../../src/platform/esp32/moonlive_emit_xtensa.cpp"
        "../../src/platform/esp32/moonlive_emit_riscv.cpp"
        "../../src/platform/esp32/moonlive_asm_xtensa.cpp"
        "../../src/platform/esp32/moonlive_asm_riscv.cpp"
        "../../src/platform/esp32/platform_esp32_fs.cpp"
    "../../src/platform/esp32/platform_esp32_h264.cpp"
        "../../src/platform/esp32/platform_esp32_ota.cpp"
        "../../src/platform/esp32/platform_esp32_improv.cpp"
        "../../src/platform/esp32/platform_esp32_rmt.cpp"
        "../../src/platform/esp32/rmt_hi_vector.S"
        "../../src/platform/esp32/platform_esp32_tasks.cpp"
        "../../src/platform/esp32/platform_esp32_worker.cpp"
        "../../src/platform/esp32/platform_esp32_gpio.cpp"
        "../../src/platform/esp32/platform_esp32_i80.cpp"
        "../../src/platform/esp32/platform_esp32_hub75.cpp"
        "../../src/platform/esp32/platform_esp32_moon_i80.cpp"
        "../../src/platform/esp32/platform_esp32_parlio.cpp"
        "../../src/platform/esp32/platform_esp32_i2s.cpp"
        "../../src/platform/esp32/platform_esp32_i2c.cpp"
        "../../src/platform/esp32/platform_esp32_ir.cpp"
        "../../src/platform/esp32/platform_esp32_es8311.cpp"
    INCLUDE_DIRS
        "../../src"
        "../../src/platform/esp32"
)
# Note: no explicit REQUIRES — the component relies on IDF's default common
# requirements, which already transitively expose driver/, esp_timer, nvs_flash,
# esp_https_ota, esp_driver_rmt, etc. Adding an explicit REQUIRES switches the
# component to strict mode and would hide every implicitly-available component,
# breaking the existing files. esp_driver_rmt's include dir is already on the
# compile line via the common set, so platform_esp32_rmt.cpp finds driver/rmt_*.h.

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# Xtensa: interleave literal pools into the text section (-mauto-litpools). Required by MM_RAMFUNC
# (IRAM_ATTR) on TEMPLATE functions: a comdat instantiation's code lands in `.iram1.*` but its literal
# pool in `.literal._Z…`, which the IDF linker script keeps in flash — out of l32r's backward reach
# ("dangerous relocation: literal placed after use"). Auto-litpools makes each function self-contained.
# RISC-V targets (P4) place constants inline natively and don't know the flag.
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    target_compile_options(${COMPONENT_LIB} PRIVATE -mauto-litpools)
endif()

# Classic ESP32 only: the level-5 RMT refill vector (rmt_hi_vector.S) overrides a WEAK default, so
# nothing references its object and the linker would drop it. -u forces the symbol in.
if(CONFIG_IDF_TARGET_ESP32)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ld_include_rmt_hi_vector")
endif()

# Firmware-variant defines, set by moondeck/build/build_esp32.py via firmware_cmake_args().
# See docs/explanation/architecture/mooninstaller.md § Firmware vs board — "firmware" is the compiled
# binary variant; the physical "board" is a separate concept the device
# cannot identify on its own.
#
# MM_FIRMWARE_NAME — the firmware variant key (`esp32`, `esp32-eth`,
#                 `esp32-eth-wifi`, `esp32s3-n16r8`). Surfaced by
#                 SystemModule.firmware on the device card and used by
#                 FirmwareUpdateModule's release picker to filter to
#                 OTA-compatible binaries. Cache variables in ESP-IDF reach
#                 this CMakeLists but not the component compile line by
#                 default, so we forward it explicitly. Without this,
#                 build_info.h's `#ifndef` fallback kicks in and the device
#                 reports "firmware: unknown".
#
# MM_ETH_ONLY  — set on `--firmware esp32-eth` (no WiFi compiled in).
#                The script also passes EXCLUDE_COMPONENTS for the WiFi libs;
#                this define gates the WiFi source paths in platform_esp32.cpp
#                and the platform::hasWiFi compile-time flag.
#
# MM_NO_ETH    — set on firmwares that don't enable CONFIG_ETH_USE_ESP32_EMAC
#                (today: `--firmware esp32`, `--firmware esp32s3-n16r8`).
#                Without the EMAC headers, the on-chip Ethernet code in
#                platform_esp32.cpp won't compile; this define stubs the
#                eth* platform functions to return false/empty, mirroring
#                the desktop platform layer.
if(MM_FIRMWARE_NAME)
    # build_esp32.py passes -DMM_FIRMWARE_NAME="<key>" where the outer quotes
    # become part of the CMake variable value (verified via CMakeCache.txt).
    # So ${MM_FIRMWARE_NAME} already expands to "esp32-eth" *with* the quotes —
    # don't re-add them here, or the preprocessor sees MM_FIRMWARE_NAME=""esp32""
    # and fails as if "esp32" were an identifier.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_FIRMWARE_NAME=${MM_FIRMWARE_NAME})
endif()
if(MM_RELEASE)
    # Same quoting note as MM_FIRMWARE_NAME above — the value arrives with its
    # quotes already baked in. Release-channel tag (latest / vX.Y.Z); only the
    # release workflow sets it, so local builds fall through to build_info.h's
    # #ifndef "" default.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_RELEASE=${MM_RELEASE})
endif()
if(MM_VERSION)
    # Same quoting note again. The computed semver (compute_version.py): the core
    # for a stable tag, <core>-dev.<N> for a moving latest build. Only the release
    # pipeline sets it; a local build falls through to build_info.h's #ifndef
    # default (library.json). Overrides the header's #ifndef so the binary, the
    # release asset names, and the manifest all carry the same version.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_VERSION=${MM_VERSION})
endif()
if(MM_ETH_ONLY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_NO_WIFI)
endif()
if(MM_PANEL_CARDS)
    # Links PanelCardDriver (panel receiver cards over raw L2). Opt-in per firmware because the
    # cards want a gigabit link and no CONFIG_SOC_* macro distinguishes a gigabit board from a
    # 100 Mbit one — see the gate in src/main.cpp.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_PANEL_CARDS)
endif()
if(MM_NO_ETH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_NO_ETH)
endif()
# MM_TASK_CPU_STATS — opt-in per-task CPU% in TasksModule. Off by default because the FreeRTOS
# run-time-stats counter it needs (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, added by the
# sdkconfig.defaults.task-cpu-stats fragment build_esp32.py appends when this is set) reads a timer
# on every context switch — measured ~5% tick. A profiling build turns it on; normal builds pay
# nothing. The define both fills TaskInfo.cpuPermille (platform_esp32_tasks.cpp) and shows the column.
if(MM_TASK_CPU_STATS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_TASK_CPU_STATS=1)
endif()

# MM_MAX_GPIO — the highest valid GPIO for this chip, the default clamp for
# Control.h's Pin controls. Derived from the IDF's own per-chip soc-cap
# (CONFIG_SOC_GPIO_PIN_COUNT, exposed as a CMake var after `idf.py set-target`):
# count-1 is the max index (62 on the S31 → 61). Injecting it here keeps Control.h
# core (no platform/IDF include) while the value stays IDF-sourced, not hand-kept.
if(CONFIG_SOC_GPIO_PIN_COUNT)
    math(EXPR MM_MAX_GPIO "${CONFIG_SOC_GPIO_PIN_COUNT} - 1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MM_MAX_GPIO=${MM_MAX_GPIO})
endif()

# Embed UI files — regenerate on every build. ESP-IDF's bundled Python venv
# is named `python.exe` on Windows and `python3` on macOS/Linux; bare `python3`
# fails on the Windows IDF venv. find_package(Python3) lets CMake locate the
# interpreter via the same logic IDF uses (PATH, IDF env), so the same code
# works on every host. The desktop CMakeLists routes through `uv run python`
# for the same reason — only for the IDF build path do we let CMake pick the
# interpreter, because adding uv to ESP-IDF docker would be a bigger CI lift
# than the portability win pays for.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(UI_DIR ${COMPONENT_DIR}/../../src/ui)
add_custom_command(
    OUTPUT ${UI_DIR}/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DOUT=${UI_DIR}/ui_embedded.h -DPYTHON_CMD=${Python3_EXECUTABLE} -P ${UI_DIR}/embed_ui.cmake
    DEPENDS ${UI_DIR}/index.html ${UI_DIR}/app.js ${UI_DIR}/style.css ${UI_DIR}/install-picker.js ${UI_DIR}/preview3d.js ${UI_DIR}/moonmodules-logo.png ${UI_DIR}/embed_ui.cmake
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${UI_DIR}/ui_embedded.h)

# The MoonLive script catalog: names of every factory script, so the picker can offer the whole
# library while the device holds only what someone actually picked. CONFIGURE_DEPENDS re-globs when
# a script is added or removed, so the catalog cannot go stale against the repo.
set(MOONLIVE_DIR ${COMPONENT_DIR}/../../moonlive)
set(MOONLIVE_GEN ${COMPONENT_DIR}/../../src/light/moonlive)
file(GLOB MOONLIVE_SCRIPTS CONFIGURE_DEPENDS
     ${MOONLIVE_DIR}/effects/*.mle ${MOONLIVE_DIR}/layouts/*.mll ${MOONLIVE_DIR}/modifiers/*.mlm)
# See the desktop CMakeLists: the file LIST is a dependency, so a DELETED script regenerates the
# catalog rather than lingering in it.
string(REPLACE ";" "\n" MOONLIVE_SCRIPT_LIST "${MOONLIVE_SCRIPTS}")
set(MOONLIVE_STAMP ${CMAKE_BINARY_DIR}/moonlive_scripts.stamp)
file(CONFIGURE OUTPUT ${MOONLIVE_STAMP} CONTENT "${MOONLIVE_SCRIPT_LIST}\n")
add_custom_command(
    OUTPUT ${MOONLIVE_GEN}/script_catalog.h
    COMMAND ${CMAKE_COMMAND} -DSCRIPT_DIR=${MOONLIVE_DIR} -DOUT=${MOONLIVE_GEN}/script_catalog.h -DPYTHON_CMD=${Python3_EXECUTABLE} -P ${MOONLIVE_GEN}/catalog_scripts.cmake
    DEPENDS ${MOONLIVE_SCRIPTS} ${MOONLIVE_STAMP} ${MOONLIVE_GEN}/catalog_scripts.cmake ${MOONLIVE_GEN}/catalog_scripts.py
    COMMENT "Generating MoonLive script catalog"
)
add_custom_target(moonlive_catalog DEPENDS ${MOONLIVE_GEN}/script_catalog.h)

# Generate build_info.h from library.json + git (carries version, build id, build date, board name).
#
# **Deliberately ALWAYS out-of-date** — the target runs the generator on every build, not just when
# library.json changes. MM_BUILD_ID is the git hash the binary is built from, and it is the only
# trustworthy answer to "which code is on this board?": MM_BUILD_DATE cannot be, because __DATE__
# expands when the *including* TU compiles, so editing a driver .cpp leaves the date frozen and a
# freshly flashed board reports the OLD one, which reads as "the flash didn't take" and sends debugging
# at the wrong binary. Pinning the rule to library.json's mtime has the same failure. The generator is a
# sub-second script and rewrites the header only when its content changes, so an unchanged tree still
# costs nothing downstream.
set(BUILD_INFO_DIR ${COMPONENT_DIR}/../../src/core)
add_custom_target(build_info_gen ALL
    COMMAND ${Python3_EXECUTABLE} ${COMPONENT_DIR}/../../moondeck/build/generate_build_info.py
    BYPRODUCTS ${BUILD_INFO_DIR}/build_info.h
    COMMENT "Generating build_info.h (git build id)"
    VERBATIM
)

add_dependencies(${COMPONENT_LIB} ui_embed build_info_gen moonlive_catalog)
