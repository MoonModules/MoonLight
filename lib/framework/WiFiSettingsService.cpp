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

#include <WiFiSettingsService.h>

#include <ESP32SvelteKit.h> // 🌙 safeMode

#include "esp_heap_caps.h"
#include "esp_dma_utils.h"

 // This tells the C++ compiler that the functions we're wrapping are C-style.
// This tells the C++ compiler that the function we're wrapping is C-style.
extern "C" {
  // Our WRAPPER function. It MUST keep the signature of the old function it's wrapping.
  esp_err_t __real_esp_dma_capable_malloc(size_t size, const esp_dma_mem_info_t* dma_mem_info, void** out_ptr, size_t* actual_size);

  // Our WRAPPER function. The linker will redirect all calls to the original function here.
  esp_err_t __wrap_esp_dma_capable_malloc(size_t size, const esp_dma_mem_info_t* dma_mem_info, void** out_ptr, size_t* actual_size) {
    
    void* ptr = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM, MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);

    // 3. Handle the result to match the old function's return style.
    if (!ptr) {
      // Allocation failed
      *out_ptr = NULL;
      return ESP_ERR_NO_MEM;
    }

    // Allocation succeeded
    *out_ptr = ptr;
    if (actual_size) {
      // The new function doesn't report the actual aligned size,
      // so we just return the requested size. This is usually fine.
      *actual_size = size;
    }
    
    return ESP_OK;
  }
}

WiFiSettingsService::WiFiSettingsService(PsychicHttpServer *server,
                                         FS *fs,
                                         SecurityManager *securityManager,
                                         EventSocket *socket) : _server(server),
                                                                _securityManager(securityManager),
                                                                _httpEndpoint(WiFiSettings::read, WiFiSettings::update, this, server, WIFI_SETTINGS_SERVICE_PATH, securityManager,
                                                                              AuthenticationPredicates::IS_ADMIN),
                                                                _fsPersistence(WiFiSettings::read, WiFiSettings::update, this, fs, WIFI_SETTINGS_FILE), _lastConnectionAttempt(0),
                                                                _socket(socket)
{
    addUpdateHandler([&](const String &originId)
                     { reconfigureWiFiConnection(); },
                     false);
}

