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
    // Only on non-PSRAM boards (ESP32-D0) where heap is tight.
    // Skip on PSRAM boards for two reasons:
    //  1. Plenty of heap — WiFi and ethernet can coexist.
    //  2. On ESP32-P4 with WiFi coprocessor (ESP-Hosted), WiFi.mode(WIFI_OFF) deinits
    //     the coprocessor, but any subsequent WiFi API call (even WiFi.getMode() in
    //     updateEthernet()) triggers a full coprocessor reinit — creating an infinite
    //     deinit/reinit loop every 500ms. On D0 this doesn't happen because WiFi is
    //     built-in and getMode() is just reading a variable with no side effects.
    //  Note: ESP32-P4-ETH (FT_WIFI=0) has no WiFi at all — this code is safely skipped
    //  via the psramFound() guard since all P4 boards have PSRAM.
    // Use WiFi.mode(WIFI_OFF) only — it handles disconnect + stop + deinit in one call.
    // Do NOT call WiFi.disconnect(true) separately as double-deinit causes
    // "wifi_init_default: netstack cb reg failed" errors and leaks memory.
    if (!psramFound() && WiFi.getMode() != WIFI_OFF) {
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
    // 🌙 Use system hostname (unified across WiFi/Ethernet) when available, otherwise own
    String hostname = systemHostnameProvider ? systemHostnameProvider() : _state.hostname;
    ETH.setHostname(hostname.c_str());
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
        // Fallback for boards with built-in ethernet that works with default ETH.begin()
        ETH.begin();
    }
    // set hostname (again) after (re)starting ethernet due to a bug in the ESP-IDF implementation
    ETH.setHostname(hostname.c_str());
}

void EthernetSettingsService::reconfigureEthernet()
{
    configureNetwork(_state.ethernetSettings);
}

void EthernetSettingsService::updateEthernet()
{
    bool ethConnected = ETH.connected();

    // 🌙 Disable WiFi when ethernet is connected to free ~50KB heap (ESP32-D0 only).
    // Skip on PSRAM boards — plenty of heap; on P4 with coprocessor WiFi, calling
    // WiFi.mode(WIFI_OFF) then WiFi.getMode() reinits ESP-Hosted in an infinite loop.
    // On P4-ETH (FT_WIFI=0) there is no WiFi — safely skipped via psramFound().
    if (!psramFound() && ethConnected && WiFi.getMode() != WIFI_OFF) {
        ESP_LOGI(SVK_TAG, "Ethernet connected — disabling WiFi to free heap (free: %lu)", (unsigned long)ESP.getFreeHeap());
        WiFi.mode(WIFI_OFF);
        _wifiDisabledByEthernet = true;
        ESP_LOGI(SVK_TAG, "WiFi disabled (free heap now: %lu)", (unsigned long)ESP.getFreeHeap());
    } else if (!ethConnected && _wifiDisabledByEthernet) {
        // 🌙 Just clear the flag — WiFiSettingsService::manageSTA() will handle
        // reconnection with proper credentials on its next loop iteration.
        // Don't call WiFi.begin() here: WiFi.persistent(false) means credentials
        // aren't stored, so WiFi.begin() without SSID/password would fail.
        ESP_LOGI(SVK_TAG, "Ethernet lost — allowing WiFi reconnection");
        _wifiDisabledByEthernet = false;
    }

    JsonDocument doc;
    doc["connected"] = ethConnected;
    JsonObject jsonObject = doc.as<JsonObject>();
    _socket->emitEvent(EVENT_ETHERNET, jsonObject);
}

#endif // end FT_ENABLED(FT_ETHERNET)
