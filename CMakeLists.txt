cmake_minimum_required(VERSION 3.20)
project(projectMM LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/<host>/compile_commands.json — the compilation database clangd reads to give
# real diagnostics in the editor (it needs the actual include paths and flags, not guesses).
# On by default because it costs nothing, is ignored by every consumer that doesn't want it,
# and a missing database is the single most common reason clangd reports phantom errors.
# `.clangd` at the repo root points at it; see docs/contributing/coding-standards.md § Editor setup.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags differ between GCC/Clang and MSVC. Gate by compiler so the
# desktop build runs cleanly on Windows runners (no -W flags on MSVC) and on
# macOS/Linux (no /W flags on Clang/GCC).
if(MSVC)
    # /W4 /WX matches the GCC/Clang `-Wall -Wextra -Werror` discipline. A few
    # MSVC-only warnings have no GCC equivalent at -Wall -Wextra and are
    # suppressed below rather than fought at every call site:
    #   C4702 — "unreachable code" inside `if constexpr` early-return branches
    #           (a well-known MSVC quirk: it analyses the discarded branch as
    #           live and flags everything after `return;`). GCC understands the
    #           constexpr discard and doesn't warn.
    #   C4244 / C4267 — implicit narrowing (int→smaller, size_t→smaller). GCC
    #           only warns under -Wconversion, which is not part of -Wall
    #           -Wextra. The codebase uses explicit static_cast at the sites
    #           where loss-of-data is the real concern; suppressing here brings
    #           MSVC noise level back in line with GCC.
    #   C5285 — vendored doctest.h (v2.4.11) forward-declares `std::tuple` as a
    #           libstdc++/libc++ portability workaround; the VS 2026 MSVC STL
    #           newly flags that as "specializing a standard template". It's in
    #           third-party header code we don't own and GCC/Clang never warn.
    add_compile_options(/W4 /WX /wd4702 /wd4244 /wd4267 /wd5285)
    # MSVC marks the standard C library (strcpy, sprintf, fopen, localtime, …)
    # as deprecated and emits C4996; /WX makes that fatal. Silence the family —
    # the code uses these standard functions deliberately and consistently with
    # the POSIX side, and the "secure" `_s` variants are a Microsoft extension.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
                            _WINSOCK_DEPRECATED_NO_WARNINGS)
    # Static MSVC runtime so the Windows binary doesn't need vcredist.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    # Tier zero on top of -Wall -Wextra: five warnings that catch real defects the base set
    # misses. -Wdouble-promotion is the one that earns its place here specifically — the Xtensa
    # has no FPU, so an accidental float->double promotion is a silent softfloat call in the
    # render path, and it enforces the integer-math rule the coding standards ask for.
    # (GCC/Clang only; the MSVC branch above uses /W4 /WX, which has no direct equivalents.)
    add_compile_options(-Wall -Wextra -Werror
                        -Wshadow -Wnon-virtual-dtor -Wdouble-promotion
                        -Wimplicit-fallthrough -Wnull-dereference)

    # GCC through 15 raises four of these on code that is provably correct, and -Werror turns
    # each into a failed build. They stay ON (a real one still has to be read and dismissed);
    # they just stop being fatal on the versions that cry wolf:
    #   -Wnull-dereference   fires on a plain `return member_` inlined through a lambda
    #                        trampoline, where GCC cannot prove `this` across the thunk, and on
    #                        `buf.data()[15]` after an allocate(10, 3) it cannot see through.
    #   -Wrestrict           fires on snprintf(dst, n, "%s", src) where both are members of the
    #                        same object, which GCC reads as a possible overlap.
    #   -Wstringop-overflow  fires inside <bits/stl_algobase.h> with a bound "between 2^63 and
    #                        2^64-1", a range no call in this codebase can produce.
    #   -Wformat-truncation  fires on snprintf(ssid_, 33, "%s", ssid) because a 99-char SSID
    #                        would not fit. Truncating there is the documented intent (see
    #                        NetworkModule.h): an over-long SSID is invalid, and snprintf
    #                        terminates what it writes.
    # Five of the twelve are inside libstdc++ and glibc headers (<bits/stl_algobase.h>,
    # <streambuf>, <stdio2.h>), so no source change can reach them. GCC 16 is clean, which is
    # why this went unnoticed locally: the maintainer's GCC is 16 and CI's is 13.
    # A version test rather than a flag probe, deliberately: the flags all EXIST here, it is
    # their judgement that is wrong, so CheckCXXCompilerFlag would happily confirm them.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        add_compile_options(-Wno-error=null-dereference
                            -Wno-error=restrict
                            -Wno-error=stringop-overflow
                            -Wno-error=format-truncation)
    endif()

    # Hot-path discipline, enforced by the compiler. MM_NONBLOCKING marks tick/tick20ms/tick1s
    # (platform.h); -Wfunction-effects then checks TRANSITIVELY that nothing they reach
    # allocates or blocks, through the whole call graph. Clang 20+ only; the flag does not
    # exist on GCC, so the ESP32 build gets no hot-path check — src/platform/esp32/ is
    # analysed only where the desktop build reaches it (docs/reference/testing.md § Static analysis).
    # Probe for the flag instead of inferring it from a version: AppleClang reports its own
    # version line (the CI macos-14 runner reads >= 20 while predating the warning), so a version
    # comparison enables -Wfunction-effects on toolchains that reject it — and it is -Werror.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wfunction-effects MM_HAS_WFUNCTION_EFFECTS)
    if(MM_HAS_WFUNCTION_EFFECTS)
        # -Wno-error is PERMANENT, not a stepping stone: this is a report, not a gate. The
        # findings are real and frozen in docs/reference/metrics/hotpath-baseline.txt as architecture
        # work; failing the build on them would block every other gate, and a new one may be
        # legitimate (a driver that must wait on hardware). The clang-hotpath card reports
        # what is NEW; a human judges it.
        add_compile_options(-Wfunction-effects -Wno-error=function-effects)
    endif()
