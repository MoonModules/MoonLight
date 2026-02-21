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

WiFiSettingsService::WiFiSettingsService(PsychicHttpServer *server,
                                         FS *fs,
                                         SecurityManager *securityManager,
                                         EventSocket *socket) : _server(server),
                                                                _securityManager(securityManager),
                                                                _httpEndpoint(WiFiSettings::read, WiFiSettings::update, this, server, WIFI_SETTINGS_SERVICE_PATH, securityManager,
                                                                              AuthenticationPredicates::IS_ADMIN),
                                                                _fsPersistence(WiFiSettings::read, WiFiSettings::update, this, fs, WIFI_SETTINGS_FILE),
                                                                _lastConnectionAttempt(0),
                                                                _delayedReconnectTime(0),
                                                                _delayedReconnectPending(false),
                                                                _analyticsSent(false),
                                                                _socket(socket)
{
    addUpdateHandler([&](const String &originId)
                     { delayedReconnect(); },
                     false);
}

void WiFiSettingsService::initWiFi()
{
    WiFi.mode(WIFI_MODE_STA); // this is the default.

    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);   // 🌙 from WLED-MM bugfix: ensure that all channels are scanned, and the strongest signal is used, see https://github.com/wled/WLED/pull/5351 and https://github.com/MoonModules/WLED-MM/commit/812c5ca31532c741cb45b83d856c102a37877ca4

    // Disable WiFi config persistance and auto reconnect
    #ifndef CONFIG_IDF_TARGET_ESP32P4
        WiFi.persistent(false);
    #else
        WiFi.persistent(true);
    #endif
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
    _socket->registerEvent(EVENT_RECONNECT);

    _httpEndpoint.begin();
}

