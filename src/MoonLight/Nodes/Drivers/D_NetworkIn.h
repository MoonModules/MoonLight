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
  Char<32> status = "Not connected";
  unsigned long lastPacketMs = 0;
  bool statusReceiving = false;

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
    addControl(status, "status", "text", 0, 32, true);
  }

  void onUpdate(const JsonObject& control) override {
    if (control["name"] == "protocol") {
      if (protocol == 0)
        port = 6454;
      else if (protocol == 1)
        port = 4048;
      else if (protocol == 2)
        port = 5568;
      updateControl("port", port);
      // restart UDP listener on the new port
      if (init) {
        udp.stop();
        init = false;
      }
    } else if (control["name"] == "port") {
      if (init) {
        udp.stop();
        init = false;
      }
    }
  }

  bool init = false;

  void loop() override {
    const char* proto = protocol == 0 ? "Art-Net" : protocol == 1 ? "DDP" : "E1.31";

    if (!networkIsConnected()) {
      if (init) {
        EXT_LOGI(ML_TAG, "Stop Listening for %s on port %d", proto, port);
        udp.stop();
        init = false;
        status = "Not connected";
        updateControl("status", status);
        statusReceiving = false;
      }
      return;
    }

    if (!init) {
      udp.begin(port);
      EXT_LOGI(ML_TAG, "Listening for %s on port %d", proto, port);
      init = true;
      status.format("Listening %s:%d", proto, port);
      updateControl("status", status);
      statusReceiving = false;
    }

    while (int packetSize = udp.parsePacket()) {
      static constexpr int DDP_HEADER_LEN = 10;
      const int minHeader = (protocol == 1)   ? DDP_HEADER_LEN
                            : (protocol == 2) ? 126  // E1.31 DMP layer starts at byte 126
                                              : static_cast<int>(sizeof(ArtNetHeader));
      if (packetSize < minHeader || packetSize > static_cast<int>(sizeof(packetBuffer))) {
        udp.clear();
        continue;
      }

      udp.read(packetBuffer, MIN(packetSize, sizeof(packetBuffer)));
      lastPacketMs = millis();

      if (protocol == 0)
        handleArtNet(packetSize);
      else if (protocol == 1)
        handleDDP(packetSize);
      else if (protocol == 2)
        handleE131(packetSize);
    }

    // Update status on receiving-state transitions only (cheap — no per-frame updateControl)
    bool nowReceiving = (lastPacketMs > 0 && millis() - lastPacketMs < 3000);
    if (nowReceiving != statusReceiving) {
      statusReceiving = nowReceiving;
      if (nowReceiving)
        status.format("Receiving %s:%d", proto, port);
      else
        status.format("Listening %s:%d", proto, port);
      updateControl("status", status);
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
    uint16_t length;  // DMX data length
  };

  // DDP header is exactly 10 bytes on the wire: flags(1) seq(1) type(1) id(1) offset(4BE) dataLen(2BE)
  // Do NOT use a struct — trailing alignment padding would make sizeof == 12, mis-indexing the payload.

  // -----------------------------------------------------------------------
  // Pixel write helper — shared by all three protocols
  // -----------------------------------------------------------------------

  void writePixels(int startPixel, int numPixels, uint8_t* dmxData) {
    if (startPixel < 0) return;

    // Resolve bounds against the target layer's own light count.
    // Physical layer: clamp to nrOfLights (physical).
    // Virtual layer:  clamp to vLayer->nrOfLights (virtual) — physical count may be
    //                 smaller when a modifier expands the virtual grid, so clamping to
    //                 physical would silently drop valid virtual indices.
    VirtualLayer* vLayer = nullptr;
    nrOfLights_t maxLights;
    if (layer == 0) {
      maxLights = layerP.lights.header.nrOfLights;
    } else {
      if (layer - 1 >= layerP.layers.size() || !layerP.layers[layer - 1]) return;
      vLayer = layerP.layers[layer - 1];
      // If no virtual mapping exists (no effects → onLayoutPre skipped → mappingTableSize==0),
      // nrOfLights stays at the default (256). Use physical bounds to prevent buffer overflow.
      maxLights = (vLayer->mappingTableSize > 0) ? vLayer->nrOfLights : layerP.lights.header.nrOfLights;
    }

    EXT_LOGD(ML_TAG, "%d %d %d %d %p", startPixel, maxLights, MIN(numPixels, (int)(maxLights - startPixel)), layer, (void*)dmxData);

    if (startPixel >= (int)maxLights) return;
    numPixels = MIN(numPixels, (int)(maxLights - startPixel));
    if (numPixels <= 0) return;

    xSemaphoreTake(swapMutex, portMAX_DELAY);
    for (int i = 0; i < numPixels; i++) {
      int ledIndex = startPixel + i;
      if (layer == 0) {  // Physical layer — write directly to channelsD (bypasses compositing)
        memcpy(&layerP.lights.channelsD[ledIndex * layerP.lights.header.channelsPerLight], &dmxData[i * layerP.lights.header.channelsPerLight], layerP.lights.header.channelsPerLight);
      } else {  // Virtual layer — write to virtualChannels so compositeTo() maps it to channelsD
        if (vLayer->virtualChannels && (nrOfLights_t)ledIndex < vLayer->nrOfLights)
          memcpy(&vLayer->virtualChannels[ledIndex * layerP.lights.header.channelsPerLight], &dmxData[i * layerP.lights.header.channelsPerLight], layerP.lights.header.channelsPerLight);
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
    static constexpr int DDP_HEADER_LEN = 10;
    if (packetSize < DDP_HEADER_LEN) return;

    // Explicit byte indexing — avoids struct padding and unaligned multi-byte reads on ESP32
    uint8_t dataType = packetBuffer[2];
    // Accept both de-facto values (0x01 RGB, 0x1A RGBW — used by WLED ecosystem) and
    // strict-spec values (0x0B RGB, 0x1B RGBW — TTT/SSS bit fields per 3waylabs spec).
    const bool isRGBW = (dataType == 0x1A || dataType == 0x1B);
    const bool isRGB = (dataType == 0x01 || dataType == 0x0B);
    if (!isRGB && !isRGBW) return;

    // Reject if the packet's pixel stride doesn't match the configured channel layout.
    // DDP offset is an absolute byte offset into the receiver's channel memory, so the
    // stride used here must equal channelsPerLight — otherwise pixel positions are wrong.
    const uint8_t packetBytesPerPixel = isRGBW ? 4 : 3;
    const uint32_t channelsPerLight = static_cast<uint32_t>(layerP.lights.header.channelsPerLight);
    if (packetBytesPerPixel != channelsPerLight) return;

    uint32_t offset = ((uint32_t)packetBuffer[4] << 24) | ((uint32_t)packetBuffer[5] << 16) | ((uint32_t)packetBuffer[6] << 8) | (uint32_t)packetBuffer[7];
    // Reject mid-pixel offsets: a non-aligned offset shifts all pixel boundaries and
    // causes silent color corruption. Compliant senders always use pixel-aligned offsets.
    if (offset % channelsPerLight != 0) return;
    uint16_t dataLen = ((uint16_t)packetBuffer[8] << 8) | packetBuffer[9];

    uint8_t* pixelData = packetBuffer + DDP_HEADER_LEN;
    int payloadBytes = packetSize - DDP_HEADER_LEN;
    int safeDataLen = MIN(static_cast<int>(dataLen), payloadBytes);

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

    uint16_t e131Base = universeMin > 0 ? universeMin : 1;  // E1.31 universes are 1-based; universe 1 → pixel 0
    int startPixel = (universe - e131Base) * (512 / layerP.lights.header.channelsPerLight);
    int numPixels = safeDataLen / layerP.lights.header.channelsPerLight;
    writePixels(startPixel, numPixels, dmxData);
  }
};

#endif