endif()

# MM_MOONLIVE_NO_HOST_JIT builds this machine the way a desktop with NO MoonLive backend sees the
# code. Every x86-64 desktop is one (Windows, Linux, Intel macOS ship no backend), so a test that
# wrongly presumes a compile succeeds passes on the arm64 bench and fails only once CI runs it.
# A compile DEFINITION rather than a CMAKE_CXX_FLAGS entry: this file configures warnings through
# add_compile_options, which does not consume that variable, so a flag passed there would reach the
# cache and never the compiler.
option(MM_MOONLIVE_NO_HOST_JIT "Build as a desktop with no MoonLive backend" OFF)
if(MM_MOONLIVE_NO_HOST_JIT)
    add_compile_definitions(MM_MOONLIVE_FORCE_NO_HOST_JIT=1)
endif()

# `uv` is the project's Python launcher (see CLAUDE.md / moondeck/MoonDeck.md).
# The build invokes Python helpers for build_info.h generation and UI embedding;
# resolving them through `uv run python …` keeps Windows (where `python3` isn't
# on PATH) on the same path as macOS / Linux without a `find_package(Python3)`
# detour and without per-host scaffolding.
find_program(UV_EXECUTABLE NAMES uv REQUIRED
             HINTS "$ENV{USERPROFILE}/.local/bin" "$ENV{HOME}/.local/bin")

# Core library. Most modules ship header-only (single .h file with implementation
# inline). Core service modules that bridge to the platform (HTTP server,
# filesystem, network, …) ship as .h + .cpp per CLAUDE.md — list those .cpps
# here so both the application and the test binary link them.
add_library(mm_core STATIC
    src/core/module/Control.cpp
    src/core/util/JsonUtil.cpp
    src/core/util/ScratchBuffer.cpp
    src/core/system/HttpServerModule.cpp
    src/core/system/FilesystemModule.cpp
    src/core/system/FileManagerModule.cpp
    src/core/system/MqttModule.cpp
    src/core/util/sha256.cpp
    src/core/module/Scheduler.cpp
    src/core/moonlive/MoonLive.cpp
    src/core/moonlive/MoonLiveCompiler.cpp
    src/core/moonlive/MoonLiveSpill.cpp
)
target_include_directories(mm_core PUBLIC src/)
target_link_libraries(mm_core PUBLIC mm_platform)
# `add_dependencies(mm_core ui_embed)` is below, after the ui_embed target is defined.