void WiFiSettingsService::delayedReconnect()
{
    _delayedReconnectTime = millis() + DELAYED_RECONNECT_MS;
    _delayedReconnectPending = true;
    ESP_LOGI(SVK_TAG, "Delayed WiFi reconnection scheduled in %d ms", DELAYED_RECONNECT_MS);

    // Emit event to notify clients of impending reconnection
    JsonDocument doc;
    doc["delay_ms"] = DELAYED_RECONNECT_MS;
    JsonObject jsonObject = doc.as<JsonObject>();
    _socket->emitEvent(EVENT_RECONNECT, jsonObject);
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

    ESP_LOGI(SVK_TAG, "Reconfiguring WiFi TxPower to: %d", _state.txPower?_state.txPower:-1); // 🌙

    ESP_LOGI(SVK_TAG, "Hostname: %s", _state.hostname.c_str()); // 🌙

    // disconnect and de-configure wifi
    // 🌙 add different behavior for P4
    #ifndef CONFIG_IDF_TARGET_ESP32P4
        //default ESP32-Sveltekit
        if (WiFi.disconnect(true))
        {
            _stopping = true;
        }
        //logging: 
        // [0;32mI (5079) 🐼: Reconfiguring WiFi connection to: STRENGTH
        // I (5079) 🐼: Reconfiguring WiFi TxPower to: 34
        // [0;32mI (5079) 🐼: Hostname: ml-you35
        // ...
        // [0;32mI (16438) 🐼: 20 networks found.
        // [0;32mI (16439) 🐼: Connecting to strongest network: ewtr, BSSID: 92:5a:9e:0e:cc:e4 
        // D (16440) 🐼: Connecting to SSID: ewtr, Channel: 11, BSSID: 92:5a:9e:0e:cc:e4, Hostname: ml-you35
        // [0;32mI (16451) 🐼: WiFi setTxPower to: 34
        // [0;32mI (16612) 🐼: WiFi Connected.
        // ...
        // [0;32mI (18124) 🐼: WiFi Got IP. localIP=http://192.168.1.105, hostName=http://ml-you35.local

    #else
        //by @troyhacks, needed to make P4 networking work (AP and STA)
        //no clue why? this only disconnects when WiFi is connected, the other always disconnects.
        if (WiFi.isConnected() == true)
        {
            if (!WiFi.disconnect(true))
                ESP_LOGW(SVK_TAG, "Failed to disconnect WiFi");
            else
                ESP_LOGD(SVK_TAG, "Successfully disconnect WiFi");
            _stopping = true;
        }
        //logging
        // Version on Host is NEWER than version on co-processor
        // E (7253) system_api: 0 mac type is incorrect (not found)
        // mI (7254) 🐼: Reconfiguring WiFi connection to: STRENGTH
        // [0;32mI (7254) 🐼: Reconfiguring WiFi TxPower to: 0
        // 32mI (7259) 🐼: Hostname: ml-P4
        // ...
        // [0;32mI (19737) 🐼: WiFi Connected.
        // [0;32mI (20761) 🐼: WiFi Got IP. localIP=http://192.168.1.188, hostName=http://esp32p4-E1E3E7.local

        // findings: 
        // mac type is incorrect ??? No 20 networks found. ???, wrong hostname displayed (but the hostname ml-P4.local works!)
        // deleting the Saved network: it still connects (still stored on the C5 chip???)
        // erase flash: Wifi still connects!!

        // other messages seen when changing the network settings (but gone after restart - using settings from the C5 chip again?):

        // [0;31mE (53310) ARDUINO: Could not set mode! 0xffffffff: ESP_FAIL
        // E (53310) ARDUINO: AP enable failed!
        // E (53310) ARDUINO: Could not set mode! 0xffffffff: ESP_FAIL
        // E (53315) ARDUINO: AP enable failed!
        // [0;31mE (56840) ARDUINO: STA was enabled, but netif is NULL???[0m
        // E (56840) ARDUINO: Could not set hostname! 0x102: ESP_ERR_INVALID_ARG
        // E (56842) 🐼: WiFi scan failed.[0m
        // [0;31mE (63310) ARDUINO: Could not set mode! 0xffffffff: ESP_FAIL
        // E (63310) ARDUINO: AP enable failed!
        // E (63310) ARDUINO: Could not set mode! 0xffffffff: ESP_FAIL
        // E (63315) ARDUINO: AP enable failed!
        // [0;31mE (66840) ARDUINO: STA was enabled, but netif is NULL???[0m
        // E (66840) ARDUINO: Could not set hostname! 0x102: ESP_ERR_INVALID_ARG
        // E (66842) 🐼: WiFi scan failed.

        //Shutting down WiFi - AP is initiatalized 🎉

        // [0;32mI (5317) 🐼: WiFi Disconnected. Reason code=201 (NO_AP_FOUND)
        // [0;31mE (14628) 🐼: WiFi scan failed.
        // [0;32mI (17504) 🐼: WiFi Disconnected. Reason code=201 (NO_AP_FOUND)
        // [0;31mE (24628) 🐼: WiFi scan failed.
        // [0;32mI (27504) 🐼: WiFi Disconnected. Reason code=201 (NO_AP_FOUND)
        // [0;31mE (34636) 🐼: WiFi scan failed.
        // [0;32mI (37513) 🐼: WiFi Disconnected. Reason code=201 (NO_AP_FOUND)
        // [0;31mE (44645) 🐼: WiFi scan failed.
        // [0;32mI (47522) 🐼: WiFi Disconnected. Reason code=201 (NO_AP_FOUND)
        // [0;31mE (54644) 🐼: WiFi scan failed.
        // [0;32mI (57521) 🐼: WiFi Disconnected. Reason code=201 (NO_AP_FOUND)
        // [0;31mE (64653) 🐼: WiFi scan failed.

    #endif
}

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_src_certs_x509_crt_bundle_bin_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_src_certs_x509_crt_bundle_bin_end");

