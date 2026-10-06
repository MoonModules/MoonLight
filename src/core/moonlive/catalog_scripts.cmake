# Generate the MoonLive script catalog: a header naming every factory script.
# Usage: cmake -P catalog_scripts.cmake -DSCRIPT_DIR=<repo>/moonlive -DOUT=<path>/script_catalog.h
#
# NAMES, not contents. A name costs ~12 bytes where a script costs ~800, so a device can carry the
# whole catalog (3 KB even at ten times today's library) and fetch a script's text the first time
# someone picks it. Flash then scales with how many scripts exist and the filesystem with how many
# are actually used, which matters because a device uses a handful: one layout describes the rig it
# is wired to and the rest are meaningless on it.
#
# Unlike embed_ui.cmake, which names each file it embeds, this GLOBS: the UI is a fixed set, the
# library is a set that grows, and a script added to moonlive/ must reach devices without anyone
# remembering to edit a CMake list.
#
# Interpreter resolution mirrors embed_ui.cmake (PYTHON_CMD wins, else UV_EXECUTABLE), so both build
# entry points pass what they already pass for the UI.
if(DEFINED UV_EXECUTABLE AND NOT DEFINED PYTHON_CMD)
    set(PYTHON_CMD ${UV_EXECUTABLE} run python)
elseif(NOT DEFINED PYTHON_CMD)
    message(FATAL_ERROR
        "catalog_scripts.cmake: no Python interpreter. Pass -DUV_EXECUTABLE=<path-to-uv> "
        "or -DPYTHON_CMD=<interpreter>.")
endif()

# The repo keeps the three roles in their own folders; the DEVICE keeps one flat directory and
# carries the role in the extension. The folder survives into the catalog only because it is part of
# the fetch URL.
file(GLOB SCRIPT_PATHS
     "${SCRIPT_DIR}/effects/*.mle"
     "${SCRIPT_DIR}/layouts/*.mll"
     "${SCRIPT_DIR}/modifiers/*.mlm"
     "${SCRIPT_DIR}/services/*.mls"
     "${SCRIPT_DIR}/palettes/*.mlp")
list(SORT SCRIPT_PATHS)   # deterministic output: the same input must give a byte-identical header

list(LENGTH SCRIPT_PATHS SCRIPT_COUNT)
if(SCRIPT_COUNT EQUAL 0)
    message(FATAL_ERROR
        "catalog_scripts.cmake: no scripts found under ${SCRIPT_DIR}. An empty catalog would ship a "
        "device with an empty library and no error, so this is a build failure.")
endif()

# The paths go through a file rather than the command line: a few hundred of them would overrun the
# command-length limit on Windows long before the library stops growing.
string(REPLACE ";" "\n" SCRIPT_LIST "${SCRIPT_PATHS}")
set(LIST_FILE "${OUT}.filelist")
file(WRITE "${LIST_FILE}" "${SCRIPT_LIST}\n")

execute_process(
    COMMAND ${PYTHON_CMD} "${CMAKE_CURRENT_LIST_DIR}/catalog_scripts.py" "${LIST_FILE}" "${OUT}"
    RESULT_VARIABLE rc)
file(REMOVE "${LIST_FILE}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "catalog_scripts.py failed (PYTHON_CMD=${PYTHON_CMD} rc=${rc})")
endif()

message(STATUS "MoonLive catalog: ${SCRIPT_COUNT} scripts")