# The computed semver (compute_version.py): the core for a stable tag, <core>-dev.<N> for a
# moving latest build. Only the release pipeline sets it (package_desktop.py --version); a local
# build falls through to build_info.h's #ifndef default (library.json). The same override the
# ESP32 build has (esp32/main/CMakeLists.txt), so a desktop binary reports the same precise
# version its release assets are named with, which is what lets the UI's update badge compare
# a running desktop against the `latest` channel at all.
if(MM_VERSION)
    add_compile_definitions(MM_VERSION=${MM_VERSION})
endif()

# Platform library (desktop). moonlive_emit.cpp is the desktop MoonLive backend (host-ISA
# codegen) — it lives here because emitted machine code is platform/ISA-specific.
add_library(mm_platform
    src/platform/desktop/platform_desktop.cpp
    src/platform/desktop/platform_desktop_audio.cpp
    src/platform/desktop/moonlive_emit_noarch.cpp
    src/platform/desktop/moonlive_asm_noarch.cpp
    src/platform/desktop/moonlive_emit_arm64.cpp
    src/platform/desktop/moonlive_emit_x86_64.cpp
    src/platform/desktop/moonlive_asm_arm64.cpp
    src/platform/desktop/moonlive_asm_x86_64.cpp
)
target_include_directories(mm_platform PUBLIC src/ src/platform/desktop/)
# Winsock for the desktop socket surface; IPHLPAPI for real Ethernet link state and negotiated
# speed (PanelCardDriver warns below gigabit, and a ColorLight card tears rather than erroring).
# Npcap is deliberately NOT linked: platform_desktop.cpp resolves wpcap.dll at run time so the
# build needs no SDK and the binary runs without it.
# `dl` on Linux: miniaudio (platform_desktop_audio.cpp) runtime-links ALSA/PulseAudio via
# dlopen, so no audio SDK is a build requirement, the same no-SDK rule as Npcap above.
# `winhttp` on Windows: the one outbound HTTPS call (MoonCloud). It is the OS's own client, in the
# SDK Visual Studio already installs, so Windows needs no libcurl and no vendored TLS. The other
# platforms use libcurl for the same reason, each taking the stack its OS ships.
target_link_libraries(mm_platform PUBLIC $<$<PLATFORM_ID:Windows>:ws2_32> $<$<PLATFORM_ID:Windows>:iphlpapi>
                                          $<$<PLATFORM_ID:Windows>:winhttp>
                                          $<$<PLATFORM_ID:Linux>:dl> $<$<PLATFORM_ID:Linux>:pthread>)

# The two libraries genuinely need each other: mm_core calls into the platform surface, and the
# platform's MoonLive backend calls back into core's target-independent IR passes (lowerWith ->
# spillToBudget, MoonLiveSpill.cpp). Declaring the cycle makes CMake repeat both archives on the
# link line until the symbols resolve. Apple's linker and MSVC search archives repeatedly and
# never noticed; GNU ld makes ONE left-to-right pass, so the reference from mm_platform back into
# mm_core is unresolved unless the cycle is declared.
# libcurl for the one outbound HTTPS call (MoonCloud Stats). The OS's own TLS rather than a
# vendored stack: present on macOS and every Linux distribution, and carrying its own certificate
# verification, so nothing about TLS lives in this repository.
#
# OPTIONAL on purpose. A machine without it still builds and MM_HAVE_CURL stays undefined, in which
# case httpsPost returns false and a report is simply not sent: a build must never fail over an
# opt-in statistic. The build prints which way it went, since "why did my device never report"
# would otherwise be invisible.
find_package(CURL QUIET)
if(CURL_FOUND)
    message(STATUS "libcurl ${CURL_VERSION_STRING}: MoonCloud reporting enabled")
    target_compile_definitions(mm_platform PRIVATE MM_HAVE_CURL)
    target_link_libraries(mm_platform PUBLIC CURL::libcurl)