// 🌙 only send analytics once
bool sendAnalytics() {
  if (WiFi.status() != WL_CONNECTED) return false;

//   ESP_LOGI(SVK_TAG, "send Event %s to Google Analytics", eventName.c_str());

  String client_id = "";
  for (int i = 0; i < 16; i++) {
    uint8_t n = esp_random()%16; //true random © softhack007
    client_id += String(n, HEX);
  }

  String eventName = BUILD_TARGET;
  eventName.toLowerCase(); // GA4 requires lowercase
  eventName.replace("-", "_");
  eventName = eventName.substring(0, 40);

  WiFiClientSecure client;
//   client.setInsecure();  // skip certificate verification for GA4
  client.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);

  HTTPClient http;  // only one HTTPClient instance

  // --- Step 1: get country via ip-api.com ---
  String country = "unknown";
  http.begin("http://ip-api.com/json");  // plain HTTP
  int code = http.GET();
  if (code == 200) {
    String payloadStr = http.getString();
    // ESP_LOGI(SVK_TAG, "ip-api payload: %s", payloadStr.c_str());
    int idx = payloadStr.indexOf("\"country\":\"");
    if (idx != -1) {
      idx += 11;
      int endIdx = payloadStr.indexOf("\"", idx);
      country = payloadStr.substring(idx, endIdx);
    }
  }
  http.end();  // finish ip-api request

  // --- Step 2: send event to GA4 ---
  String url = "https://www.google-analytics.com/mp/collect?measurement_id=" 
                + String("G-0PXJER2TPD") + "&api_secret=" + String("5XSeAS") + String("6gSEibdr") + String("LiKRx1UQ");

  // Build valid JSON payload
  String payload = "{"
                   "\"client_id\":\"" + client_id + "\","
                   "\"events\":[{"
                     "\"name\":\"" + eventName + "\","
                     "\"params\":{"
                    //    "\"type\":\"" + BUILD_TARGET + "\","
                       "\"version\":\"" + APP_VERSION + "\","
                       "\"board\":\"" + "Default" + "\","
                       "\"country\":\"" + country + "\""
                     "}"
                   "}]"
                 "}";

  http.begin(client, url);           // HTTPS GA4
  http.addHeader("Content-Type", "application/json");
  code = http.POST(payload);
//   ESP_LOGI(SVK_TAG, "Event '%s' sent, HTTP %d payload: %s", eventName.c_str(), code, payload.c_str());
  http.end();
  return code == 204; // successfull
}

void WiFiSettingsService::loop()
{
    unsigned long currentMillis = millis();

    // Handle delayed reconnection
    if (_delayedReconnectPending && currentMillis >= _delayedReconnectTime)
    {
        _delayedReconnectPending = false;
        ESP_LOGI(SVK_TAG, "Executing delayed WiFi reconnection");
        reconfigureWiFiConnection();
    }

    if (!_lastConnectionAttempt || (unsigned long)(currentMillis - _lastConnectionAttempt) >= WIFI_RECONNECTION_DELAY)
    {
        _lastConnectionAttempt = currentMillis;
        manageSTA();

        // 🌙 only send analytics once (if enabled)
        if (_state.trackAnalytics && !_analyticsSent) {
            #if FT_ENABLED(FT_MOONLIGHT)
                if (psramFound())
            #endif
                    _analyticsSent = sendAnalytics(); // 🌙 disabled for MoonLight without PSRAM atm
        }
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
    else
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

        // Connection mode PRIORITY: connect to the first available network
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
        // Connection mode STRENGTH: connect to the strongest network
        else if (_state.staConnectionMode == (u_int8_t)STAConnectionMode::STRENGTH)
        {
            if (bestNetwork)
            {
                ESP_LOGI(SVK_TAG, "Connecting to strongest network: %s, BSSID: " MACSTR " ", bestNetwork->ssid.c_str(), MAC2STR(bestNetwork->bssid));
                configureNetwork(*bestNetwork);
            }
            else
            {
                ESP_LOGI(SVK_TAG, "No suitable network found.");
            }
        }
        // Connection mode OFFLINE: do not connect to any network
        else if (_state.staConnectionMode == (u_int8_t)STAConnectionMode::OFFLINE)
        {
            ESP_LOGI(SVK_TAG, "WiFi connection mode is OFFLINE, not connecting to any network.");
        }
        // Connection mode is unknown: do not connect to any network
        else
        {
            ESP_LOGE(SVK_TAG, "Unknown connection mode, not connecting to any network.");
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

    ESP_LOGD(SVK_TAG, "Connecting to SSID: %s, Channel: %d, BSSID: " MACSTR ", Hostname: %s", network.ssid.c_str(), network.channel, MAC2STR(network.bssid), _state.hostname.c_str());
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
        ESP_LOGI(SVK_TAG, "WiFi setTxPower to: %d", _state.txPower?_state.txPower:-1);
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
    manageSTA();
}

void WiFiSettingsService::onStationModeStop(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (_stopping)
    {
        _lastConnectionAttempt = 0;
        _stopping = false;
    }
}
