/**
    @title     MoonBase
    @file      ModuleDevices.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/devices/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleDevices_h
#define ModuleDevices_h

#if FT_MOONBASE == 1

  #include "MoonBase/Module.h"
  #include "MoonBase/utilities/PlatformFunctions.h"
  #include "MoonBase/utilities/pal.h"

// WLED-compatible 44-byte discovery header. Matches UDPWLEDMessage in WLED / StarLight exactly.
// WLED validates: token==255, id==1, ip0==localIP[0] (subnet check).
// type byte: low 7 bits = board type (32=ESP32, 33=S2, 34=S3, 35=C3), bit 7 = lights on.
struct UDPWLEDHeader {
  uint8_t  token;    // 0:   255 — required by WLED
  uint8_t  id;       // 1:   1   — required by WLED
  uint8_t  ip0;      // 2-5: sender IP address
  uint8_t  ip1;
  uint8_t  ip2;
  uint8_t  ip3;
  char     name[32]; // 6-37: null-padded hostname
  uint8_t  type;     // 38: board type | 0x80 if lights on
  uint8_t  insId;    // 39: last IP octet (WLED uses as instance index)
  uint32_t version;  // 40-43: numeric build date (YYYYMMDD from APP_DATE)
} __attribute__((packed));
static_assert(sizeof(UDPWLEDHeader) == 44, "UDPWLEDHeader must be exactly 44 bytes");

// Full MoonLight status/discovery message broadcast on port 65506.
// First 44 bytes match UDPWLEDHeader so WLED can read name, type, and lights-on state.
// MoonLight receivers distinguish this from a WLED packet by size (sizeof(UDPMessage) != 44).
struct UDPMessage {
  UDPWLEDHeader header; // 44 bytes — WLED-compatible
  Char<32> versionStr;  // human-readable version string
  char build[16];       // build date string
  uint32_t uptime;
  uint16_t packageSize; // sizeof(UDPMessage) — used by receiver to validate packet type
  uint8_t brightness;
  uint8_t palette;
  uint8_t preset;
} __attribute__((packed));  // Force no padding
static_assert(sizeof(UDPMessage) == 101, "UDPMessage must be exactly 101 bytes");

// MoonLight-only control message sent exclusively on port 65507.
// WLED does not listen on port 65507, so it never receives these packets.
struct UDPControlMessage {
  UDPWLEDHeader header; // 44 bytes: sender identification
  char targetName[32];  // unicast: receiver hostname; group broadcast: empty string
  uint8_t brightness;
  uint8_t lightsOn;
  uint8_t palette;
  uint8_t preset;
} __attribute__((packed));  // Force no padding
static_assert(sizeof(UDPControlMessage) == 80, "UDPControlMessage must be exactly 80 bytes");

class ModuleDevices : public Module {
 public:
  NetworkUDP deviceUDP;         // Discovery: port 65506 — WLED-compatible broadcasts
  NetworkUDP deviceControlUDP;  // Control:   port 65507 — MoonLight only, WLED never sees this
  uint16_t deviceUDPPort = 65506;
  uint16_t deviceControlUDPPort = 65507;
  bool deviceUDPConnected = false;
  bool deviceControlUDPConnected = false;
  Module* _moduleControl;

  ModuleDevices(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, Module* moduleControl) : Module("devices", server, sveltekit) {
    EXT_LOGV(MB_TAG, "constructor");
    _moduleControl = moduleControl;

    _moduleControl->addUpdateHandler(
        [this](const String& originId) {
          // EXT_LOGD(MB_TAG, "control update %s", originId.c_str());
          sendUDP(originId != "group");  // sendUDP control yes / no
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
      control = addControl(rows, "lastSync", "time", 0, 32, true);
      control["show"] = true;                        // only the first 3 are shown in RowRenderer, allow here the 4th to be shown as well
      addControl(rows, "palette", "slider", 0, 70);  // todo use define for max palette nr
      addControl(rows, "preset", "slider", 0, 64);
      addControl(rows, "ip", "ip", 0, 32, true);
      addControl(rows, "version", "text", 0, 32, true);
      addControl(rows, "build", "text", 0, 8, true);
      addControl(rows, "uptime", "time", 0, 32, true);
      addControl(rows, "packageSize", "number", 0, 256, true);
    }
  }

  void onUpdate(const UpdatedItem& updatedItem) override {
    if (!updatedItem.originId->toInt()) return;  // Front-end client IDs are numeric; internal origins ("module", etc.) return 0

    if (updatedItem.parent[0] == "devices") {
      JsonObject device = _state.data["devices"][updatedItem.index[0]];

      IPAddress targetIP;
      if (!targetIP.fromString(device["ip"].as<const char*>())) {
        return;  // Invalid IP
      }

      UDPControlMessage msg{};
      infoToControlMessage(msg);
      deviceToControlMessage(device, msg);

      IPAddress activeIP = networkLocalIP();
      if (targetIP == activeIP) {
        // this device, only update light controls
        JsonDocument doc;
        JsonObject newState = doc.to<JsonObject>();
        messageToControlState(msg, newState);
        _moduleControl->update(newState, ModuleState::update, _moduleName);  // Do not add server in the originID as that blocks updates, see execOnUpdate
        EXT_LOGD(MB_TAG, "Applied UDP control from originator: bri=%d pal=%d preset=%d", msg.brightness, msg.palette, msg.preset);
      } else {
        // if a device is updated in the UI, send that update to that device via control port (WLED does not listen there)
        strlcpy(msg.targetName, device["name"].as<const char*>(), sizeof(msg.targetName));
        if (deviceControlUDP.beginPacket(targetIP, deviceControlUDPPort)) {
          deviceControlUDP.write(reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
          deviceControlUDP.endPacket();
          EXT_LOGD(MB_TAG, "UDP control from %s update sent to ...%d / %s on=%d bri=%d pal=%d preset=%d",
                   updatedItem.originId->c_str(), targetIP[3], msg.targetName, msg.lightsOn, msg.brightness, msg.palette, msg.preset);
        }
      }
    }
  }

  void loop20ms() override {
    Module::loop20ms();

    if (!networkIsConnected()) return;

    // Each socket is polled independently — a failed bind on one does not block the other
    if (deviceUDPConnected || deviceControlUDPConnected) receiveUDP();
  }

  void loop10s() override {
    if (!networkIsConnected()) return;

    // Bind each socket independently so a failure on one does not prevent the other from retrying
    if (!deviceUDPConnected) {
      deviceUDPConnected = deviceUDP.begin(deviceUDPPort);
      if (deviceUDPConnected)
        EXT_LOGD(MB_TAG, "deviceUDP bound on port %d", deviceUDPPort);
      else
        EXT_LOGW(MB_TAG, "Failed to bind discovery UDP on port %d", deviceUDPPort);
    }
    if (!deviceControlUDPConnected) {
      deviceControlUDPConnected = deviceControlUDP.begin(deviceControlUDPPort);
      if (deviceControlUDPConnected)
        EXT_LOGD(MB_TAG, "deviceControlUDP bound on port %d", deviceControlUDPPort);
      else
        EXT_LOGW(MB_TAG, "Failed to bind control UDP on port %d", deviceControlUDPPort);
    }

    if (!deviceUDPConnected) return;

    sendUDP(false);  // send this device update to all devices. No control message
  }

  // Update device table from a full MoonLight discovery packet
  void updateDevices(const UDPMessage& message, IPAddress ip) {
    // EXT_LOGD(MB_TAG, "updateDevices ...%d %s", ip[3], message.header.name);
    if (_state.data["devices"].isNull()) _state.data["devices"].to<JsonArray>();

    // set the doc
    JsonDocument doc;
    if (_sveltekit->getSocket()->getActiveClients()) {  // rebuild the devices array
      doc.set(_state.data);             // copy
    } else {
      doc = _state.data;  // reference
    }

    // set the devices array
    JsonArray devices = doc["devices"];

    // find out if we have a new device
    JsonObject device = JsonObject();
    bool newDevice = true;
    for (JsonObject dev : devices) {
      if (dev["name"] == message.header.name) {  // check on name as IP can come from another device
        device = dev;
        newDevice = false;
        break;  // found so leave for loop
        // EXT_LOGD(MB_TAG, "updated ...%d %s", ip[3], message.header.name);
      }
    }

    // if an update for a device is received, set the device object so it is shown in the UI
    if (newDevice) {
      device = devices.add<JsonObject>();
      EXT_LOGD(MB_TAG, "added MoonLight ...%d %s", ip[3], message.header.name);
    }

    device["ip"] = ip.toString();
    device["lastSync"] = time(nullptr);  // time will change, triggering update
    device["name"] = message.header.name;
    device["version"] = message.versionStr.c_str();
    device["build"] = message.build;
    device["uptime"] = message.uptime;
    device["packageSize"] = message.packageSize;
    device["lightsOn"] = (message.header.type & 0x80) != 0;
    device["brightness"] = message.brightness;
    device["palette"] = message.palette;
    device["preset"] = message.preset;

    finaliseDeviceUpdate(doc, devices, newDevice);
  }

  // Update device table from a WLED 44-byte discovery packet (limited fields available)
  void updateDevicesWLED(const UDPWLEDHeader& header, IPAddress ip) {
    if (_state.data["devices"].isNull()) _state.data["devices"].to<JsonArray>();

    // set the doc
    JsonDocument doc;
    if (_sveltekit->getSocket()->getActiveClients()) {  // rebuild the devices array
      doc.set(_state.data);             // copy
    } else {
      doc = _state.data;  // reference
    }

    // set the devices array
    JsonArray devices = doc["devices"];

    // find out if we have a new device
    JsonObject device = JsonObject();
    bool newDevice = true;
    for (JsonObject dev : devices) {
      if (dev["name"] == header.name) {  // check on name as IP can come from another device
        device = dev;
        newDevice = false;
        break;  // found so leave for loop
      }
    }

    // if an update for a device is received, set the device object so it is shown in the UI
    if (newDevice) {
      device = devices.add<JsonObject>();
      EXT_LOGD(MB_TAG, "added WLED ...%d %s", ip[3], header.name);
    }

    device["ip"] = ip.toString();
    device["lastSync"] = time(nullptr);  // time will change, triggering update
    device["name"] = header.name;
    char verBuf[12];
    snprintf(verBuf, sizeof(verBuf), "%lu", (unsigned long)header.version);
    device["version"] = verBuf;
    device["build"] = "";
    device["uptime"] = 0;
    device["packageSize"] = (uint16_t)sizeof(UDPWLEDHeader);
    device["lightsOn"] = (header.type & 0x80) != 0;
    device["brightness"] = 0;  // not available in WLED discovery packet
    device["palette"] = 0;
    device["preset"] = 0;

    finaliseDeviceUpdate(doc, devices, newDevice);
  }

  // Apply an incoming control message from port 65507
  void processControlMessage(const UDPControlMessage& msg) {
    const char* senderName = msg.header.name;
    const char* myName = esp32sveltekit.getSystemHostname().c_str();

    if (strcmp(senderName, myName) == 0) return;  // ignore own broadcast echo

    bool isUnicast = (msg.targetName[0] != '\0') && (strcmp(myName, msg.targetName) == 0);
    bool isGroupBroadcast = (msg.targetName[0] == '\0') && partOfGroup(myName, senderName);

    if (!isUnicast && !isGroupBroadcast) return;

    JsonDocument doc;
    JsonObject newState = doc.to<JsonObject>();
    messageToControlState(msg, newState);

    EXT_LOGD(MB_TAG, "UDP control from %s (%s): bri=%d pal=%d preset=%d",
             senderName, isGroupBroadcast ? "group" : "unicast", msg.brightness, msg.palette, msg.preset);
    // "group" origin suppresses re-broadcast in addUpdateHandler to prevent loops
    _moduleControl->update(newState, ModuleState::update, isGroupBroadcast ? "group" : _moduleName);
  }

  void receiveUDP() {
    // Discovery port 65506: accept WLED (44-byte) and MoonLight (sizeof(UDPMessage)) packets
    while (size_t packetSize = deviceUDP.parsePacket()) {
      if (packetSize == sizeof(UDPWLEDHeader)) {
        UDPWLEDHeader header{};
        deviceUDP.read(reinterpret_cast<uint8_t*>(&header), packetSize);
        header.name[sizeof(header.name) - 1] = '\0';
        if (header.token == 255 && header.id == 1)
          updateDevicesWLED(header, deviceUDP.remoteIP());
        else
          EXT_LOGW(MB_TAG, "Bad WLED header from ...%d (token=%d id=%d)", deviceUDP.remoteIP()[3], header.token, header.id);
      } else if (packetSize == sizeof(UDPMessage)) {
        UDPMessage message{};
        deviceUDP.read(reinterpret_cast<uint8_t*>(&message), packetSize);
        message.header.name[sizeof(message.header.name) - 1] = '\0';
        message.versionStr.s[sizeof(message.versionStr.s) - 1] = '\0';
        message.build[sizeof(message.build) - 1] = '\0';
        if (message.header.token == 255 && message.header.id == 1)
          updateDevices(message, deviceUDP.remoteIP());
        else
          EXT_LOGW(MB_TAG, "Bad MoonLight header from ...%d", deviceUDP.remoteIP()[3]);
      } else {
        EXT_LOGW(MB_TAG, "Unknown packet size on port %d: %d (WLED=%d ML=%d)",
                 deviceUDPPort, packetSize, sizeof(UDPWLEDHeader), sizeof(UDPMessage));
        deviceUDP.clear();
      }
    }

    // Control port 65507: MoonLight-only control messages
    while (size_t packetSize = deviceControlUDP.parsePacket()) {
      if (packetSize != sizeof(UDPControlMessage)) {
        EXT_LOGW(MB_TAG, "Bad control packet size: %d (expected %d)", packetSize, sizeof(UDPControlMessage));
        deviceControlUDP.clear();
        continue;
      }
      UDPControlMessage msg{};
      deviceControlUDP.read(reinterpret_cast<uint8_t*>(&msg), packetSize);
      msg.header.name[sizeof(msg.header.name) - 1] = '\0';
      msg.targetName[sizeof(msg.targetName) - 1] = '\0';
      if (msg.header.token == 255 && msg.header.id == 1)
        processControlMessage(msg);
    }
  }

  // from addUpdateHandler (!group) and loop10s (false)
  void sendUDP(bool isControlCommand) {
    if (isControlCommand) {
      // Group control broadcast on port 65507 — WLED does not listen here
      if (!deviceControlUDPConnected) return;
      UDPControlMessage msg{};
      infoToControlMessage(msg);
      // targetName empty = group broadcast; receivers use partOfGroup(myName, senderName)
      _moduleControl->read([&](ModuleState& state) { controlStateToControlMessage(state.data, msg); }, _moduleName);

      if (deviceControlUDP.beginPacket(IPAddress(255, 255, 255, 255), deviceControlUDPPort)) {
        deviceControlUDP.write(reinterpret_cast<uint8_t*>(&msg), sizeof(msg));
        deviceControlUDP.endPacket();
        // EXT_LOGD(MB_TAG, "UDP control sent: bri=%d pal=%d preset=%d", msg.brightness, msg.palette, msg.preset);
      }
    } else {
      // broadcast own status to all devices
      UDPMessage message{};
      _moduleControl->read([&](ModuleState& state) {
        infoToMessage(message, state.data["lightsOn"]);
        message.brightness = state.data["brightness"];
        message.palette = state.data["palette"];
        message.preset = state.data["preset"]["selected"];
      }, _moduleName);

      if (deviceUDP.beginPacket(IPAddress(255, 255, 255, 255), deviceUDPPort)) {
        deviceUDP.write(reinterpret_cast<uint8_t*>(&message), sizeof(message));
        deviceUDP.endPacket();
        // EXT_LOGD(MB_TAG, "UDP update sent: bri=%d pal=%d preset=%d", message.brightness, message.palette, message.preset);

        // there is no udp received from itself so update manually
        IPAddress activeIP = networkLocalIP();
        // EXT_LOGD(MB_TAG, "UDP packet written (%s -> %d)", message.header.name, activeIP[3]);
        updateDevices(message, activeIP);
      }
    }
  }

 private:
  // Fill the 44-byte WLED-compatible header from local device info
  void infoToHeader(UDPWLEDHeader& header, bool lightsOn) {
    IPAddress localIP = networkLocalIP();
    header.token = 255;
    header.id = 1;
    header.ip0 = localIP[0];
    header.ip1 = localIP[1];
    header.ip2 = localIP[2];
    header.ip3 = localIP[3];
    memset(header.name, 0, sizeof(header.name));
    strlcpy(header.name, esp32sveltekit.getSystemHostname().c_str(), sizeof(header.name));
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    header.type = 34;
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    header.type = 33;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    header.type = 35;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
    header.type = 36;
#else
    header.type = 32;  // ESP32
#endif
    if (lightsOn) header.type |= 0x80;
    header.insId = localIP[3];
    header.version = strtoul(APP_DATE, nullptr, 10);  // YYYYMMDD numeric build date
  }

  void infoToMessage(UDPMessage& message, bool lightsOn) {
    infoToHeader(message.header, lightsOn);
    message.versionStr = APP_VERSION;
    memset(message.build, 0, sizeof(message.build));  // init with 0
    strlcpy(message.build, APP_DATE, sizeof(message.build));
    message.uptime = time(nullptr) ? time(nullptr) - pal::millis() / 1000 : pal::millis() / 1000;
    message.packageSize = sizeof(message);
  }

  void infoToControlMessage(UDPControlMessage& msg) {
    infoToHeader(msg.header, false);  // lightsOn not encoded in control sender header
    memset(msg.targetName, 0, sizeof(msg.targetName));
  }

  // Copy control fields from control message to control state (for applying updates)
  void messageToControlState(const UDPControlMessage& msg, JsonObject& newState) {
    newState["lightsOn"] = (bool)msg.lightsOn;
    newState["brightness"] = msg.brightness;
    newState["palette"] = msg.palette;
    _moduleControl->read([&](ModuleState& state) { newState["preset"] = state.data["preset"]; }, String(_moduleName) + __FUNCTION__);
    newState["preset"]["action"] = "click";
    newState["preset"]["select"] = msg.preset;
  }

  // Copy control fields from control state to control message (for broadcasting)
  void controlStateToControlMessage(const JsonObject& state, UDPControlMessage& msg) {
    msg.lightsOn = state["lightsOn"];
    msg.brightness = state["brightness"];
    msg.palette = state["palette"];
    msg.preset = state["preset"]["selected"];
  }

  // Copy control fields from device object to control message (for sending control)
  void deviceToControlMessage(const JsonObject& device, UDPControlMessage& msg) {
    msg.lightsOn = device["lightsOn"];
    msg.brightness = device["brightness"];
    msg.palette = device["palette"];
    msg.preset = device["preset"];
  }

  void finaliseDeviceUpdate(JsonDocument& doc, JsonArray& devices, bool newDevice) {
    if (newDevice) {  // sort devices in vector and add to a new document and update
      std::vector<JsonObject> devicesVector;
      for (JsonObject dev : devices) {
        if (time(nullptr) - dev["lastSync"].as<time_t>() < 86400) devicesVector.push_back(dev);  // max 1 day
      }
      std::sort(devicesVector.begin(), devicesVector.end(), [](JsonObject a, JsonObject b) {
        // Primary sort: by name
        if (a["name"] != b["name"]) return a["name"] < b["name"];
        // Tie-breaker: by IP address (ensures stable sort)
        return a["ip"] < b["ip"];
      });
      JsonDocument doc2;
      doc2["devices"].to<JsonArray>();
      for (JsonObject device : devicesVector) doc2["devices"].add(device);
      JsonObject newState = doc2.as<JsonObject>();
      update(newState, ModuleState::update, _moduleName);
    } else {
      // only update the updated device
      JsonObject newState = doc.as<JsonObject>();
      update(newState, ModuleState::update, _moduleName);
    }
  }

  bool partOfGroup(const String& base, const String& device, int level = 0) {
    EXT_LOGV(MB_TAG, "partOfGroup %s %s level:%d", base.c_str(), device.c_str(), level);

    // Count hyphens from the end: level 0 = last hyphen, level 1 = second-last, etc.
    int pos = base.length();
    for (int i = 0; i <= level; i++) {
      pos = base.lastIndexOf('-', pos - 1);
      if (pos == -1) {
        return base == device;  // Not enough hyphens at this level - only exact match (no group)
      }
    }

    // Require the character at pos to be '-' so "kitchenette" does not match the "kitchen" group
    return device.startsWith(base.substring(0, pos)) &&
           device.length() > (size_t)pos &&
           device.charAt(pos) == '-';
  }
};

#endif
#endif
