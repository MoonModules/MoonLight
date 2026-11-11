idf_component_register(
    SRCS "moonbase_main.cpp"
    # MoonBase shares no source files with the application, the trade for an image that stays small and rarely changes.
    # These headers are the exceptions, each depending on nothing but the C library, core/util/hex.h and the platform/nonblocking.h marker, since a rule in two hand-written copies drifts:
    # core/util/FirmwareImage.h (both images refuse to install the other), core/util/ConfigScrape.h (this image reads the app's config file), core/util/Ipv4.h (both refuse the same static addresses), core/util/CaptivePortal.h (both access points answer a phone the same way), core/util/DeviceName.h (both name an unnamed board alike), core/util/HttpHead.h (both read a request head alike) and core/system/WledPacket.h (both announce themselves to the Devices map in one packet).
    INCLUDE_DIRS "../../src"
    # The app's own logo, embedded IDF-natively; served as /logo.png (header + favicon).
    EMBED_FILES "../../src/ui/momo.png"
    # esp_https_ota brings esp_http_client and esp-tls with it, so neither is listed separately.
    # esp_eth: the RMII bring-up for boards whose config wires a PHY (ethStart).
    REQUIRES nvs_flash esp_netif esp_event esp_wifi esp_eth app_update esp_https_ota
             mbedtls esp_partition joltwallet__littlefs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
