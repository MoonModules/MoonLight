/**
    @title     MoonBase
    @file      ModuleDevices.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/devices/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleDevices_h
#define ModuleDevices_h

#if FT_MOONBASE == 1

  #include "MoonBase/Module.h"
  #include "MoonBase/Utilities.h"

struct UDPMessage {
  uint8_t rommel[6];
  Char<32> name;
};

class ModuleDevices : public Module {
 public:
  WiFiUDP deviceUDP;
  uint16_t deviceUDPPort = 65506;
  bool deviceUDPConnected = false;

  ModuleDevices(PsychicHttpServer* server, ESP32SvelteKit* sveltekit) : Module("devices", server, sveltekit) { EXT_LOGV(MB_TAG, "constructor"); }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(MB_TAG, "");
    JsonObject control;  // state.data has one or more properties
    JsonArray rows;      // if a control is an array, this is the rows of the array

    control = addControl(controls, "devices", "rows");
    control["filter"] = "";
    control["crud"] = "rd";  // delete old devices
    rows = control["n"].to<JsonArray>();
    {
      addControl(rows, "name", "mdnsName", 0, 32, true);
      addControl(rows, "ip", "ip", 0, 32, true);
      addControl(rows, "time", "time", 0, 32, true);
      addControl(rows, "mac", "text", 0, 32, true);
    }
  }

  void loop1s() {
    if (!_socket->getConnectedClients()) return;
    if (!WiFi.localIP() && !ETH.localIP()) return;

    if (!deviceUDPConnected) return;

    readUDP();
  }

  void loop10s() {
    if (!_socket->getConnectedClients()) return;
    if (!WiFi.localIP() && !ETH.localIP()) return;

    if (!deviceUDPConnected) {
      deviceUDPConnected = deviceUDP.begin(deviceUDPPort);
      EXT_LOGD(ML_TAG, "deviceUDPConnected %d i:%d p:%d", deviceUDPConnected, deviceUDP.remoteIP()[3], deviceUDPPort);
    }

    if (!deviceUDPConnected) return;

    writeUDP();
  }

  void updateDevices(const char* name, IPAddress ip) {
    // EXT_LOGD(ML_TAG, "updateDevices ...%d %s", ip[3], name);
    if (_state.data["devices"].isNull()) _state.data["devices"].to<JsonArray>();

    JsonObject device = JsonObject();

    for (JsonObject dev : _state.data["devices"].as<JsonArray>()) {
      if (dev["ip"] == ip.toString()) {
        device = dev;
        // EXT_LOGD(ML_TAG, "updated ...%d %s", ip[3], name);
      }
    }
    if (device.isNull()) {
      device = _state.data["devices"].as<JsonArray>().add<JsonObject>();
      EXT_LOGD(ML_TAG, "added ...%d %s", ip[3], name);
      device["ip"] = ip.toString();
    }

    device["name"] = name;           // name can change
    device["time"] = time(nullptr);  // time will change

    if (!_socket->getConnectedClients()) return;
    if (!WiFi.localIP() && !ETH.localIP()) return;

    // sort in vector
    std::vector<JsonObject> v;
    for (JsonObject obj : _state.data["devices"].as<JsonArray>()) v.push_back(obj);
    std::sort(v.begin(), v.end(), [](JsonObject a, JsonObject b) { return a["name"] < b["name"]; });

    // send only the readonly values ...

    // add sorted to devices
    JsonDocument devices;
    devices["devices"].to<JsonArray>();
    for (auto& obj : v) devices["devices"].add(obj);

    JsonObject object = devices.as<JsonObject>();
    update(object, ModuleState::update, _moduleName + "server");
  }

  void readUDP() {
    size_t packetSize = deviceUDP.parsePacket();
    if (packetSize > 0) {
      char buffer[packetSize];
      deviceUDP.read(buffer, packetSize);
      // EXT_LOGD(ML_TAG, "UDP packet read from %d: %s (%d)", deviceUDP.remoteIP()[3], buffer+6, packetSize);

      updateDevices(buffer + 6, deviceUDP.remoteIP());
    }
  }

  void writeUDP() {
    if (deviceUDP.beginPacket(IPAddress(255, 255, 255, 255), deviceUDPPort)) {
      UDPMessage message;
      message.name = esp32sveltekit.getWiFiSettingsService()->getHostname().c_str();
      deviceUDP.write((uint8_t*)&message, sizeof(message));
      deviceUDP.endPacket();

      IPAddress activeIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
      // EXT_LOGD(MB_TAG, "UDP packet written (%s -> %d)", message.name.c_str(), activeIP[3]);
      updateDevices(message.name.c_str(), activeIP);
    }
  }
};

#endif
#endif