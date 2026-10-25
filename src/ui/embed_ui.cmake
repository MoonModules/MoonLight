# Generate C header with the embedded UI files as byte arrays, bundled and minified first
# Usage: cmake -P embed_ui.cmake -DUI_DIR=src/ui -DOUT=src/ui/ui_embedded.h
#
# Text assets are gzipped at build time and served with Content-Encoding: gzip
# (HttpServerModule); the browser inflates natively, so the firmware carries the
# UI ~3.5x smaller with no device-side or front-end decompressor. Standard
# embedded-web-UI practice (WLED, ESPHome, ESP-IDF examples all pre-gzip assets).
# Compression runs on the BUILD HOST via Python's gzip module — stdlib, so any
# Python interpreter works (chosen over a `gzip` binary that Windows toolchains
# may lack). The PNG is already compressed, so it stays raw.
#
# Interpreter resolution — callers pick ONE of:
#   PYTHON_CMD       — a CMake list giving the exact argv prefix (ESP32 path
#                      passes `${Python3_EXECUTABLE}`). Wins if both are set.
#   UV_EXECUTABLE    — path to `uv`; built into `<uv> run python` here (desktop
#                      path). The list is built in-script so the desktop
#                      CMakeLists doesn't have to pass semicolons through
#                      MSBuild / make's command-line splitter.
# The two build entry points always pass one of these (root CMakeLists passes
# UV_EXECUTABLE via find_program(... REQUIRED); the ESP32 path passes PYTHON_CMD).
# A standalone `cmake -P` invocation that supplies neither is a configuration
# error — fail loudly here rather than fall back to a bare `python3` that doesn't
# exist on Windows (where it's `python`/`py`), which would surface as a cryptic
# mid-build "python3 not found" inside the gzip custom command.
if(DEFINED UV_EXECUTABLE AND NOT DEFINED PYTHON_CMD)
    set(PYTHON_CMD ${UV_EXECUTABLE} run python)
elseif(NOT DEFINED PYTHON_CMD)
    message(FATAL_ERROR
        "embed_ui.cmake: no Python interpreter. Pass -DUV_EXECUTABLE=<path-to-uv> "
        "or -DPYTHON_CMD=<interpreter>. (The normal build supplies one; this only "
        "fires on a standalone `cmake -P embed_ui.cmake` with neither set.)")
endif()

# The served files, bundled and minified from ${UI_DIR} by moondeck/build/bundle_ui.py into a stage under build/.
get_filename_component(REPO_DIR "${UI_DIR}/../.." ABSOLUTE)
set(STAGE_DIR "${REPO_DIR}/build/ui-bundle")
execute_process(
    COMMAND ${PYTHON_CMD} "${REPO_DIR}/moondeck/build/bundle_ui.py" "${UI_DIR}" "${STAGE_DIR}"
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "bundling the UI failed (PYTHON_CMD=${PYTHON_CMD} rc=${rc})")
endif()

# Gzip ${STAGE_DIR}/<name> to ${OUT_DIR}/<name>.gz and HEX-read it into OUT_VAR.
function(gzip_file_hex NAME OUT_VAR)
    set(src "${STAGE_DIR}/${NAME}")
    # Write the temp .gz next to the generated header (a writable build dir).
    get_filename_component(out_dir "${OUT}" DIRECTORY)
    set(gz "${out_dir}/${NAME}.gz")
    execute_process(
        COMMAND ${PYTHON_CMD} -c "import sys,gzip; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))" "${src}" "${gz}"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "gzip of ${src} failed (PYTHON_CMD=${PYTHON_CMD} rc=${rc})")
    endif()
    file(READ "${gz}" hex HEX)
    file(REMOVE "${gz}")
    set(${OUT_VAR} "${hex}" PARENT_SCOPE)
endfunction()

# Convert hex string to C array initializer
function(hex_to_c_array HEX_STR VAR_NAME OUT_VAR)
    string(LENGTH "${HEX_STR}" HEX_LEN)
    set(result "")
    math(EXPR last "${HEX_LEN} - 1")
    foreach(i RANGE 0 ${last} 2)
        string(SUBSTRING "${HEX_STR}" ${i} 2 byte)
        if(result)
            string(APPEND result ",")
        endif()
        string(APPEND result "0x${byte}")
    endforeach()
    set(${OUT_VAR} "${result}" PARENT_SCOPE)
endfunction()

# Every file the firmware serves, from the stage: `app.js` holds every module it imports. Text is gzipped; the PNG is already compressed and stays raw.
set(UI_FILES index.html app.js style.css prism.js momo.png)

set(arrays "")
set(entries "")
set(i 0)
foreach(f ${UI_FILES})
    get_filename_component(name "${f}" NAME)
    get_filename_component(ext "${f}" LAST_EXT)
    if(ext STREQUAL ".png")
        file(READ "${STAGE_DIR}/${f}" hex HEX)
        set(gzipped false)
        set(type "image/png")
    else()
        gzip_file_hex("${f}" hex)
        set(gzipped true)
        if(ext STREQUAL ".html")
            set(type "text/html")
        elseif(ext STREQUAL ".css")
            set(type "text/css")
        else()
            set(type "application/javascript")
        endif()
    endif()
    # A validator for conditional requests (RFC 9110 section 8.8.3): the served bytes' hash, so a reload of an unchanged file costs a 304. The gzip carries no timestamp (mtime=0), so the same source gives the same bytes and tag in every build.
    string(SHA1 sha "${hex}")
    string(SUBSTRING "${sha}" 0 16 etag)
    hex_to_c_array("${hex}" "file${i}" array)
    string(LENGTH "${hex}" hex_len)
    math(EXPR len "${hex_len} / 2")
    string(APPEND arrays "constexpr uint8_t file${i}[] = {${array}};\n")
    string(APPEND entries "    {\"${name}\", \"${type}\", file${i}, ${len}, ${gzipped}, \"\\\"${etag}\\\"\"},\n")
    math(EXPR i "${i} + 1")
endforeach()

file(WRITE "${OUT}" "#pragma once\n\n")
file(APPEND "${OUT}" "/// Every file the web UI serves, as byte arrays in one table: text gzipped, already-compressed binaries raw.\n")
file(APPEND "${OUT}" "///\n")
file(APPEND "${OUT}" "/// Generated by src/ui/embed_ui.cmake from its UI_FILES list: do not edit, rebuild to update.\n")
file(APPEND "${OUT}" "///\n")
file(APPEND "${OUT}" "/// @technical\n")
file(APPEND "${OUT}" "///\n")
file(APPEND "${OUT}" "/// The HTTP server answers from flash with no filesystem involved, so a device serves its UI before any filesystem is mounted, and a file cannot go missing.\n")
file(APPEND "${OUT}" "#include <cstdint>\n#include <cstddef>\nnamespace mm::ui {\n")
file(APPEND "${OUT}" "${arrays}")
file(APPEND "${OUT}" "/// One served file: its name under `/`, its content type, its bytes, whether they are gzipped, and its quoted entity tag.\n")
file(APPEND "${OUT}" "struct EmbeddedFile { const char* name; const char* contentType; const uint8_t* data; size_t len; bool gzipped; const char* etag; };\n")
file(APPEND "${OUT}" "/// Every file the UI serves, in the order embed_ui.cmake lists them.\n")
file(APPEND "${OUT}" "constexpr EmbeddedFile kFiles[] = {\n${entries}};\n")
file(APPEND "${OUT}" "} // namespace mm::ui\n")
