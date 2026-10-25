# Core's sources, the list every build reads.
include(${CMAKE_CURRENT_LIST_DIR}/../../src/core/sources.cmake)

idf_component_register(
    SRCS
        "main.cpp"
        "../../src/main.cpp"
        # The shared type registry, compiled beside main.cpp here and beside the scenario runner on
        # desktop, so one list serves both: src/module_types.cpp carries the reason.
        "../../src/module_types.cpp"
        ${MM_CORE_SOURCES}
        "../../src/platform/esp32/platform_esp32.cpp"
        "../../src/platform/esp32/moonlive_emit_xtensa.cpp"
        "../../src/platform/esp32/moonlive_emit_riscv.cpp"
        "../../src/platform/esp32/moonlive_asm_xtensa.cpp"
        "../../src/platform/esp32/moonlive_asm_riscv.cpp"
        "../../src/platform/esp32/platform_esp32_fs.cpp"
        "../../src/platform/esp32/platform_esp32_sdmmc.cpp"
    "../../src/platform/esp32/platform_esp32_h264.cpp"
        "../../src/platform/esp32/platform_esp32_ota.cpp"
        "../../src/platform/esp32/platform_esp32_improv.cpp"
        "../../src/platform/esp32/platform_esp32_rmt.cpp"
        "../../src/platform/esp32/platform_esp32_pwm.cpp"
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
        "../../src/platform/esp32/platform_esp32_usb_midi.cpp"
        "../../src/platform/esp32/platform_esp32_usbvideo.cpp"
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

# The script compiler runs once per script load, never per frame, so it is built for size: the code it emits is the same either way.
set_source_files_properties("../../src/core/moonlive/MoonLiveCompiler.cpp" PROPERTIES COMPILE_OPTIONS "-Os")

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

# Where build_info.cpp and its generated build_id.h live: the build identity's one unit.
set(BUILD_INFO_DIR ${COMPONENT_DIR}/../../src/core/util)

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
#                 build_info.cpp's `#ifndef` fallback kicks in and the device
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
    # build_esp32.py passes the bare key and the quotes go on here, as the Teensy build does: idf.py
    # strips quotes from a -D value before comparing it with the cache, so a quoted value never
    # matched and re-ran CMake on every build.
    set_property(SOURCE ${BUILD_INFO_DIR}/build_info.cpp APPEND PROPERTY COMPILE_DEFINITIONS MM_FIRMWARE_NAME="${MM_FIRMWARE_NAME}")
endif()
if(MM_RELEASE)
    # Quoted here, as MM_FIRMWARE_NAME above. Release-channel tag (latest / vX.Y.Z); only the
    # release workflow sets it, so local builds fall through to build_info.cpp's #ifndef "" default.
    set_property(SOURCE ${BUILD_INFO_DIR}/build_info.cpp APPEND PROPERTY COMPILE_DEFINITIONS MM_RELEASE="${MM_RELEASE}")
endif()
if(MM_VERSION)
    # Quoted here too. The computed semver (compute_version.py): the core
    # for a stable tag, <core>-dev.<N> for a moving latest build. Only the release
    # pipeline sets it; a local build falls through to build_info.cpp's #ifndef
    # default (library.json). Overrides the header's #ifndef so the binary, the
    # release asset names, and the manifest all carry the same version.
    set_property(SOURCE ${BUILD_INFO_DIR}/build_info.cpp APPEND PROPERTY COMPILE_DEFINITIONS MM_VERSION="${MM_VERSION}")
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
# Every UI file, by glob, so a file added to src/ui re-embeds without a second edit here: the hand-kept list had already missed migrate.js, semver.js and preview-adapt.js.
file(GLOB UI_SOURCES CONFIGURE_DEPENDS ${UI_DIR}/*.js ${UI_DIR}/*.html ${UI_DIR}/*.css ${UI_DIR}/*.png
     ${UI_DIR}/vendor/*.js)
add_custom_command(
    OUTPUT ${UI_DIR}/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DOUT=${UI_DIR}/ui_embedded.h -DPYTHON_CMD=${Python3_EXECUTABLE} -P ${UI_DIR}/embed_ui.cmake
    DEPENDS ${UI_SOURCES} ${UI_DIR}/embed_ui.cmake ${UI_DIR}/../../moondeck/build/bundle_ui.py
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${UI_DIR}/ui_embedded.h)

# The MoonLive script catalog: names of every factory script, so the picker can offer the whole
# library while the device holds only what someone actually picked. CONFIGURE_DEPENDS re-globs when
# a script is added or removed, so the catalog cannot go stale against the repo.
set(MOONLIVE_DIR ${COMPONENT_DIR}/../../moonlive)
set(MOONLIVE_GEN ${COMPONENT_DIR}/../../src/core/moonlive)
file(GLOB_RECURSE MOONLIVE_SCRIPTS CONFIGURE_DEPENDS
     ${MOONLIVE_DIR}/*.mle ${MOONLIVE_DIR}/*.mll ${MOONLIVE_DIR}/*.mlm ${MOONLIVE_DIR}/*.mls ${MOONLIVE_DIR}/*.mlp)
# See the desktop CMakeLists: the file LIST is a dependency, so a DELETED script regenerates the
# catalog rather than lingering in it.
string(REPLACE ";" "\n" MOONLIVE_SCRIPT_LIST "${MOONLIVE_SCRIPTS}")
set(MOONLIVE_STAMP ${CMAKE_BINARY_DIR}/moonlive_scripts.stamp)
file(CONFIGURE OUTPUT ${MOONLIVE_STAMP} CONTENT "${MOONLIVE_SCRIPT_LIST}\n")
# The rule's OUTPUT is a stamp in the build tree, not the tracked header, which a clean would delete: the root CMakeLists.txt carries the reason.
set(MOONLIVE_CATALOG_STAMP ${CMAKE_BINARY_DIR}/moonlive_catalog.stamp)
add_custom_command(
    OUTPUT ${MOONLIVE_CATALOG_STAMP}
    COMMAND ${CMAKE_COMMAND} -DSCRIPT_DIR=${MOONLIVE_DIR} -DOUT=${MOONLIVE_GEN}/script_catalog.h -DPYTHON_CMD=${Python3_EXECUTABLE} -P ${MOONLIVE_GEN}/catalog_scripts.cmake
    COMMAND ${CMAKE_COMMAND} -E touch ${MOONLIVE_CATALOG_STAMP}
    DEPENDS ${MOONLIVE_SCRIPTS} ${MOONLIVE_STAMP} ${MOONLIVE_GEN}/catalog_scripts.cmake ${MOONLIVE_GEN}/catalog_scripts.py
    COMMENT "Generating MoonLive script catalog"
)
add_custom_target(moonlive_catalog DEPENDS ${MOONLIVE_CATALOG_STAMP})

# Generate build_id.h from library.json + git: the build id and the default version, for build_info.cpp alone.
#
# **Deliberately ALWAYS out-of-date** — the target runs the generator on every build, not just when
# library.json changes. MM_BUILD_ID is the git hash the binary is built from, and it is the only
# trustworthy answer to "which code is on this board?": a stale one reads as "the flash didn't take"
# and sends debugging at the wrong binary, which pinning the rule to library.json's mtime caused. The generator is a
# sub-second script and rewrites the header only when its content changes, so an unchanged tree recompiles
# build_info.cpp alone, the one unit holding the clock, and relinks.
add_custom_target(build_info_gen ALL
    COMMAND ${Python3_EXECUTABLE} ${COMPONENT_DIR}/../../moondeck/build/generate_build_info.py
    BYPRODUCTS ${BUILD_INFO_DIR}/build_id.h ${BUILD_INFO_DIR}/build_date.stamp
    COMMENT "Generating build_id.h (git build id)"
    VERBATIM
)
# The one unit holding the clock recompiles on every build, the generator touching its stamp.
set_source_files_properties(${BUILD_INFO_DIR}/build_info.cpp PROPERTIES OBJECT_DEPENDS ${BUILD_INFO_DIR}/build_date.stamp)

add_dependencies(${COMPONENT_LIB} ui_embed build_info_gen moonlive_catalog)
