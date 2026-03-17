#ifndef ESP32SvelteKit_h
#define ESP32SvelteKit_h

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

#include <Arduino.h>

#include <WiFi.h>
#include <ESPmDNS.h>
#include <AnalyticsService.h>
#include <FeaturesService.h>
#if FT_ENABLED(FT_WIFI) // 🌙
#include <APSettingsService.h>
#include <APStatus.h>
#include <WiFiScanner.h>
#include <WiFiSettingsService.h>
#include <WiFiStatus.h>
#endif
#if FT_ENABLED(FT_ETHERNET) // 🌙
#include <EthernetSettingsService.h>
#include <EthernetStatus.h>
#endif
#include <AuthenticationService.h>
#include <BatteryService.h>
#include <FactoryResetService.h>
#include <DownloadFirmwareService.h>
#include <EventSocket.h>
#include <MqttSettingsService.h>
#include <MqttStatus.h>
#include <NotificationService.h>
#include <NTPSettingsService.h>
#include <NTPStatus.h>
#include <UploadFirmwareService.h>
#include <RestartService.h>
#include <SecuritySettingsService.h>
#include <SleepService.h>
#include <SystemStatus.h>
#include <CoreDump.h>
#include <ESPFS.h>
#include <PsychicHttp.h>
#include <vector>

#ifdef EMBED_WWW
#include <WWWData.h>
#endif

#ifndef CORS_ORIGIN
#define CORS_ORIGIN "*"
#endif

#ifndef APP_VERSION
#define APP_VERSION "demo"
#endif

#ifndef APP_NAME
#define APP_NAME "ESP32 SvelteKit Demo"
#endif

#ifndef ESP32SVELTEKIT_RUNNING_CORE
#define ESP32SVELTEKIT_RUNNING_CORE -1
#endif

#ifndef ESP32SVELTEKIT_LOOP_INTERVAL
#define ESP32SVELTEKIT_LOOP_INTERVAL 10
#endif

// define callback function to include into the main loop
typedef std::function<void()> loopCallback;

// enum for connection status
enum class ConnectionStatus
{
    OFFLINE,
    AP,
    AP_CONNECTED,
    STA,
    STA_CONNECTED,
    STA_MQTT
};

//🌙 added to telemetry
extern bool safeModeMB; // 🌙 true when the ESP32 is in safe mode, false when it is not
extern bool restartNeeded; // 🌙
extern bool saveNeeded; // 🌙 saveNeeded Indicates that changes has been made which need to be saved (or canceled)

// 🌙 Network helpers — work regardless of FT_WIFI / FT_ETHERNET feature flags
/// Returns true if any network interface (WiFi or Ethernet) is connected.
inline bool networkIsConnected() {
    if (WiFi.isConnected()) return true;
#if FT_ENABLED(FT_ETHERNET)
    if (ETH.connected()) return true;
#endif
    return false;
}

/// Returns the local IP of the active network interface (WiFi preferred, then Ethernet).
inline IPAddress networkLocalIP() {
    if (WiFi.isConnected()) return WiFi.localIP();
#if FT_ENABLED(FT_ETHERNET)
    if (ETH.connected()) return ETH.localIP();
#endif
    return IPAddress();
}

class ESP32SvelteKit
{
public:
    uint16_t lps_all = 0;            // 🌙 frame rate live counter — incremented by driverTask, reset each second
    uint16_t lps_all_snapshot = 0;   // 🌙 latched copy of lps_all (stable for readers like SystemStatus)
    uint32_t lps_effects_cycles = 0; // 🌙 CPU cycles consumed by layerP.loop() per second (accumulated)
    uint32_t lps_drivers_cycles = 0; // 🌙 CPU cycles consumed by layerP.loopDrivers() per second (accumulated)
    uint16_t lps_effects = 0;        // 🌙 effects theoretical max loops/s (computed each second)
    uint16_t lps_drivers = 0;        // 🌙 drivers theoretical max loops/s (computed each second)

    ESP32SvelteKit(PsychicHttpServer *server, unsigned int numberEndpoints = 115);

    void begin();

    ConnectionStatus getConnectionStatus()
    {
        return _connectionStatus;
    }

    FS *getFS()
    {
        return &ESPFS;
    }

    PsychicHttpServer *getServer()
    {
        return _server;
    }

    SecurityManager *getSecurityManager()
    {
        return &_securitySettingsService;
    }

    EventSocket *getSocket()
    {
        return &_socket;
    }

#if FT_ENABLED(FT_SECURITY)
    SecuritySettingsService *getSecuritySettingsService()
    {
        return &_securitySettingsService;
    }
#endif

#if FT_ENABLED(FT_WIFI) // 🌙
    WiFiSettingsService *getWiFiSettingsService()
    {
        return &_wifiSettingsService;
    }

    APSettingsService *getAPSettingsService()
    {
        return &_apSettingsService;
    }
#endif

