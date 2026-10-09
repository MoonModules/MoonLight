# The migration image: what a board running WLED installs through WLED's own update page to move to MoonLight's layout without a cable.
# It carries MoonBase and MoonLight's partition table, so its build follows theirs: build_esp32.py passes their paths as MOONBASE_BIN and TABLE_BIN.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MoonLight-migrate)
