# Core's sources, the one list every build reads: the desktop's mm_core, the ESP32 component and the Teensy firmware.
# Most of core is header-only; these are the modules that split interface from body (coding-standards.md).
set(MM_CORE_SOURCES
    module/Control.cpp
    module/Scheduler.cpp
    module/StateDocument.cpp
    util/JsonUtil.cpp
    util/build_info.cpp
    util/JsonFetch.cpp
    util/parse.cpp
    util/ScratchBuffer.cpp
    util/sha256.cpp
    system/HttpServerModule.cpp
    system/HttpServerState.cpp
    system/HttpServerWled.cpp
    system/FilesystemModule.cpp
    system/FileManagerModule.cpp
    system/MqttModule.cpp
    services/SdCardService.cpp
    moonlive/MoonLive.cpp
    moonlive/MoonLiveCompiler.cpp
    moonlive/MoonLiveSpill.cpp
)
list(TRANSFORM MM_CORE_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
