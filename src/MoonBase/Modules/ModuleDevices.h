/**
    @title     MoonBase
    @file      ModuleDevices.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/devices/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleDevices_h
#define ModuleDevices_h

#if FT_MOONBASE == 1

  #include "MoonBase/Module.h"
  #include "MoonBase/Utilities.h"
  #include "MoonBase/pal.h"

struct UDPMessage {
  uint8_t rommel[6];
  Char<32> name;
  Char<32> version;
  uint32_t uptime;
  uint16_t packageSize;
  bool lightsOn;
  uint8_t brightness;
  uint8_t palette;
  uint8_t preset;
  bool isControlCommand;
} __attribute__((packed));  // ✅ Force no padding

class ModuleDevices : public Module {
 public:
  NetworkUDP deviceUDP;
  uint16_t deviceUDPPort = 65506;
  bool deviceUDPConnected = false;
  Module* _moduleControl;

  ModuleDevices(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, Module* moduleControl) : Module("devices", server, sveltekit) {
    EXT_LOGV(MB_TAG, "constructor");
    _moduleControl = moduleControl;

    _moduleControl->addUpdateHandler(
        [this](const String& originId) {
          sendUDP(false);  // send this device update over the network, not updateDevices? (also locks _accessMutex ...)
        },
        false);
  }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(MB_TAG, "");
    JsonObject control;  // state.data has one or more properties
    JsonArray rows;      // if a control is an array, this is the rows of the array

    control = addControl(controls, "devices", "rows");
    control["filter"] = "";
    control["crud"] = "r";
    rows = control["n"].to<JsonArray>();
    {
      addControl(rows, "name", "mDNSName", 0, 32, true);
      addControl(rows, "ip", "ip", 0, 32, true);
      addControl(rows, "time", "time", 0, 32, true);
      addControl(rows, "mac", "text", 0, 32, true);
      addControl(rows, "version", "text", 0, 32, true);
      addControl(rows, "uptime", "time", 0, 32, true);
      addControl(rows, "packageSize", "number", 0, 256, true);
      addControl(rows, "lightsOn", "checkbox");
      addControl(rows, "brightness", "slider", 0, 255);
      addControl(rows, "palette", "slider", 0, 71);
      addControl(rows, "preset", "slider", 0, 64);
    }
  }

  void onUpdate(const UpdatedItem& updatedItem) override {
    if (_state.updateOriginId == (String(_moduleName) + "server").c_str()) return;  // only triggered by updates from the UI

    if (updatedItem.parent[0] == "devices") {
      JsonObject device = _state.data["devices"][updatedItem.index[0]];

      IPAddress targetIP;
      if (!targetIP.fromString(device["ip"].as<const char*>())) {
        return;  // Invalid IP
      }

      if (deviceUDP.beginPacket(targetIP, deviceUDPPort)) {
        UDPMessage message{};  // Zero-initialize
        message.name = esp32sveltekit.getWiFiSettingsService()->getHostname().c_str();
        message.version = APP_VERSION;
        // strncpy(message.name, esp32sveltekit.getWiFiSettingsService()->getHostname().c_str(), 32);
        // strncpy(message.version, APP_VERSION, 32);
        message.packageSize = sizeof(message);
        message.lightsOn = device["lightsOn"];
        message.brightness = device["brightness"];
        message.palette = device["palette"];
        message.preset = device["preset"];

        message.uptime = time(nullptr) ? time(nullptr) - pal::millis() / 1000 : pal::millis() / 1000;
        message.isControlCommand = true;

        deviceUDP.write((uint8_t*)&message, sizeof(message));
        deviceUDP.endPacket();
        EXT_LOGD(ML_TAG, "UDP update sent to %s bri=%d pal=%d preset=%d", targetIP.toString().c_str(), message.brightness, message.palette, message.preset);
      }
    }
  }

  void loop20ms() {
    if (!WiFi.localIP() && !ETH.localIP()) return;

    if (!deviceUDPConnected) return;

    receiveUDP();  // and updateDevices
  }

  void loop10s() {
    if (!WiFi.localIP() && !ETH.localIP()) return;

    if (!deviceUDPConnected) {
      deviceUDPConnected = deviceUDP.begin(deviceUDPPort);
      EXT_LOGD(ML_TAG, "deviceUDPConnected %d i:%d p:%d", deviceUDPConnected, deviceUDP.remoteIP()[3], deviceUDPPort);
    }

    if (!deviceUDPConnected) return;

    sendUDP(true);  // send this device update to all devices, updateDevices! ...
  }

  void updateDevices(const UDPMessage& message, IPAddress ip) {
    // EXT_LOGD(ML_TAG, "updateDevices ...%d %s", ip[3], name);
    if (_state.data["devices"].isNull()) _state.data["devices"].to<JsonArray>();

    JsonArray devices;
    JsonDocument doc;
    if (_socket->getConnectedClients()) {
      doc.set(_state.data);  // copy
      devices = doc["devices"];
    } else
      devices = _state.data["devices"];

    JsonObject device = JsonObject();

    for (JsonObject dev : devices) {
      if (dev["ip"] == ip.toString()) {
        device = dev;
        // EXT_LOGD(ML_TAG, "updated ...%d %s", ip[3], name);
      }
    }
    if (device.isNull()) {
      device = devices.add<JsonObject>();
      EXT_LOGD(ML_TAG, "added ...%d %s", ip[3], message.name);
      device["ip"] = ip.toString();
    }

    device["time"] = time(nullptr);  // time will change, triggering update
    device["name"] = message.name.c_str();
    device["version"] = message.version.c_str();
    device["uptime"] = message.uptime;
    device["packageSize"] = message.packageSize;
    device["lightsOn"] = message.lightsOn;
    device["brightness"] = message.brightness;
    device["palette"] = message.palette;
    device["preset"] = message.preset;

    if (!_socket->getConnectedClients()) return;  // no need to update if no clients

    // sort in vector
    std::vector<JsonObject> devicesVector;
    for (JsonObject dev : devices) {
      if (time(nullptr) - dev["time"].as<time_t>() < 86400) devicesVector.push_back(dev);  // max 1 day
    }
    std::sort(devicesVector.begin(), devicesVector.end(), [](JsonObject a, JsonObject b) { return a["name"] < b["name"]; });

    // create sorted devices
    JsonDocument doc2;
    doc2["devices"].to<JsonArray>();
    for (JsonObject device : devicesVector) {
      doc2["devices"].add(device);
    }

    JsonObject controls = doc2.as<JsonObject>();
    update(controls, ModuleState::update, String(_moduleName) + "server");
  }

  void receiveUDP() {
    while (size_t packetSize = deviceUDP.parsePacket()) {
      if (packetSize < 38 || packetSize > sizeof(UDPMessage)) {
        EXT_LOGW(MB_TAG, "Invalid UDP packet size: %d (expected %d-%d)", packetSize, 38, sizeof(UDPMessage));
        deviceUDP.clear();  // Discard invalid packet
        continue;
      }

      char buffer[sizeof(UDPMessage)];
      UDPMessage message{};
      deviceUDP.read(buffer, packetSize);
      memcpy(&message, buffer, packetSize);
      // EXT_LOGD(ML_TAG, "UDP packet read from %d: %s (%d)", deviceUDP.remoteIP()[3], buffer + 6, packetSize);

      if (message.isControlCommand) {
        // If this packet is intended for THIS device (not just broadcast status),
        // OR check a flag in UDPMessage to distinguish control commands from status broadcasts
        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();

        newState["lightsOn"] = message.lightsOn;
        newState["brightness"] = message.brightness;
        newState["palette"] = message.palette;
        newState["preset"]["action"] = "click";
        newState["preset"]["select"] = message.preset;

        _moduleControl->update(newState, ModuleState::update, String(_moduleName) + "server");  // Do not add server in the originID as that blocks updates, see execOnUpdate

        EXT_LOGD(ML_TAG, "Applied UDP control: bri=%d pal=%d preset=%d", message.brightness, message.palette, message.preset);
      } else
        updateDevices(message, deviceUDP.remoteIP());
    }
  }

  void sendUDP(bool includingUpdateDevices) {
    if (deviceUDP.beginPacket(IPAddress(255, 255, 255, 255), deviceUDPPort)) {
      UDPMessage message{};  // Zero-initialize
      message.name = esp32sveltekit.getWiFiSettingsService()->getHostname().c_str();
      message.version = APP_VERSION;
      // strncpy(message.name, esp32sveltekit.getWiFiSettingsService()->getHostname().c_str(), 32);
      // strncpy(message.version, APP_VERSION, 32);
      message.packageSize = sizeof(message);
      _moduleControl->read(
          [&](ModuleState& state) {
            message.lightsOn = state.data["lightsOn"];
            message.brightness = state.data["brightness"];
            message.palette = state.data["palette"];
            message.preset = state.data["preset"]["selected"];
          },
          _moduleName);

      message.uptime = time(nullptr) ? time(nullptr) - pal::millis() / 1000 : pal::millis() / 1000;
      message.isControlCommand = false;

      deviceUDP.write((uint8_t*)&message, sizeof(message));
      deviceUDP.endPacket();
      EXT_LOGD(ML_TAG, "UDP update sent bri=%d pal=%d preset=%d", message.brightness, message.palette, message.preset);

      if (includingUpdateDevices) {
        IPAddress activeIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
        // EXT_LOGD(MB_TAG, "UDP packet written (%s -> %d)", message.name.c_str(), activeIP[3]);
        updateDevices(message, activeIP);
      }
    }
  }
};

#endif
#endif