elseif(DEFINED ENV{MM_PACKAGING} AND NOT WIN32)
    # A PACKAGED build is the one case where absent is not acceptable: the binary would ask for
    # consent and then be structurally unable to honor it, and nobody would find out. Optional
    # stays optional for a contributor's machine; a published artifact must be able to report.
    #
    # Keyed on MM_PACKAGING, which package_desktop.py exports and nothing else sets. Two earlier
    # signals were both wrong. MM_RELEASE reaches the build as a CMake -D from build_esp32.py and
    # is never an environment variable, so that branch could not fire and three of the four
    # published desktop binaries shipped with the feature compiled out. CI is set by every runner,
    # including the sanitizer lanes that configure CMake directly and legitimately have no libcurl,
    # so keying on it failed builds that produce no artifact at all.
    #
    # Windows is exempt, and for a reason that is now permanent rather than pending: it sends
    # through WinHTTP (httpsPost), so libcurl being absent there costs it nothing. Failing a
    # Windows build over a library it does not use would block a binary that reports perfectly.
    message(FATAL_ERROR
        "libcurl not found while packaging a release. A published build with the client compiled "
        "out asks for consent it can never honor. Install it in the job "
        "(Linux: libcurl4-openssl-dev, Windows: vcpkg install curl).")
else()
    message(STATUS "libcurl not found: MoonCloud reporting disabled (install libcurl to enable)")
endif()

target_link_libraries(mm_platform PUBLIC mm_core)
set_target_properties(mm_core     PROPERTIES LINK_INTERFACE_MULTIPLICITY 3)
set_target_properties(mm_platform PROPERTIES LINK_INTERFACE_MULTIPLICITY 3)

# Generate build_info.h from library.json + git (carries version, build id, build date, board name).
# ALWAYS out-of-date on purpose — see the same rule in esp32/main/CMakeLists.txt for why: MM_BUILD_ID
# must track the git hash on every build, and pinning this to library.json's mtime is what made the
# reported build stamp go stale, which reads as a failed flash and sends debugging at the wrong binary. The generator
# rewrites the header only when its content changes, so an unchanged tree triggers no rebuild.
add_custom_target(build_info_gen ALL
    COMMAND ${UV_EXECUTABLE} run python ${CMAKE_SOURCE_DIR}/moondeck/build/generate_build_info.py
    BYPRODUCTS ${CMAKE_SOURCE_DIR}/src/core/util/build_info.h
    COMMENT "Generating build_info.h (git build id)"
    VERBATIM
)

# Embed UI files as C arrays. Pass UV_EXECUTABLE through as a single value
# (no semicolons on the command line — those would either get split by `make`
# on POSIX hosts or land as literals in the value on MSBuild) and let
# embed_ui.cmake assemble the `<uv> run python` list internally.
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${CMAKE_SOURCE_DIR}/src/ui -DOUT=${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h -DUV_EXECUTABLE=${UV_EXECUTABLE} -P ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/index.html ${CMAKE_SOURCE_DIR}/src/ui/app.js ${CMAKE_SOURCE_DIR}/src/ui/style.css ${CMAKE_SOURCE_DIR}/src/ui/install-picker.js ${CMAKE_SOURCE_DIR}/src/ui/vendor/prism.js ${CMAKE_SOURCE_DIR}/src/ui/preview3d.js ${CMAKE_SOURCE_DIR}/src/ui/moonlight-logo.png ${CMAKE_SOURCE_DIR}/src/ui/embed_ui.cmake
    COMMENT "Embedding UI files"
)
add_custom_target(ui_embed DEPENDS ${CMAKE_SOURCE_DIR}/src/ui/ui_embedded.h)