void WiFiSettingsService::initWiFi()
{
    WiFi.mode(WIFI_MODE_STA); // this is the default.

    // Disable WiFi config persistance and auto reconnect
    WiFi.persistent(true);
    WiFi.setAutoReconnect(false);

    WiFi.onEvent(
        std::bind(&WiFiSettingsService::onStationModeDisconnected, this, std::placeholders::_1, std::placeholders::_2),
        WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent(std::bind(&WiFiSettingsService::onStationModeStop, this, std::placeholders::_1, std::placeholders::_2),
                 WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_STOP);

    _fsPersistence.readFromFS();
    reconfigureWiFiConnection();
}

void WiFiSettingsService::begin()
{
    _socket->registerEvent(EVENT_RSSI);

    _httpEndpoint.begin();
}

void WiFiSettingsService::reconfigureWiFiConnection()
{
    // reset last connection attempt to force loop to reconnect immediately
    _lastConnectionAttempt = 0;

    String connectionMode;

    switch (_state.staConnectionMode)
    {
    case (u_int8_t)STAConnectionMode::OFFLINE:
        connectionMode = "OFFLINE";
        break;
    case (u_int8_t)STAConnectionMode::PRIORITY:
        connectionMode = "PRIORITY";
        break;
    case (u_int8_t)STAConnectionMode::STRENGTH:
        connectionMode = "STRENGTH";
        break;
    default:
        connectionMode = "UNKNOWN";
        break;
    }

    ESP_LOGI(SVK_TAG, "Reconfiguring WiFi connection to: %s", connectionMode.c_str());

    ESP_LOGI(SVK_TAG, "Reconfiguring WiFi TxPower to: %d", _state.txPower); // 🌙

    // disconnect and de-configure wifi
    if (WiFi.isConnected() == true)
    {
        WiFi.disconnect(true);
        _stopping = true;
    }
}

void WiFiSettingsService::loop()
{
    unsigned long currentMillis = millis();
    if (!_lastConnectionAttempt || (unsigned long)(currentMillis - _lastConnectionAttempt) >= WIFI_RECONNECTION_DELAY)
    {
        _lastConnectionAttempt = currentMillis;
        manageSTA();
    }

    if (!_lastRssiUpdate || (unsigned long)(currentMillis - _lastRssiUpdate) >= RSSI_EVENT_DELAY)
    {
        _lastRssiUpdate = currentMillis;
        updateRSSI();
    }
}

String WiFiSettingsService::getHostname()
{
    return _state.hostname;
}

String WiFiSettingsService::getIP()
{
    if (WiFi.isConnected())
    {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}

void WiFiSettingsService::manageSTA()
{
    // Abort if already connected, if we have no SSID, or are in offline mode
    if (WiFi.isConnected() || _state.wifiSettings.empty() || _state.staConnectionMode == (u_int8_t)STAConnectionMode::OFFLINE)
    {
        return;
    }

    // Connect or reconnect as required
    if ((WiFi.getMode() & WIFI_STA) == 0)
    {
#ifdef SERIAL_INFO
        Serial.println("Connecting to WiFi...");
#endif
        connectToWiFi();
    }
}

void WiFiSettingsService::connectToWiFi()
{
    // reset availability flag for all stored networks
    for (auto &network : _state.wifiSettings)
    {
        network.available = false;
    }

    // scanning for available networks
    int scanResult = WiFi.scanNetworks();
    if (scanResult == WIFI_SCAN_FAILED)
    {
        ESP_LOGE(SVK_TAG, "WiFi scan failed.");
    }
    else if (scanResult == 0)
    {
        ESP_LOGW(SVK_TAG, "No networks found.");
    }
    else
    {
        ESP_LOGI(SVK_TAG, "%d networks found.", scanResult);

        // find the best network to connect
        wifi_settings_t *bestNetwork = NULL;
        int bestNetworkDb = FACTORY_WIFI_RSSI_THRESHOLD;

        for (int i = 0; i < scanResult; ++i)
        {
            String ssid_scan;
            int32_t rssi_scan;
            uint8_t sec_scan;
            uint8_t *BSSID_scan;
            int32_t chan_scan;

            WiFi.getNetworkInfo(i, ssid_scan, sec_scan, rssi_scan, BSSID_scan, chan_scan);
            ESP_LOGV(SVK_TAG, "SSID: %s, BSSID: " MACSTR ", RSSI: %d dbm, Channel: %d", ssid_scan.c_str(), MAC2STR(BSSID_scan), rssi_scan, chan_scan);

            for (auto &network : _state.wifiSettings)
            {
                if (ssid_scan.equals(network.ssid))
                { // SSID match
                    if (rssi_scan > bestNetworkDb)
                    { // best network
                        bestNetworkDb = rssi_scan;
                        ESP_LOGV(SVK_TAG, "--> New best network SSID: %s, BSSID: " MACSTR "", ssid_scan.c_str(), MAC2STR(BSSID_scan));
                        network.available = true;
                        network.channel = chan_scan;
                        memcpy(network.bssid, BSSID_scan, 6);
                        bestNetwork = &network;
                    }
                    else if (rssi_scan >= FACTORY_WIFI_RSSI_THRESHOLD && network.available == false)
                    { // available network
                        network.available = true;
                        network.channel = chan_scan;
                        memcpy(network.bssid, BSSID_scan, 6);
                    }
                    break;
                }
            }
        }

        // if configured to prioritize signal strength, use the best network else use the first available network
        // if (_state.priorityBySignalStrength == false)
        if (_state.staConnectionMode == (u_int8_t)STAConnectionMode::PRIORITY)
        {
            for (auto &network : _state.wifiSettings)
            {
                if (network.available == true)
                {
                    ESP_LOGI(SVK_TAG, "Connecting to first available network: %s", network.ssid.c_str());
                    configureNetwork(network);
                    break;
                }
            }
        }
        else if (_state.staConnectionMode == (u_int8_t)STAConnectionMode::STRENGTH && bestNetwork)
        {
            ESP_LOGI(SVK_TAG, "Connecting to strongest network: %s, BSSID: " MACSTR " ", bestNetwork->ssid.c_str(), MAC2STR(bestNetwork->bssid));
            configureNetwork(*bestNetwork);
        }
        else // no suitable network to connect
        {
            ESP_LOGI(SVK_TAG, "No known networks found.");
        }

        // delete scan results
        WiFi.scanDelete();
    }
}

void WiFiSettingsService::configureNetwork(wifi_settings_t &network)
{
    if (network.staticIPConfig)
    {
        // configure for static IP
        WiFi.config(network.localIP, network.gatewayIP, network.subnetMask, network.dnsIP1, network.dnsIP2);
    }
    else
    {
        // configure for DHCP
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }
    WiFi.setHostname(_state.hostname.c_str());

    // attempt to connect to the network
    WiFi.begin(network.ssid.c_str(), network.password.c_str(), network.channel, network.bssid);
    // WiFi.begin(network.ssid.c_str(), network.password.c_str());

    // 🌙 AP will use the LOLIN_WIFI_FIX, WiFi can set the power which works best for the board
    if (_state.txPower != 0 && _state.txPower != abs(WiFi.getTxPower())) { // abs to make 4 of -4 (WIFI_POWER_MINUS_1dBm)
        switch (_state.txPower)
        {
            case 84: WiFi.setTxPower(WIFI_POWER_21dBm); break;
            case 82: WiFi.setTxPower(WIFI_POWER_20_5dBm); break;
            case 80: WiFi.setTxPower(WIFI_POWER_20dBm); break;
            case 78: WiFi.setTxPower(WIFI_POWER_19_5dBm); break;
            case 76: WiFi.setTxPower(WIFI_POWER_19dBm); break;
            case 74: WiFi.setTxPower(WIFI_POWER_18_5dBm); break;
            case 68: WiFi.setTxPower(WIFI_POWER_17dBm); break;
            case 60: WiFi.setTxPower(WIFI_POWER_15dBm); break;
            case 52: WiFi.setTxPower(WIFI_POWER_13dBm); break;
            case 44: WiFi.setTxPower(WIFI_POWER_11dBm); break;
            case 34: WiFi.setTxPower(WIFI_POWER_8_5dBm); break;
            case 28: WiFi.setTxPower(WIFI_POWER_7dBm); break;
            case 20: WiFi.setTxPower(WIFI_POWER_5dBm); break;
            case 8: WiFi.setTxPower(WIFI_POWER_2dBm); break;
            case 4: WiFi.setTxPower(WIFI_POWER_MINUS_1dBm); break;
            case 0: break; //default, do not set the power
            default:
                ESP_LOGE(SVK_TAG, "Invalid txPower value: %d", _state.txPower);
                return;
        }
        ESP_LOGI(SVK_TAG, "WiFi setTxPower to: %d", _state.txPower);
    }
}

void WiFiSettingsService::updateRSSI()
{
    if (!_socket->getConnectedClients()) return;  // 🌙 No need for UI tasks

    JsonDocument doc;
    doc["rssi"] = WiFi.RSSI();
    doc["ssid"] = WiFi.isConnected() ? WiFi.SSID() : "disconnected";
    doc["safeMode"] = safeModeMB; // 🌙
    doc["restartNeeded"] = restartNeeded; // 🌙
    doc["saveNeeded"] = saveNeeded; // 🌙
    doc["hostName"] = getHostname(); // 🌙
    JsonObject jsonObject = doc.as<JsonObject>();
    _socket->emitEvent(EVENT_RSSI, jsonObject);
}

void WiFiSettingsService::onStationModeDisconnected(WiFiEvent_t event, WiFiEventInfo_t info)
{
    WiFi.disconnect(true);
}

void WiFiSettingsService::onStationModeStop(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (_stopping)
    {
        _lastConnectionAttempt = 0;
        _stopping = false;
    }
}
