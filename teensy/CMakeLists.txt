# The Teensy 4.1 firmware: the shared application on src/platform/teensy, linked with PJRC's Teensy 4 core.
# Configured by moondeck/build/build_teensy.py with -DCMAKE_TOOLCHAIN_FILE=teensy/teensy41.cmake.

cmake_minimum_required(VERSION 3.20)
project(MoonLightTeensy C CXX ASM)

set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(CORE "${TEENSY_SDK}/cores/teensy4")

find_program(UV_EXECUTABLE NAMES uv REQUIRED HINTS "$ENV{HOME}/.local/bin")

# The SDK's include paths are SYSTEM, so the application's -Werror covers MoonLight's code and not the vendors' headers.
# PJRC's core: startup, USB serial, timing, and main(), which calls setup() and loop().
file(GLOB CORE_SOURCES "${CORE}/*.c" "${CORE}/*.cpp" "${CORE}/*.S")
add_library(teensy_core STATIC ${CORE_SOURCES})
target_include_directories(teensy_core SYSTEM PUBLIC "${CORE}")
target_compile_options(teensy_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fpermissive -Wno-error=narrowing>)

# LittleFS for the settings, with the SPI library its header includes.
file(GLOB LITTLEFS_SOURCES "${TEENSY_SDK}/LittleFS/src/*.cpp" "${TEENSY_SDK}/LittleFS/src/littlefs/*.c")
add_library(teensy_libs STATIC ${LITTLEFS_SOURCES} "${TEENSY_SDK}/SPI/SPI.cpp")
target_include_directories(teensy_libs SYSTEM PUBLIC "${TEENSY_SDK}/LittleFS/src" "${TEENSY_SDK}/SPI")
target_link_libraries(teensy_libs PUBLIC teensy_core)
target_compile_options(teensy_libs PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# SD over PJRC's SdFat for the built-in microSD socket, a library of its own so it runs from flash like the application: it never writes program flash, and in RAM it takes the heap's room.
file(GLOB_RECURSE SDFAT_SOURCES "${TEENSY_SDK}/SdFat/src/*.cpp")
add_library(teensy_sd STATIC ${SDFAT_SOURCES} "${TEENSY_SDK}/SD/src/SD.cpp")
target_include_directories(teensy_sd SYSTEM PUBLIC "${TEENSY_SDK}/SD/src" "${TEENSY_SDK}/SdFat/src")
target_link_libraries(teensy_sd PUBLIC teensy_libs)
target_compile_options(teensy_sd PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# Which binary this is, set by build_teensy.py from the firmware registry, which names and checks the variants.
set(MM_FIRMWARE "teensy41" CACHE STRING "the firmware name the binary reports")

# The network stack: lwIP with QNEthernet, carrying both its drivers, the board's own PHY and a W5500 on the hardware SPI port and chip select 10.
# QNEthernet builds one driver, so each is compiled with its `driver` namespace renamed, and our external driver (its hook) picks the chip the board has at boot.
file(GLOB_RECURSE QNETHERNET_SOURCES "${TEENSY_SDK}/QNEthernet/src/*.c" "${TEENSY_SDK}/QNEthernet/src/*.cpp")
add_library(qnethernet STATIC ${QNETHERNET_SOURCES} ${REPO}/src/platform/teensy/qnethernet/qnethernet_dual_driver.cpp)
target_include_directories(qnethernet SYSTEM PUBLIC "${TEENSY_SDK}/QNEthernet/src" "${REPO}/src/platform/teensy/qnethernet")
set_source_files_properties("${TEENSY_SDK}/QNEthernet/src/qnethernet/drivers/driver_teensy41.cpp"
    PROPERTIES COMPILE_DEFINITIONS "QNETHERNET_INTERNAL_DRIVER_TEENSY41;driver=driver_t41")
set_source_files_properties("${TEENSY_SDK}/QNEthernet/src/qnethernet/drivers/driver_w5500.cpp"
    PROPERTIES COMPILE_DEFINITIONS "QNETHERNET_INTERNAL_DRIVER_W5500;driver=driver_w5500")
# The preview drains one send buffer per 20 ms tick, so the buffer bounds its rate: 16 segments, with the segment pool and lwIP's heap to match.
target_compile_definitions(qnethernet PUBLIC TCP_SND_BUF=23360 MEMP_NUM_TCP_SEG=64 MEM_SIZE=48000)
target_link_libraries(qnethernet PUBLIC teensy_libs)
target_compile_options(qnethernet PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# Generated headers, the same steps the desktop and ESP32 builds run.
add_custom_target(build_info_gen ALL
    COMMAND ${UV_EXECUTABLE} run python ${REPO}/moondeck/build/generate_build_info.py
    BYPRODUCTS ${REPO}/src/core/util/build_info.h
    WORKING_DIRECTORY ${REPO}
    COMMENT "Generating build_info.h (git build id)"
    VERBATIM)
add_custom_command(
    OUTPUT ${REPO}/src/ui/ui_embedded.h
    COMMAND ${CMAKE_COMMAND} -DUI_DIR=${REPO}/src/ui -DOUT=${REPO}/src/ui/ui_embedded.h -DUV_EXECUTABLE=${UV_EXECUTABLE} -P ${REPO}/src/ui/embed_ui.cmake
    DEPENDS ${REPO}/src/ui/index.html ${REPO}/src/ui/app.js ${REPO}/src/ui/style.css ${REPO}/src/ui/embed_ui.cmake
    COMMENT "Embedding UI files")
add_custom_target(ui_embed DEPENDS ${REPO}/src/ui/ui_embedded.h)
file(GLOB MOONLIVE_SCRIPTS CONFIGURE_DEPENDS
     ${REPO}/moonlive/effects/*.mle ${REPO}/moonlive/layouts/*.mll ${REPO}/moonlive/modifiers/*.mlm
     ${REPO}/moonlive/services/*.mls ${REPO}/moonlive/palettes/*.mlp)
set(MOONLIVE_CATALOG_STAMP ${CMAKE_BINARY_DIR}/moonlive_catalog.stamp)
add_custom_command(
    OUTPUT ${MOONLIVE_CATALOG_STAMP}
    COMMAND ${CMAKE_COMMAND} -DSCRIPT_DIR=${REPO}/moonlive -DOUT=${REPO}/src/core/moonlive/script_catalog.h -DUV_EXECUTABLE=${UV_EXECUTABLE} -P ${REPO}/src/core/moonlive/catalog_scripts.cmake
    COMMAND ${CMAKE_COMMAND} -E touch ${MOONLIVE_CATALOG_STAMP}
    DEPENDS ${MOONLIVE_SCRIPTS} ${REPO}/src/core/moonlive/catalog_scripts.cmake ${REPO}/src/core/moonlive/catalog_scripts.py
    COMMENT "Generating MoonLive script catalog")
add_custom_target(moonlive_catalog DEPENDS ${MOONLIVE_CATALOG_STAMP})

# PJRC's linker script copies all code into ITCM and all constants into DTCM, which a 1 MB application does not fit.
# The application's code and constants execute from flash through the cache instead (XIP); the core, its libraries and libc stay in RAM, since a flash write must not run from flash.
set(RAM_RESIDENT "*libteensy_core.a:* *libteensy_libs.a:* *libqnethernet.a:* *libc.a:* *libm.a:* *libgcc.a:*")
file(READ "${CORE}/imxrt1062_t41.ld" LINKER_SCRIPT)
string(FIND "${LINKER_SCRIPT}" "*(.progmem*)" ANCHOR)
if(ANCHOR EQUAL -1)
    message(FATAL_ERROR "imxrt1062_t41.ld has no .progmem section to extend; the Teensy core changed its linker script")
endif()
string(REPLACE "*(.progmem*)" "*(.progmem*)\n\t\t*(EXCLUDE_FILE(${RAM_RESIDENT}) .text*)\n\t\t*(EXCLUDE_FILE(${RAM_RESIDENT}) .rodata*)" LINKER_SCRIPT "${LINKER_SCRIPT}")
# The unwind table cannot span ITCM at 0 and flash at 0x60000000 (31-bit offsets), and nothing unwinds without exceptions, so it is dropped.
string(FIND "${LINKER_SCRIPT}" "\t.text.itcm : {" ANCHOR)
if(ANCHOR EQUAL -1)
    message(FATAL_ERROR "imxrt1062_t41.ld has no .text.itcm section to put the unwind discard before; the Teensy core changed its linker script")
endif()
string(REPLACE "\t.text.itcm : {" "\t/DISCARD/ : {\n\t\t*(.ARM.exidx* .ARM.extab* .gnu.linkonce.armexidx.*)\n\t}\n\n\t.text.itcm : {" LINKER_SCRIPT "${LINKER_SCRIPT}")
file(WRITE "${CMAKE_BINARY_DIR}/imxrt1062_t41.ld" "${LINKER_SCRIPT}")

# The application: the same sources the ESP32 build lists, on the Teensy platform layer.
add_executable(MoonLight.elf
    main.cpp
    ${REPO}/src/main.cpp
    ${REPO}/src/module_types.cpp
    ${REPO}/src/core/module/Control.cpp
    ${REPO}/src/core/util/JsonUtil.cpp
    ${REPO}/src/core/util/parse.cpp
    ${REPO}/src/core/util/ScratchBuffer.cpp
    ${REPO}/src/core/system/HttpServerModule.cpp
    ${REPO}/src/core/system/FilesystemModule.cpp
    ${REPO}/src/core/system/FileManagerModule.cpp
    ${REPO}/src/core/services/SdCardService.cpp
    ${REPO}/src/core/system/MqttModule.cpp
    ${REPO}/src/core/module/Scheduler.cpp
    ${REPO}/src/core/module/StateDocument.cpp
    ${REPO}/src/core/util/sha256.cpp
    ${REPO}/src/core/moonlive/MoonLive.cpp
    ${REPO}/src/core/moonlive/MoonLiveCompiler.cpp
    ${REPO}/src/core/moonlive/MoonLiveSpill.cpp
    ${REPO}/src/platform/desktop/moonlive_emit_noarch.cpp
    ${REPO}/src/platform/teensy/moonlive_asm_thumb2.cpp
    ${REPO}/src/platform/teensy/platform_teensy.cpp
    ${REPO}/src/platform/teensy/platform_teensy_fs.cpp
    ${REPO}/src/platform/teensy/platform_teensy_gpio_dma.cpp
    ${REPO}/src/platform/teensy/platform_teensy_ota.cpp
    ${REPO}/src/platform/teensy/platform_teensy_improv.cpp
)
add_dependencies(MoonLight.elf build_info_gen ui_embed moonlive_catalog)
target_include_directories(MoonLight.elf PRIVATE ${REPO}/src ${REPO}/src/platform/teensy)
target_compile_options(MoonLight.elf PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20> -Wall -Wextra -Werror)
target_compile_definitions(MoonLight.elf PRIVATE MM_FIRMWARE_NAME="${MM_FIRMWARE}")
# A release passes its channel and version; empty leaves build_info.h's defaults, as on the ESP32.
if(MM_RELEASE)
    target_compile_definitions(MoonLight.elf PRIVATE MM_RELEASE="${MM_RELEASE}")
endif()
if(MM_VERSION)
    target_compile_definitions(MoonLight.elf PRIVATE MM_VERSION="${MM_VERSION}")
endif()
target_link_libraries(MoonLight.elf PRIVATE qnethernet teensy_sd teensy_libs teensy_core m stdc++)
target_link_options(MoonLight.elf PRIVATE "-T${CMAKE_BINARY_DIR}/imxrt1062_t41.ld")

# The HEX Teensy Loader writes, and the same image flat for the writers that take raw bytes at an offset: from the HEX, which holds only the flash records at 0x60000000, and gap-filled with 0xFF, what erased flash reads.
add_custom_command(TARGET MoonLight.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex -R .eeprom MoonLight.elf MoonLight.hex
    COMMAND ${CMAKE_OBJCOPY} -I ihex -O binary --gap-fill 0xff MoonLight.hex MoonLight.bin
    COMMAND ${CMAKE_SIZE} -A MoonLight.elf
    COMMENT "MoonLight.hex, MoonLight.bin")