# The MoonLive script catalog: names of every factory script, so the picker can offer the whole
# library while the device holds only what someone actually picked. Globbed, so the DEPENDS is the
# directory contents rather than a file list that would drift; CONFIGURE_DEPENDS re-globs when a
# script is added or removed.
file(GLOB MOONLIVE_SCRIPTS CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/moonlive/effects/*.mle
     ${CMAKE_SOURCE_DIR}/moonlive/layouts/*.mll
     ${CMAKE_SOURCE_DIR}/moonlive/modifiers/*.mlm
     ${CMAKE_SOURCE_DIR}/moonlive/services/*.mls
     ${CMAKE_SOURCE_DIR}/moonlive/palettes/*.mlp)
# The file LIST itself is a dependency, not just each file's timestamp: DEPENDS notices an edited
# script but not a DELETED one, so removing a script left it in the catalog and the device went on
# offering a name that no longer exists upstream. The stamp is written at configure time from the
# glob, so a removal changes it and the command re-runs.
string(REPLACE ";" "\n" MOONLIVE_SCRIPT_LIST "${MOONLIVE_SCRIPTS}")
set(MOONLIVE_STAMP ${CMAKE_BINARY_DIR}/moonlive_scripts.stamp)
file(CONFIGURE OUTPUT ${MOONLIVE_STAMP} CONTENT "${MOONLIVE_SCRIPT_LIST}\n")
add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/src/light/moonlive/script_catalog.h
    COMMAND ${CMAKE_COMMAND} -DSCRIPT_DIR=${CMAKE_SOURCE_DIR}/moonlive -DOUT=${CMAKE_SOURCE_DIR}/src/light/moonlive/script_catalog.h -DUV_EXECUTABLE=${UV_EXECUTABLE} -P ${CMAKE_SOURCE_DIR}/src/light/moonlive/catalog_scripts.cmake
    DEPENDS ${MOONLIVE_SCRIPTS} ${MOONLIVE_STAMP} ${CMAKE_SOURCE_DIR}/src/light/moonlive/catalog_scripts.cmake ${CMAKE_SOURCE_DIR}/src/light/moonlive/catalog_scripts.py
    COMMENT "Generating MoonLive script catalog"
)
add_custom_target(moonlive_catalog DEPENDS ${CMAKE_SOURCE_DIR}/src/light/moonlive/script_catalog.h)

# mm_core's HttpServerModule.cpp consumes ui_embedded.h and SystemModule.h
# (a mm_core public header) consumes build_info.h. Anything that links mm_core
# transitively includes those headers, so both generated files must exist
# before any mm_core compile unit starts. CI's parallel clean build exposes
# the race (test/ compiles SystemModule.h while build_info_gen is still
# running); wiring the dep here makes the ordering explicit on every consumer.
add_dependencies(mm_core ui_embed build_info_gen moonlive_catalog)

# Windows: give the exe its own icon, so it is recognizable in Explorer, the taskbar and the Start
# menu whether it was installed or just unzipped. Generated from the same mooninstaller/favicon.png
# the macOS .icns comes from, so the mark has one source rather than a checked-in binary per
# platform. Nothing here runs on macOS or Linux, which carry their icon in the .app and the .deb.
set(MM_WIN_RESOURCES "")
if(WIN32)
    set(MM_ICON "${CMAKE_BINARY_DIR}/projectMM.ico")
    set(MM_RC   "${CMAKE_BINARY_DIR}/projectMM.rc")
    add_custom_command(
        OUTPUT "${MM_ICON}"
        # `uv run <script>` rather than the usual `uv run python <script>`: make_ico.py declares
        # Pillow inline (PEP 723), and only the script form makes uv honor that declaration.
        COMMAND ${UV_EXECUTABLE} run "${CMAKE_SOURCE_DIR}/moondeck/ci/make_ico.py"
                "${CMAKE_SOURCE_DIR}/mooninstaller/favicon.png" "${MM_ICON}"
        DEPENDS "${CMAKE_SOURCE_DIR}/mooninstaller/favicon.png"
                "${CMAKE_SOURCE_DIR}/moondeck/ci/make_ico.py"
        COMMENT "Generating projectMM.ico from favicon.png"
        VERBATIM)
    # Forward slashes: rc.exe accepts them, and a backslash here would be an escape.
    file(WRITE "${MM_RC}"
         "// Generated by CMake from mooninstaller/favicon.png. Do not edit.\n"
         "IDI_ICON1 ICON \"${MM_ICON}\"\n")
    # The .ico rides along as a source so the custom command runs, but it is data, not something to
    # compile; the .rc recompiles whenever the icon is regenerated.
    set_source_files_properties("${MM_ICON}" PROPERTIES HEADER_FILE_ONLY TRUE)
    set_source_files_properties("${MM_RC}" PROPERTIES OBJECT_DEPENDS "${MM_ICON}")
    set(MM_WIN_RESOURCES "${MM_RC}" "${MM_ICON}")
endif()

# Application
# module_types.cpp is the shared type registry, compiled into the app and the scenario runner
# alike so one list serves both: src/module_types.cpp carries the reason.
add_executable(projectMM src/main.cpp src/module_types.cpp src/platform/desktop/main_desktop.cpp ${MM_WIN_RESOURCES})
target_link_libraries(projectMM PRIVATE mm_core mm_platform)
add_dependencies(projectMM ui_embed build_info_gen moonlive_catalog)

# Tests
enable_testing()
add_subdirectory(test)
