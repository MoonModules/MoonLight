cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# A compiler cache where one is installed, ESP-IDF's own switch turned on rather than left to an idf.py flag.
find_program(MM_CCACHE ccache)
if(MM_CCACHE)
    set(CCACHE_ENABLE 1)
endif()
project(MoonLight)   # the ESP-IDF descriptor a shipped device checks, the first of kAppImageNames in FirmwareImage.h
