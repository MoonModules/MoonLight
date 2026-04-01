/**
    @title     MoonLight
    @file      D_NetworkIn.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

extern SemaphoreHandle_t swapMutex;

class NetworkInDriver : public Node {
 public:
  static const char* name() { return "Network In"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }
  static const char* category() { return "Driver"; }

  NetworkUDP udp;
  uint8_t packetBuffer[1500];

  uint8_t protocol = 0;  // 0=Art-Net, 1=DDP, 2=E1.31
  uint8_t layer = 1;     // Physical is 0, virtual layer 0 (shown as 1) is 1 by default
  uint16_t port = 6454;
  uint16_t universeMin = 0;
  uint16_t universeMax = 32767;

  void setup() override {
    addControl(protocol, "protocol", "select");
    addControlValue("Art-Net");
    addControlValue("DDP");
    addControlValue("E1.31");
    addControl(port, "port", "number", 0, 65535);
    addControl(universeMin, "universeMin", "number", 0, 32767);
    addControl(universeMax, "universeMax", "number", 0, 32767);
    addControl(layer, "layer", "select");
    addControlValue("Physical layer");
    uint8_t i = 1;  // start with one
    for (VirtualLayer* vLayer : layerP.layers) {
      Char<32> layerName;
      layerName.format("Layer %d", i);
      addControlValue(layerName.c_str());
      i++;
    }
  }

  void onUpdate(const JsonObject& control) override {
    if (control["name"] == "protocol") {
      if      (protocol == 0) port = 6454;
      else if (protocol == 1) port = 4048;
      else if (protocol == 2) port = 5568;
      updateControl("port", port);
      // restart UDP listener on the new port
      if (init) { udp.stop(); init = false; }
    }
  }

  bool init = false;

  void loop() override {
    if (!networkIsConnected()) {
      if (init) {
        EXT_LOGI(ML_TAG, "Stop Listening for %s on port %d", protocol == 0 ? "Art-Net" : protocol == 1 ? "DDP" : "E1.31", port);
        udp.stop();
        init = false;
      }
      return;
    }

    if (!init) {
      udp.begin(port);
      EXT_LOGI(ML_TAG, "Listening for %s on port %d", protocol == 0 ? "Art-Net" : protocol == 1 ? "DDP" : "E1.31", port);
      init = true;
    }

    while (int packetSize = udp.parsePacket()) {
      const int minHeader = (protocol == 1) ? static_cast<int>(sizeof(DDPHeader))
                          : (protocol == 2) ? 126  // E1.31 DMP layer starts at byte 126
                                            : static_cast<int>(sizeof(ArtNetHeader));
      if (packetSize < minHeader || packetSize > static_cast<int>(sizeof(packetBuffer))) {
        udp.clear();
        continue;
      }

      udp.read(packetBuffer, MIN(packetSize, sizeof(packetBuffer)));

      if      (protocol == 0) handleArtNet(packetSize);
      else if (protocol == 1) handleDDP(packetSize);
      else if (protocol == 2) handleE131(packetSize);
    }
  }

  // -----------------------------------------------------------------------
  // Packet structures
  // -----------------------------------------------------------------------

  struct ArtNetHeader {
    char id[8];        // "Art-Net\0"
    uint16_t opcode;   // 0x5000 for DMX data
    uint16_t version;  // Protocol version
    uint8_t sequence;
    uint8_t physical;
    uint16_t universe;
    uint16_t length;   // DMX data length
  };

  struct DDPHeader {
    uint8_t flags;
    uint8_t sequence;
    uint8_t dataType;
    uint8_t id;
    uint32_t offset;
    uint16_t dataLen;
  };

  // -----------------------------------------------------------------------
  // Pixel write helper — shared by all three protocols
  // -----------------------------------------------------------------------

  void writePixels(int startPixel, int numPixels, uint8_t* dmxData) {
    if (startPixel < 0 || startPixel >= layerP.lights.header.nrOfLights) return;
    numPixels = MIN(numPixels, layerP.lights.header.nrOfLights - startPixel);
    if (numPixels <= 0) return;

    xSemaphoreTake(swapMutex, portMAX_DELAY);
    for (int i = 0; i < numPixels; i++) {
      int ledIndex = startPixel + i;
      if (layer == 0) {  // Physical layer
        memcpy(&layerP.lights.channelsD[ledIndex * layerP.lights.header.channelsPerLight],
               &dmxData[i * layerP.lights.header.channelsPerLight],
               layerP.lights.header.channelsPerLight);
      } else {  // Virtual layer — guard against a slot that hasn't been created yet
        if (layerP.layers[layer - 1]) {
          layerP.layers[layer - 1]->forEachLightIndex(ledIndex, [&](nrOfLights_t indexP) {
            memcpy(&layerP.lights.channelsD[indexP * layerP.lights.header.channelsPerLight],
                   &dmxData[i * layerP.lights.header.channelsPerLight],
                   layerP.lights.header.channelsPerLight);
          });
        }
      }
    }
    xSemaphoreGive(swapMutex);
  }

  // -----------------------------------------------------------------------
  // Protocol handlers
  // -----------------------------------------------------------------------

  void handleArtNet(int packetSize) {
    if (packetSize < static_cast<int>(sizeof(ArtNetHeader))) return;
    if (memcmp(packetBuffer, "Art-Net", 7) != 0) return;

    ArtNetHeader* header = reinterpret_cast<ArtNetHeader*>(packetBuffer);
    if (header->opcode != 0x5000) return;  // not ArtDMX

    uint16_t universe = header->universe;
    if (universe < universeMin || universe > universeMax) return;

    uint16_t dataLength = (header->length >> 8) | (header->length << 8);
    int payloadBytes = packetSize - static_cast<int>(sizeof(ArtNetHeader));
    int safeDataLen = MIN(static_cast<int>(dataLength), payloadBytes);

    uint8_t* dmxData = packetBuffer + sizeof(ArtNetHeader);
    int startPixel = (universe - universeMin) * (512 / layerP.lights.header.channelsPerLight);
    int numPixels = safeDataLen / layerP.lights.header.channelsPerLight;
    writePixels(startPixel, numPixels, dmxData);
  }

  void handleDDP(int packetSize) {
    if (packetSize < static_cast<int>(sizeof(DDPHeader))) return;

    DDPHeader* header = reinterpret_cast<DDPHeader*>(packetBuffer);
    uint8_t dataType = header->dataType;
    if (dataType != 0x01 && dataType != 0x1A) return;  // accept RGB24 and RGBW32

    uint32_t offset = (header->offset >> 24) | ((header->offset >> 8) & 0xFF00) | ((header->offset << 8) & 0xFF0000) | (header->offset << 24);
    uint16_t dataLen = (header->dataLen >> 8) | (header->dataLen << 8);

    uint8_t* pixelData = packetBuffer + sizeof(DDPHeader);
    int payloadBytes = packetSize - static_cast<int>(sizeof(DDPHeader));
    int safeDataLen = MIN(static_cast<int>(dataLen), payloadBytes);

    const uint32_t channelsPerLight = static_cast<uint32_t>(layerP.lights.header.channelsPerLight);
    const uint32_t startPixelU = offset / channelsPerLight;
    if (startPixelU >= static_cast<uint32_t>(layerP.lights.header.nrOfLights)) return;

    writePixels(static_cast<int>(startPixelU), safeDataLen / static_cast<int>(channelsPerLight), pixelData);
  }

  void handleE131(int packetSize) {
    if (packetSize < 126) return;
    // Verify ACN packet identifier at offset 4
    if (memcmp(packetBuffer + 4, "ASC-E1.17", 9) != 0) return;

    // Universe at offset 113–114 (big-endian)
    uint16_t universe = ((uint16_t)packetBuffer[113] << 8) | packetBuffer[114];
    if (universe < universeMin || universe > universeMax) return;

    // property_value_count at offset 123–124 (big-endian); includes the DMX start code byte
    uint16_t propCount = ((uint16_t)packetBuffer[123] << 8) | packetBuffer[124];
    if (propCount < 1) return;
    uint16_t dataLength = propCount - 1;  // exclude start code

    // DMX data starts at offset 126 (property_values[1], after start code at 125)
    uint8_t* dmxData = packetBuffer + 126;
    int payloadBytes = packetSize - 126;
    int safeDataLen = MIN(static_cast<int>(dataLength), payloadBytes);

    int startPixel = (universe - universeMin) * (512 / layerP.lights.header.channelsPerLight);
    int numPixels = safeDataLen / layerP.lights.header.channelsPerLight;
    writePixels(startPixel, numPixels, dmxData);
  }
};

#endif
