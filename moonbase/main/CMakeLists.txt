idf_component_register(
    SRCS "moonbase_main.cpp"
    # The app's src/ is NOT on the include path: MoonBase shares no sources with the application,
    # which is the trade for an image that stays small and rarely changes. core/FirmwareImage.h is
    # the one exception, and earns it: a header-only parser of the ESP32 image format that depends
    # on nothing but <cstdint>. Both images must refuse to install the other, and that rule living
    # in two hand-written copies is how they drift.
    INCLUDE_DIRS "../../src"
    # The app's own logo, embedded IDF-natively; served as /logo.png (header + favicon).
    EMBED_FILES "../../src/ui/moonmodules-logo.png"
    # esp_https_ota brings esp_http_client and esp-tls with it, so neither is listed separately.
    # esp_eth: the RMII bring-up for boards whose config wires a PHY (ethStart).
    REQUIRES nvs_flash esp_netif esp_event esp_wifi esp_eth app_update esp_https_ota
             mbedtls esp_partition joltwallet__littlefs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
