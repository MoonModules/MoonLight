# The migration image: what a board running WLED, Tasmota or ESPHome installs through that firmware's own update page to move to MoonLight's layout without a cable.
# It carries MoonBase and the family's MoonLight partition tables with a bootloader beside each, one pair per flash size and type, so its build follows MoonBase's and the bootloaders': build_esp32.py passes their paths as MOONBASE_BIN, TABLE_BIN and BOOTLOADER_BIN.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# A compiler cache where one is installed, as the application's build does.
find_program(MM_CCACHE ccache)
if(MM_CCACHE)
    set(CCACHE_ENABLE 1)
endif()
project(MoonLight-migrate)
