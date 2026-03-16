/**
 *   ESP32 SvelteKit
 *
 *   A simple, secure and extensible framework for IoT projects for ESP32 platforms
 *   with responsive Sveltekit front-end built with TailwindCSS and DaisyUI.
 *   https://github.com/theelims/ESP32-sveltekit
 *
 *   Copyright (C) 2018 - 2023 rjwats
 *   Copyright (C) 2023 - 2025 theelims
 *
 *   All Rights Reserved. This software may be modified and distributed under
 *   the terms of the LGPL v3 license. See the LICENSE file for details.
 **/

#include <EthernetSettingsService.h>

#if FT_ENABLED(FT_ETHERNET)

EthernetSettingsService::EthernetSettingsService(PsychicHttpServer *server,
                                                 FS *fs,
                                                 SecurityManager *securityManager,
                                                 EventSocket *socket) : _server(server),
                                                                        _securityManager(securityManager),
                                                                        _httpEndpoint(EthernetSettings::read, EthernetSettings::update, this, server, ETHERNET_SETTINGS_SERVICE_PATH, securityManager,
                                                                                      AuthenticationPredicates::IS_ADMIN),
                                                                        _fsPersistence(EthernetSettings::read, EthernetSettings::update, this, fs, ETHERNET_SETTINGS_FILE),
                                                                        _socket(socket)
{
    addUpdateHandler([&](const String &originId)
                     { reconfigureEthernet(); },
                     false);
}

void EthernetSettingsService::initEthernet()
{
    // 🌙 Disable WiFi BEFORE starting ethernet to free ~50KB heap.
    // EMAC DMA buffers are allocated during ETH.begin() — if WiFi is still running,
    // there may not be enough heap for both, causing "no mem for receive buffer" errors.
    // Use WiFi.mode(WIFI_OFF) only — it handles disconnect + stop + deinit in one call.
    // Do NOT call WiFi.disconnect(true) separately as double-deinit causes
    // "wifi_init_default: netstack cb reg failed" errors and leaks memory.
    if (WiFi.getMode() != WIFI_OFF) {
        ESP_LOGI(SVK_TAG, "Disabling WiFi before ethernet init to free heap (free: %lu)", (unsigned long)ESP.getFreeHeap());
        WiFi.mode(WIFI_OFF);
        _wifiDisabledByEthernet = true;
        ESP_LOGI(SVK_TAG, "WiFi disabled (free heap now: %lu)", (unsigned long)ESP.getFreeHeap());
    }

    // make sure the interface is stopped before continuing and initializing
    ETH.end();
    _fsPersistence.readFromFS();
    configureNetwork(_state.ethernetSettings);
}

void EthernetSettingsService::begin()
{
    _socket->registerEvent(EVENT_ETHERNET);
    _httpEndpoint.begin();
}

void EthernetSettingsService::loop()
{
    unsigned long currentMillis = millis();

    if (!_lastEthernetUpdate || (unsigned long)(currentMillis - _lastEthernetUpdate) >= ETHERNET_EVENT_DELAY)
    {
        _lastEthernetUpdate = currentMillis;
        updateEthernet();
    }
}

String EthernetSettingsService::getHostname()
{
    return _state.hostname;
}

String EthernetSettingsService::getIP()
{
    if (ETH.connected())
    {
        return ETH.localIP().toString();
    }
    return "Not connected";
}

void EthernetSettingsService::configureNetwork(ethernet_settings_t &network)
{
    // set hostname before IP configuration starts
    ETH.setHostname(_state.hostname.c_str());
    if (network.staticIPConfig)
    {
        // configure for static IP
        ETH.config(network.localIP, network.gatewayIP, network.subnetMask, network.dnsIP1, network.dnsIP2);
    }
    else
    {
        // configure for DHCP
        ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
// 🌙 (re)start ethernet — configured by ModuleIO board presets
    if (v_ETH_SPI_CONFIGURED) {
        // SPI Ethernet (W5500, ENC28J60 etc.) — available on all targets
        SPI.begin(v_ETH_SPI_SCK, v_ETH_SPI_MISO, v_ETH_SPI_MOSI);
        ETH.begin(v_ETH_PHY_TYPE, v_ETH_PHY_ADDR, v_ETH_PHY_CS, v_ETH_PHY_IRQ, v_ETH_PHY_RST, SPI);
    }
#if CONFIG_ETH_USE_ESP32_EMAC
    else if (v_ETH_RMII_CONFIGURED) {
        // RMII Ethernet (LAN8720A etc.) — ESP32/P4 with built-in EMAC
        ETH.begin(v_ETH_PHY_TYPE, v_ETH_PHY_ADDR, v_ETH_PHY_MDC, v_ETH_PHY_MDIO, v_ETH_PHY_POWER, v_ETH_CLK_MODE);
    }
#endif
    else {
        ETH.begin();
    }
    // set hostname (again) after (re)starting ethernet due to a bug in the ESP-IDF implementation
    ETH.setHostname(_state.hostname.c_str());
}

void EthernetSettingsService::reconfigureEthernet()
{
    configureNetwork(_state.ethernetSettings);
}

void EthernetSettingsService::updateEthernet()
{
    bool ethConnected = ETH.connected();

    // 🌙 Disable WiFi when ethernet is connected to free ~50KB heap (critical on ESP32-D0).
    // Re-enable WiFi when ethernet is lost so the device remains reachable.
    if (ethConnected && !_wifiDisabledByEthernet) {
        ESP_LOGI(SVK_TAG, "Ethernet connected — disabling WiFi to free heap (free: %lu)", (unsigned long)ESP.getFreeHeap());
        WiFi.mode(WIFI_OFF);
        _wifiDisabledByEthernet = true;
        ESP_LOGI(SVK_TAG, "WiFi disabled (free heap now: %lu)", (unsigned long)ESP.getFreeHeap());
    } else if (!ethConnected && _wifiDisabledByEthernet) {
        ESP_LOGI(SVK_TAG, "Ethernet lost — re-enabling WiFi");
        WiFi.mode(WIFI_STA);
        WiFi.begin();
        _wifiDisabledByEthernet = false;
    }

    JsonDocument doc;
    doc["connected"] = ethConnected;
    JsonObject jsonObject = doc.as<JsonObject>();
    _socket->emitEvent(EVENT_ETHERNET, jsonObject);
}

#endif // end FT_ENABLED(FT_ETHERNET)
