idf_component_register(
    SRCS "moonbase_main.cpp"
    # MoonBase shares no sources with the application, which is the trade for an image that stays
    # small and rarely changes. Two headers are the exceptions, and earn it by depending on nothing
    # but the C library: core/util/FirmwareImage.h, since both images must refuse to install the
    # other, and core/util/ConfigScrape.h, since this image reads the app's config file. A rule
    # living in two hand-written copies is how they drift.
    INCLUDE_DIRS "../../src"
    # The app's own logo, embedded IDF-natively; served as /logo.png (header + favicon).
    EMBED_FILES "../../src/ui/moonmodules-logo.png"
    # esp_https_ota brings esp_http_client and esp-tls with it, so neither is listed separately.
    # esp_eth: the RMII bring-up for boards whose config wires a PHY (ethStart).
    REQUIRES nvs_flash esp_netif esp_event esp_wifi esp_eth app_update esp_https_ota
             mbedtls esp_partition joltwallet__littlefs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
