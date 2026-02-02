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
  char build[16];
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

  bool partOfGroup(const String& base, const String& device, int level = 0) {
    EXT_LOGV(MB_TAG, "partOfGroup %s %s level:%d", base.c_str(), device.c_str(), level);

    String prefix = base;

    // Count dots from the end: level 0 = last dot, level 1 = second-last, etc.
    int pos = base.length();
    for (int i = 0; i <= level; i++) {
      pos = base.lastIndexOf('.', pos - 1);
      if (pos == -1) {
        // Not enough dots at this level - match all devices
        return true;
      }
    }

    prefix = base.substring(0, pos);
    return device.startsWith(prefix);
  }

  ModuleDevices(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, Module* moduleControl) : Module("devices", server, sveltekit) {
    EXT_LOGV(MB_TAG, "constructor");
    _moduleControl = moduleControl;

    _moduleControl->addUpdateHandler(
        [this](const String& originId) {
          EXT_LOGD(MB_TAG, "control update origin %s", originId.c_str());
          sendUDP(originId != "group");  // control message if from UI or not a group (to avoid infinity send loop)
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
      addControl(rows, "lightsOn", "checkbox");
      addControl(rows, "brightness", "slider", 0, 255);
      control = addControl(rows, "time", "time", 0, 32, true);
      control["show"] = true;                        // only the first 3 are shown in RowRenderer, allow here the 4th to be shown as well
      addControl(rows, "palette", "slider", 0, 70);  // todo use define for max palette nr
      addControl(rows, "preset", "slider", 0, 64);
      addControl(rows, "ip", "ip", 0, 32, true);
      // addControl(rows, "mac", "text", 0, 32, true);
      addControl(rows, "version", "text", 0, 32, true);
      addControl(rows, "build", "text", 0, 8, true);
      addControl(rows, "uptime", "time", 0, 32, true);
      addControl(rows, "packageSize", "number", 0, 256, true);
    }
  }

  void onUpdate(const UpdatedItem& updatedItem, const String& originId) override {
    if (!originId.toInt()) return;  // Front-end client IDs are numeric; internal origins ("module", etc.) return 0

    if (updatedItem.parent[0] == "devices") {
      JsonObject device = _state.data["devices"][updatedItem.index[0]];

      IPAddress targetIP;
      if (!targetIP.fromString(device["ip"].as<const char*>())) {
        return;  // Invalid IP
      }

      UDPMessage message{};
      infoToMessage(message, true);  // isControlCommand

      message.name = device["name"].as<const char*>();  // send the name of the device
      deviceToMessage(device, message);

      IPAddress activeIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
      if (targetIP == activeIP) {
        // this device, only update light controls
        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();

        messageToControlState(message, newState);

        _moduleControl->update(newState, ModuleState::update, _moduleName);  // Do not add server in the originID as that blocks updates, see execOnUpdate

        EXT_LOGD(ML_TAG, "Applied UDP control from originator: bri=%d pal=%d preset=%d", message.brightness, message.palette, message.preset);
      } else {
        // if a device is updated in the UI, send that update to that device
        if (deviceUDP.beginPacket(targetIP, deviceUDPPort)) {
          deviceUDP.write((uint8_t*)&message, sizeof(message));
          deviceUDP.endPacket();
          EXT_LOGD(ML_TAG, "UDP from %s update sent to ...%d / %s bri=%d pal=%d preset=%d", originId.c_str(), targetIP[3], message.name.c_str(), message.brightness, message.palette, message.preset);
          // need to add the targetip?
        }
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

    sendUDP(false);  // send this device update to all devices. No control message
  }

  void updateDevices(const UDPMessage& message, IPAddress ip) {
    // EXT_LOGD(ML_TAG, "updateDevices ...%d %s", ip[3], name);
    if (_state.data["devices"].isNull()) _state.data["devices"].to<JsonArray>();

    // set the doc
    JsonDocument doc;
    if (_socket->getActiveClients()) {  // rebuild the devices array
      doc.set(_state.data);             // copy
    } else {
      doc = _state.data;  // reference
    }

    // set the devices array
    JsonArray devices;
    devices = doc["devices"];

    // find out if we have a new device
    JsonObject device = JsonObject();
    bool newDevice = true;
    for (JsonObject dev : devices) {
      if (dev["name"] == message.name.c_str()) {  // check on name as IP can come from another device
        device = dev;
        newDevice = false;
        break;  // found so leave for loop
        // EXT_LOGD(ML_TAG, "updated ...%d %s", ip[3], name);
      }
    }

    // if an update for a device is received, set the device object so it is shown in the UI
    if (newDevice) {
      device = devices.add<JsonObject>();
      EXT_LOGD(ML_TAG, "added ...%d %s", ip[3], message.name.c_str());
    }

    device["ip"] = ip.toString();
    device["time"] = time(nullptr);  // time will change, triggering update
    device["name"] = message.name.c_str();
    device["version"] = message.version.c_str();
    device["build"] = message.build;
    device["uptime"] = message.uptime;
    device["packageSize"] = message.packageSize;
    messageToDevice(message, device);

    if (newDevice) {  // sort devices in vector and add to a new document and update
      JsonDocument doc2;

      std::vector<JsonObject> devicesVector;
      for (JsonObject dev : devices) {
        if (time(nullptr) - dev["time"].as<time_t>() < 86400) devicesVector.push_back(dev);  // max 1 day
      }
      std::sort(devicesVector.begin(), devicesVector.end(), [](JsonObject a, JsonObject b) { return a["name"] < b["name"]; });

      doc2["devices"].to<JsonArray>();
      for (JsonObject device : devicesVector) {
        doc2["devices"].add(device);
      }
      JsonObject newState = doc2.as<JsonObject>();
      update(newState, ModuleState::update, _moduleName);
    } else {
      // only update the updated device
      JsonObject newState = doc.as<JsonObject>();
      update(newState, ModuleState::update, _moduleName);
    }
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

      // if a controlmessage is received (from another device), and this device is part of its group update this device.
      // this can be both a broadcast or a unicast (then partofgroup will also be true)
      if (message.isControlCommand && partOfGroup(esp32sveltekit.getWiFiSettingsService()->getHostname(), message.name.c_str())) {
        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();

        messageToControlState(message, newState);

        EXT_LOGD(ML_TAG, "Applied UDP control from group via %s : bri=%d pal=%d preset=%d", message.name.c_str(), message.brightness, message.palette, message.preset);

        if (esp32sveltekit.getWiFiSettingsService()->getHostname() == message.name.c_str()) {
          _moduleControl->update(newState, ModuleState::update, _moduleName);  // Do not add server in the originID as that blocks updates, see execOnUpdate
          // not a group so it needs to propagate...
        } else
          _moduleControl->update(newState, ModuleState::update, "group");  // Do not add server in the originID as that blocks updates, see execOnUpdate
      }

      // also update if control command as it can be another device
      updateDevices(message, deviceUDP.remoteIP());
    }
  }

  // from addUpdateHandler (false, true) and loop10s (true, false)
  void sendUDP(bool isControlCommand) {
    // broadcast own status to all devices, called by addUpdateHandler and loop10s
    if (deviceUDP.beginPacket(IPAddress(255, 255, 255, 255), deviceUDPPort)) {
      UDPMessage message{};  // Zero-initialize
      infoToMessage(message, isControlCommand);

      _moduleControl->read([&](ModuleState& state) { controlStateToMessage(state.data, message); }, _moduleName);

      deviceUDP.write((uint8_t*)&message, sizeof(message));
      deviceUDP.endPacket();
      EXT_LOGD(ML_TAG, "UDP update sent: bri=%d pal=%d preset=%d control: %d", message.brightness, message.palette, message.preset, isControlCommand);

      // in case of loop10s, there is no udp received from itself so update manually
      IPAddress activeIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
      // EXT_LOGD(MB_TAG, "UDP packet written (%s -> %d)", message.name.c_str(), activeIP[3]);
      updateDevices(message, activeIP);
    }
  }

 private:
  void infoToMessage(UDPMessage& message, bool isControlCommand) {
    message.name = esp32sveltekit.getWiFiSettingsService()->getHostname().c_str();
    message.version = APP_VERSION;
    strncpy(message.build, APP_DATE, sizeof(message.build));
    // message.build[8] = '\0';
    message.packageSize = sizeof(message);
    message.uptime = time(nullptr) ? time(nullptr) - pal::millis() / 1000 : pal::millis() / 1000;
    message.isControlCommand = isControlCommand;
  }

  // ✅ Helper: Copy control fields from message to control state (for applying updates)
  void messageToControlState(const UDPMessage& message, JsonObject& newState) {
    newState["lightsOn"] = message.lightsOn;
    newState["brightness"] = message.brightness;
    newState["palette"] = message.palette;
    _moduleControl->read([&](ModuleState& state) { newState["preset"] = state.data["preset"]; }, String(_moduleName) + __FUNCTION__);
    newState["preset"]["action"] = "click";
    newState["preset"]["select"] = message.preset;
  }

  // ✅ Helper: Copy control fields from control state to message (for broadcasting)
  void controlStateToMessage(const JsonObject& state, UDPMessage& message) {
    message.lightsOn = state["lightsOn"];
    message.brightness = state["brightness"];
    message.palette = state["palette"];
    message.preset = state["preset"]["selected"];
  }

  // ✅ Helper: Copy all fields from message to device object (for UI display)
  void messageToDevice(const UDPMessage& message, JsonObject& device) {
    device["lightsOn"] = message.lightsOn;
    device["brightness"] = message.brightness;
    device["palette"] = message.palette;
    device["preset"] = message.preset;
  }

  // ✅ Helper: Copy control fields from device to message (for sending control)
  void deviceToMessage(const JsonObject& device, UDPMessage& message) {
    message.lightsOn = device["lightsOn"];
    message.brightness = device["brightness"];
    message.palette = device["palette"];
    message.preset = device["preset"];
  }
};

#endif
#endif