    // 🌙 added getEthernetSettingsService
#if FT_ENABLED(FT_ETHERNET)
    EthernetSettingsService *getEthernetSettingsService()
    {
        return &_ethernetSettingsService;
    }
#endif

    NotificationService *getNotificationService()
    {
        return &_notificationService;
    }

#if FT_ENABLED(FT_NTP)
    NTPSettingsService *getNTPSettingsService()
    {
        return &_ntpSettingsService;
    }
#endif

#if FT_ENABLED(FT_MQTT)
    MqttSettingsService *getMqttSettingsService()
    {
        return &_mqttSettingsService;
    }

    PsychicMqttClient *getMqttClient()
    {
        return _mqttSettingsService.getMqttClient();
    }
#endif

#if FT_ENABLED(FT_SLEEP)
    SleepService *getSleepService()
    {
        return &_sleepService;
    }
#endif

#if FT_ENABLED(FT_BATTERY)
    BatteryService *getBatteryService()
    {
        return &_batteryService;
    }
#endif

    FeaturesService *getFeatureService()
    {
        return &_featureService;
    }

    RestartService *getRestartService()
    {
        return &_restartService;
    }

#if FT_ENABLED(FT_ANALYTICS)
    // 🌙 needed to get lps 
    AnalyticsService *getAnalyticsService()
    {
        return &_analyticsService;
    }
#endif

    void factoryReset()
    {
        _factoryResetService.factoryReset();
    }

    void setMDNSAppName(String name)
    {
        _appName = name;
    }

#if FT_ENABLED(FT_WIFI) // 🌙
    void recoveryMode()
    {
        _apSettingsService.recoveryMode();
    }
#endif

    void addLoopFunction(loopCallback function)
    {
        _loopFunctions.push_back(function);
    }

    // 🌙 Unified system hostname — returns the active network hostname.
    // Priority: WiFi hostname → Ethernet hostname → "ML" + last 4 MAC chars → "MoonLight"
    String getSystemHostname()
    {
#if FT_ENABLED(FT_WIFI) // 🌙
        String h = _wifiSettingsService.getHostname();
        if (h.length()) return h;
#endif
#if FT_ENABLED(FT_ETHERNET)
        String h2 = _ethernetSettingsService.getHostname();
        if (h2.length()) return h2;
#endif
        // Fallback: "ML" + last 4 hex chars of base MAC (e.g. "ML1A2B")
        // Use esp_efuse base MAC — always available on all ESP32 variants,
        // independent of WiFi/Ethernet interface state.
        uint8_t mac[6];
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            char suffix[5];
            snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
            return String("ML") + suffix;
        }
        return "MoonLight";
    }

private:
    PsychicHttpServer *_server;
    TaskHandle_t _loopTaskHandle;
    unsigned int _numberEndpoints;
    FeaturesService _featureService;
    SecuritySettingsService _securitySettingsService;
#if FT_ENABLED(FT_WIFI) // 🌙
    WiFiSettingsService _wifiSettingsService;
    WiFiScanner _wifiScanner;
    WiFiStatus _wifiStatus;
    APSettingsService _apSettingsService;
    APStatus _apStatus;
#endif
#if FT_ENABLED(FT_ETHERNET)
    EthernetSettingsService _ethernetSettingsService;
    EthernetStatus _ethernetStatus;
#endif
    EventSocket _socket;
    NotificationService _notificationService;
#if FT_ENABLED(FT_NTP)
    NTPSettingsService _ntpSettingsService;
    NTPStatus _ntpStatus;
#endif
#if FT_ENABLED(FT_UPLOAD_FIRMWARE)
    UploadFirmwareService _uploadFirmwareService;
#endif
#if FT_ENABLED(FT_DOWNLOAD_FIRMWARE)
    DownloadFirmwareService _downloadFirmwareService;
#endif
#if FT_ENABLED(FT_MQTT)
    MqttSettingsService _mqttSettingsService;
    MqttStatus _mqttStatus;
#endif
#if FT_ENABLED(FT_SECURITY)
    AuthenticationService _authenticationService;
#endif
#if FT_ENABLED(FT_SLEEP)
    SleepService _sleepService;
#endif
#if FT_ENABLED(FT_BATTERY)
    BatteryService _batteryService;
#endif
#if FT_ENABLED(FT_ANALYTICS)
    AnalyticsService _analyticsService;
#endif
#if FT_ENABLED(FT_COREDUMP)
    CoreDump _coreDump;
#endif
    RestartService _restartService;
    FactoryResetService _factoryResetService;
    SystemStatus _systemStatus;

    String _appName = APP_NAME;

protected:
    static void _loopImpl(void *_this) { static_cast<ESP32SvelteKit *>(_this)->_loop(); }
    void _loop();

    std::vector<loopCallback> _loopFunctions;

    // Connectivity status
    ConnectionStatus _connectionStatus = ConnectionStatus::OFFLINE;
};

#endif
