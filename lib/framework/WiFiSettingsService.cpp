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

#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#endif

WiFiSettingsService::WiFiSettingsService(PsychicHttpServer *server,
                                         FS *fs,
                                         SecurityManager *securityManager,
                                         EventSocket *socket) : _server(server),
                                                                _securityManager(securityManager),
                                                                _httpEndpoint(WiFiSettings::read, WiFiSettings::update, this, server, WIFI_SETTINGS_SERVICE_PATH, securityManager,
                                                                              AuthenticationPredicates::IS_ADMIN),
                                                                _fsPersistence(WiFiSettings::read, WiFiSettings::update, this, fs, WIFI_SETTINGS_FILE), _lastConnectionAttempt(0),
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
    #ifdef CONFIG_IDF_TARGET_ESP32P4
        // P4: Initialize ESP-IDF WiFi
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);
        
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_start());
    #else
        WiFi.mode(WIFI_MODE_STA); // this is the default.

        // Disable WiFi config persistance and auto reconnect
        WiFi.persistent(false);
        WiFi.setAutoReconnect(false);
    #endif

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
    #ifdef CONFIG_IDF_TARGET_ESP32P4
        // P4: Use ESP-IDF commands to disconnect
        esp_err_t err = esp_wifi_disconnect();
        if (err == ESP_OK)
        {
            _stopping = true;
            ESP_LOGD(SVK_TAG, "Successfully disconnected WiFi for reconfiguration");
        }
        else if (err == ESP_ERR_WIFI_NOT_STARTED)
        {
            ESP_LOGD(SVK_TAG, "WiFi not started, nothing to disconnect");
            _stopping = false;
        }
        else
        {
            ESP_LOGW(SVK_TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(err));
        }
    #else
        // Default ESP32-Sveltekit behavior
        if (WiFi.disconnect(true))
        {
            _stopping = true;
        }
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
    uint8_t n = random(0, 16);
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
            _analyticsSent = sendAnalytics();
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
    #ifdef CONFIG_IDF_TARGET_ESP32P4
        // P4: Use ESP-IDF scan
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = {
                .active = {
                    .min = 100,
                    .max = 300
                }
            }
        };
        
        esp_err_t err = esp_wifi_scan_start(&scan_config, true);
        if (err != ESP_OK)
        {
            ESP_LOGE(SVK_TAG, "WiFi scan failed: %s", esp_err_to_name(err));
            return;
        }
        
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        int scanResult = ap_count;
    #else
        int scanResult = WiFi.scanNetworks();
    #endif
    
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

        #ifdef CONFIG_IDF_TARGET_ESP32P4
            // P4: Get scan results from ESP-IDF
            wifi_ap_record_t *ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * scanResult);
            if (ap_records)
            {
                uint16_t ap_count = scanResult;
                esp_wifi_scan_get_ap_records(&ap_count, ap_records);
                
                for (int i = 0; i < scanResult; ++i)
                {
                    String ssid_scan = String((char *)ap_records[i].ssid);
                    int32_t rssi_scan = ap_records[i].rssi;
                    uint8_t *BSSID_scan = ap_records[i].bssid;
                    int32_t chan_scan = ap_records[i].primary;
                    
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
                free(ap_records);
            }
        #else
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
        #endif

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
        #ifdef CONFIG_IDF_TARGET_ESP32P4
            esp_wifi_scan_stop();
        #else
            WiFi.scanDelete();
        #endif
    }
}

void WiFiSettingsService::configureNetwork(wifi_settings_t &network)
{
    #ifdef CONFIG_IDF_TARGET_ESP32P4
        // P4: Use ESP-IDF configuration
        wifi_config_t wifi_config = {};
        
        // Set SSID
        strncpy((char *)wifi_config.sta.ssid, network.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
        
        // Set password
        strncpy((char *)wifi_config.sta.password, network.password.c_str(), sizeof(wifi_config.sta.password) - 1);
        
        // Set BSSID if available
        if (network.channel > 0)
        {
            memcpy(wifi_config.sta.bssid, network.bssid, 6);
            wifi_config.sta.bssid_set = true;
            wifi_config.sta.channel = network.channel;
        }
        
        ESP_LOGD(SVK_TAG, "Connecting to SSID: %s, Channel: %d, BSSID: " MACSTR ", Hostname: %s", network.ssid.c_str(), network.channel, MAC2STR(network.bssid), _state.hostname.c_str());
        
        // Set hostname
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif)
        {
            esp_netif_set_hostname(netif, _state.hostname.c_str());
        }
        
        // Configure static IP if needed
        if (network.staticIPConfig)
        {
            esp_netif_dhcpc_stop(netif);
            
            esp_netif_ip_info_t ip_info;
            ip_info.ip.addr = static_cast<uint32_t>(network.localIP);
            ip_info.gw.addr = static_cast<uint32_t>(network.gatewayIP);
            ip_info.netmask.addr = static_cast<uint32_t>(network.subnetMask);
            
            esp_netif_set_ip_info(netif, &ip_info);
            
            // Set DNS servers
            esp_netif_dns_info_t dns_info;
            dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(network.dnsIP1);
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
            
            if (network.dnsIP2 != IPAddress(0, 0, 0, 0))
            {
                dns_info.ip.u_addr.ip4.addr = static_cast<uint32_t>(network.dnsIP2);
                esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_info);
            }
        }
        else
        {
            esp_netif_dhcpc_start(netif);
        }
        
        // Set WiFi config
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        
        // Set TX power
        if (_state.txPower != 0)
        {
            int8_t power = 0;
            switch (_state.txPower)
            {
                case 84: power = 84; break; // 21dBm
                case 82: power = 82; break; // 20.5dBm
                case 80: power = 80; break; // 20dBm
                case 78: power = 78; break; // 19.5dBm
                case 76: power = 76; break; // 19dBm
                case 74: power = 74; break; // 18.5dBm
                case 68: power = 68; break; // 17dBm
                case 60: power = 60; break; // 15dBm
                case 52: power = 52; break; // 13dBm
                case 44: power = 44; break; // 11dBm
                case 34: power = 34; break; // 8.5dBm
                case 28: power = 28; break; // 7dBm
                case 20: power = 20; break; // 5dBm
                case 8: power = 8; break;   // 2dBm
                case 4: power = -4; break;  // -1dBm
                default:
                    ESP_LOGE(SVK_TAG, "Invalid txPower value: %d", _state.txPower);
                    break;
            }
            
            if (power != 0)
            {
                esp_wifi_set_max_tx_power(power);
                ESP_LOGI(SVK_TAG, "WiFi setTxPower to: %d", _state.txPower);
            }
        }
        
        // Connect
        esp_wifi_connect();
    #else
        // Non-P4: Use Arduino WiFi library
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
    #endif
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
    #ifdef CONFIG_IDF_TARGET_ESP32P4
        // P4: Use ESP-IDF disconnect
        esp_wifi_disconnect();
    #else
        WiFi.disconnect(true);
    #endif
}

void WiFiSettingsService::onStationModeStop(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (_stopping)
    {
        _lastConnectionAttempt = 0;
        _stopping = false;
    }
}