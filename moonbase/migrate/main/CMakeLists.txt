idf_component_register(
    SRCS "migrate_main.cpp"
    EMBED_FILES "${MOONBASE_BIN}" "${TABLE_BIN}"
    REQUIRES esp_partition spi_flash app_update joltwallet__littlefs)
# The LittleFS core writes the new filesystem straight onto the flash, so its header is needed beside the VFS one the component publishes.
idf_component_get_property(littlefs_dir joltwallet__littlefs COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${littlefs_dir}/src/littlefs")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
