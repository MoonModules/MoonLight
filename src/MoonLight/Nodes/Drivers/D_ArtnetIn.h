/**
    @title     MoonLight
    @file      ArtnetIn.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class ArtNetInDriver : public Node {
 public:
  static const char* name() { return "Art-Net In"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  NetworkUDP artnetUdp;
  uint8_t packetBuffer[1500];

  bool ddp = false;
  uint8_t layer = 1;  // Physical is 0, virtual layer 0 (shown as 1) is 1 by default
  uint16_t port = 6454;
  uint16_t universeMin = 0;
  uint16_t universeMax = 32767;

  void setup() override {
    addControl(ddp, "DDP", "checkbox");
    addControl(port, "port", "number", 0, 65538);
    addControl(universeMin, "universeMin", "number", 0, 32767);
    addControl(universeMax, "universeMax", "number", 0, 32767);
    addControl(layer, "layer", "select");
    addControlValue("Physical layer");
    uint8_t i = 1;  // start with one
    for (VirtualLayer* layer : layerP.layers) {
      Char<32> layerName;
      layerName.format("Layer %d", i);
      addControlValue(layerName.c_str());
      i++;
    }
  }

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    // add your custom onUpdate code here
    if (control["name"] == "DDP") {
      if (control["value"] == 0) {
      }
    }
  }

  bool init = false;

  void loop() override {
    if (!WiFi.localIP() && !ETH.localIP()) {
      if (init) {
        EXT_LOGI(ML_TAG, "Stop Listening for %s on port %d", ddp ? "DDP" : "Art-Net", port);
        artnetUdp.stop();
        init = false;
      }
      return;
    }

    if (!init) {
      artnetUdp.begin(port);
      EXT_LOGI(ML_TAG, "Listening for %s on port %d", ddp ? "DDP" : "Art-Net", port);
      init = true;
    }

    while (int packetSize = artnetUdp.parsePacket()) {
      if (packetSize < sizeof(ArtNetHeader) || packetSize > sizeof(packetBuffer)) {
        artnetUdp.clear();  // drains all available packets
        continue;
      }

      artnetUdp.read(packetBuffer, min(packetSize, (int)sizeof(packetBuffer)));

      if (ddp)
        handleDDP();
      else
        handleArtNet();
    }
  }

  // Art-Net Configuration
  // uint16_t artnetUniverse = 0;

  // Art-Net packet structure
  struct ArtNetHeader {
    char id[8];        // "Art-Net\0"
    uint16_t opcode;   // 0x5000 for DMX data
    uint16_t version;  // Protocol version
    uint8_t sequence;
    uint8_t physical;
    uint16_t universe;
    uint16_t length;  // DMX data length
  };

  // DDP packet structure
  struct DDPHeader {
    uint8_t flags;     // Bit 7: Push, Bit 6: Query, Bits 5-4: Timecode, Bits 3-0: reserved
    uint8_t sequence;  // Rolling sequence number
    uint8_t dataType;  // 0x01 = RGB data
    uint8_t id;        // Destination ID (0 = all)
    uint32_t offset;   // Byte offset into LED array
    uint16_t dataLen;  // Length of data in bytes
  };

  void handleArtNet() {
    // Verify Art-Net packet
    if (memcmp(packetBuffer, "Art-Net", 7) == 0) {
      ArtNetHeader* header = (ArtNetHeader*)packetBuffer;
      uint16_t opcode = header->opcode;

      // EXT_LOGD(ML_TAG, "size:%d universe:%d", packetSize, header->universe);

      // Check if it's a DMX packet (opcode 0x5000)
      if (opcode == 0x5000) {
        uint16_t universe = header->universe;
        if (universe >= universeMin && universe <= universeMax) {
          uint16_t dataLength = (header->length >> 8) | (header->length << 8);

          uint8_t* dmxData = packetBuffer + sizeof(ArtNetHeader);

          int startPixel = (universe - universeMin) * (512 / layerP.lights.header.channelsPerLight);
          int numPixels = min((uint16_t)(dataLength / layerP.lights.header.channelsPerLight), (uint16_t)(layerP.lights.header.nrOfLights - startPixel));

          for (int i = 0; i < numPixels; i++) {
            int ledIndex = startPixel + i;
            if (ledIndex < layerP.lights.header.nrOfLights) {
              if (layer == 0) {  // Physical layer
                memcpy(&layerP.lights.channelsE[ledIndex * layerP.lights.header.channelsPerLight], &dmxData[i * layerP.lights.header.channelsPerLight], layerP.lights.header.channelsPerLight);
              } else {  // Virtual layer
                layerP.layers[layer - 1]->setLight(ledIndex, &dmxData[i * layerP.lights.header.channelsPerLight], 0, layerP.lights.header.channelsPerLight);
              }
            }
          }
        }
      }
    }
  }

  void handleDDP() {
    DDPHeader* header = (DDPHeader*)packetBuffer;

    // bool pushFlag = (header->flags & 0x80) != 0;
    uint8_t dataType = header->dataType;

    uint32_t offset = (header->offset >> 24) | ((header->offset >> 8) & 0xFF00) | ((header->offset << 8) & 0xFF0000) | (header->offset << 24);
    uint16_t dataLen = (header->dataLen >> 8) | (header->dataLen << 8);

    if (dataType == 0x01) {
      uint8_t* pixelData = packetBuffer + sizeof(DDPHeader);

      int startPixel = offset / layerP.lights.header.channelsPerLight;
      int numPixels = min((uint16_t)(dataLen / layerP.lights.header.channelsPerLight), (uint16_t)(layerP.lights.header.nrOfLights - startPixel));

      for (int i = 0; i < numPixels; i++) {
        int ledIndex = startPixel + i;
        if (ledIndex < layerP.lights.header.nrOfLights) {
          if (layer == 0) {  // Physical layer
            memcpy(&layerP.lights.channelsE[ledIndex * layerP.lights.header.channelsPerLight], &pixelData[i * layerP.lights.header.channelsPerLight], layerP.lights.header.channelsPerLight);
          } else {  // Virtual layer
            layerP.layers[layer - 1]->setLight(ledIndex, &pixelData[i * layerP.lights.header.channelsPerLight], 0, layerP.lights.header.channelsPerLight);
          }
        }
      }
    }
  }
};

#endif
