idf_component_register(
    SRCS "migrate_main.cpp"
    # WledNetworks.h and FirmwareImage.h, the headers shared with the app's tree: the same network reader a host test runs, and the NVS names MoonBase reads.
    PRIV_INCLUDE_DIRS "../../../src"
    EMBED_FILES "${MOONBASE_BIN}" "${TABLE_BIN}" "${BOOTLOADER_BIN}"
    REQUIRES esp_partition spi_flash app_update bootloader_support efuse nvs_flash joltwallet__littlefs)
# The LittleFS core writes the new filesystem straight onto the flash, so its header is needed beside the VFS one the component publishes.
idf_component_get_property(littlefs_dir joltwallet__littlefs COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${littlefs_dir}/src/littlefs")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
