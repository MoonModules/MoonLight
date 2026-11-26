# The light domain's sources, the one list every build reads: the host's mm_light, the ESP32 component and the Teensy firmware.
# The light domain is header-only except where a file splits interface from body (coding-standards.md, File shape).
set(MM_LIGHT_SOURCES
    layers/Layer.cpp
    drivers/DriverBase.cpp
    drivers/Drivers.cpp
)
list(TRANSFORM MM_LIGHT_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/")
