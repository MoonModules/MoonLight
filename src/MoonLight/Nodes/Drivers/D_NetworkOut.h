/**
    @title     MoonLight
    @file      D_NetworkOut.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include <AsyncUDP.h>
  #include <WiFi.h>

  // DDP constants
  #define DDP_HEADER_LEN          10
  #define DDP_DEFAULT_PORT        4048
  #define DDP_FLAGS1_VER1         0x40
  #define DDP_FLAGS1_PUSH         0x01
  #define DDP_ID_DISPLAY          1
  #define DDP_TYPE_RGB24          0x01   // 3 channels/pixel
  #define DDP_TYPE_RGBW32         0x1A   // 4 channels/pixel (bits 5:3 = 011)
  #define DDP_CHANNELS_PER_PACKET 1440

  // E1.31 / sACN constants
  #define E131_DEFAULT_PORT       5568
  #define E131_DMP_DATA           125    // byte offset of property_values[0] (DMX start code)
  #define E131_HEADER_LEN         126    // bytes before DMX channel data

class ArtNetOutDriver : public DriverNode {
 public:
  static const char* name() { return "Art-Net Out"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }
  static const char* category() { return "Driver"; }

  uint8_t protocol = 0;           // 0=Art-Net, 1=DDP, 2=E1.31
  bool broadcast = false;         // Art-Net only: send to subnet broadcast instead of unicast IPs
  Char<32> controllerIP3s = "11";
  uint16_t port = 6454;           // auto-updated when protocol changes
  uint8_t FPSLimiter = 50;
  uint16_t universeSize = 512;    // Art-Net / E1.31: bytes per universe (max 512)
  uint8_t nrOfOutputsPerIP = 1;
  uint8_t universesPerOutput = 1;
  uint16_t channelsPerOutput = 1024;

  uint16_t usedChannelsPerUniverse = 510;  // calculated
  uint16_t totalUniverses = 0;             // calculated
  uint32_t totalChannels = 0;              // calculated

  // -----------------------------------------------------------------------
  // Setup
  // -----------------------------------------------------------------------

  void setup() override {
    DriverNode::setup();

    addControl(protocol, "protocol", "select");
    addControlValue("Art-Net");
    addControlValue("DDP");
    addControlValue("E1.31");
    addControl(broadcast, "broadcast", "checkbox");
    addControl(controllerIP3s, "controllerIPs", "text", 0, 32);
    addControl(port, "port", "number", 0, 65535);
    addControl(FPSLimiter, "Limiter", "number", 1, 255, false, "FPS");
    addControl(universeSize, "universeSize", "number", 0, 512);
    addControl(usedChannelsPerUniverse, "usedChannels", "number", 1, 1000, true);
    addControl(nrOfOutputsPerIP, "#Outputs per IP", "number", 0, 255);
    addControl(universesPerOutput, "universesPerOutput", "number", 0, 255);
    addControl(totalUniverses, "totalUniverses", "number", 0, 65538, true);
    addControl(channelsPerOutput, "channelsPerOutput", "number", 0, 65538);
    addControl(totalChannels, "totalChannels", "number", 0, 390000, true);

    setupArtNetHeader();
  }

  uint8_t ipAddresses[16];
  uint8_t nrOfIPAddresses = 0;

  // -----------------------------------------------------------------------
  // onUpdate
  // -----------------------------------------------------------------------

  void onUpdate(const JsonObject& control) override {
    DriverNode::onUpdate(control);

    if (control["name"] == "protocol") {
      if      (protocol == 0) { port = 6454;             setupArtNetHeader(); }
      else if (protocol == 1) { port = DDP_DEFAULT_PORT;                      }
      else if (protocol == 2) { port = E131_DEFAULT_PORT; setupE131Header();  }
      updateControl("port", port);
      moduleNodes->requestUIUpdate = true;
    }

    if (control["name"] == "controllerIPs") {
      nrOfIPAddresses = 0;
      size_t index = controllerIP3s.indexOf("-");
      EXT_LOGD(MB_TAG, "IPs: %s (%d)", controllerIP3s.c_str(), index);
      if (index != SIZE_MAX) {
        Char<8> first = controllerIP3s.substring(0, index);
        Char<8> second = controllerIP3s.substring(index + 1);
        for (int ipSegment = first.toInt(); ipSegment <= second.toInt(); ipSegment++) {
          if (nrOfIPAddresses < std::size(ipAddresses) && ipSegment >= 0 && ipSegment <= 255) {
            EXT_LOGD(MB_TAG, "Found IP: %s-%s %d (%d)", first.c_str(), second.c_str(), ipSegment, nrOfIPAddresses);
            ipAddresses[nrOfIPAddresses] = ipSegment;
            nrOfIPAddresses++;
          }
        }
      } else {
        controllerIP3s.split(",", [this](const char* token, uint8_t nr) {
          int ipSegment = atoi(token);
          if (nrOfIPAddresses < std::size(ipAddresses) && ipSegment >= 0 && ipSegment <= 255) {
            EXT_LOGD(MB_TAG, "Found IP: %s (%d / %d)", token, nr, nrOfIPAddresses);
            ipAddresses[nrOfIPAddresses] = ipSegment;
            nrOfIPAddresses++;
          } else
            EXT_LOGW(MB_TAG, "Too many IPs provided (%d) or invalid IP segment: %d ", nrOfIPAddresses, ipSegment);
        });
      }
    }

    totalChannels = layerP.lights.header.nrOfLights * layerP.lights.header.channelsPerLight / (nrOfIPAddresses ? nrOfIPAddresses : 1);
    usedChannelsPerUniverse = universeSize / layerP.lights.header.channelsPerLight * layerP.lights.header.channelsPerLight;
    totalUniverses = (totalChannels + usedChannelsPerUniverse - 1) / usedChannelsPerUniverse;

    updateControl("usedChannels", usedChannelsPerUniverse);
    updateControl("totalUniverses", totalUniverses);
    updateControl("totalChannels", totalChannels);
    moduleNodes->requestUIUpdate = true;

    EXT_LOGD(ML_TAG, "c/u:%d #u:%d #c%d (%d)", usedChannelsPerUniverse, totalUniverses, totalChannels, totalUniverses * usedChannelsPerUniverse);
  }

  // -----------------------------------------------------------------------
  // Shared state
  // packet_buffer layout:
  //   Art-Net: [0..17] header, [18..] DMX data          (max ~530 bytes)
  //   DDP:     [0..9]  header, [10..] channel data       (max 1450 bytes)
  //   E1.31:   [0..125] header, [126..] DMX channel data (max 638 bytes)
  // -----------------------------------------------------------------------

  uint8_t packet_buffer[DDP_HEADER_LEN + DDP_CHANNELS_PER_PACKET];  // 1450 bytes — fits all protocols

  size_t sequenceNumber = 0;  // ArtNet: masks to 1-254, DDP: & 0x0F, E1.31: 0-255
  AsyncUDP udp;
  unsigned long lastSendTime = 0;
  bool blackFrameSent = false;  // true after sending one all-zero frame when all layers are black

  // shared loop variables (reset at the start of each loopArtNet / loopE131 call)
  uint_fast16_t universe = 0;
  uint_fast16_t packetSize = 0;
  uint_fast16_t channels_remaining = 0;
  uint8_t processedOutputs = 0;
  IPAddress controllerIP;

  // -----------------------------------------------------------------------
  // Art-Net
  // -----------------------------------------------------------------------

  void setupArtNetHeader() {
    memcpy(packet_buffer,
           (uint8_t[]){
               'A', 'r', 't', '-', 'N', 'e', 't', 0x00, 0x00, 0x50,  // OpCode ArtDMX (little-endian)
               0x00, 0x0e,                                            // ProtVer 14
               0x00,                                                  // Sequence (filled per frame)
               0x00,                                                  // Physical input port
               0x00, 0x00,                                            // Universe (filled per packet)
               0x00, 0x00                                             // Length (filled per packet)
           },
           18);
  }

  bool sendArtNetPacket() {
    packet_buffer[14] = universe;
    packet_buffer[15] = universe >> 8;
    packet_buffer[16] = packetSize >> 8;
    packet_buffer[17] = packetSize;

    if (!udp.writeTo(packet_buffer, MIN(packetSize, universeSize) + 18, controllerIP, port))
      return false;

    packetSize = 0;
    universe++;
    return true;
  }

  // Broadcast ArtSync (OpCode 0x5200) after each frame so all receivers display simultaneously.
  void sendArtSync() {
    uint8_t syncPacket[14] = {
        'A', 'r', 't', '-', 'N', 'e', 't', 0x00,  // ID
        0x00, 0x52,                                  // OpSync (little-endian)
        0x00, 0x0e,                                  // ProtVer 14
        0x00, 0x00                                   // AuxData
    };
    IPAddress broadcastIP = networkLocalIP();
    broadcastIP[3] = 255;
    udp.writeTo(syncPacket, sizeof(syncPacket), broadcastIP, port);
  }

  void loopArtNet(LightsHeader* header) {
    packet_buffer[12] = (sequenceNumber++ % 254) + 1;

    universe = 0;
    packetSize = 0;
    channels_remaining = channelsPerOutput;
    processedOutputs = 0;
    uint8_t actualIPIndex = 0;

    controllerIP = networkLocalIP();
    controllerIP[3] = broadcast ? 255 : ipAddresses[actualIPIndex];
    if (!controllerIP) return;

    for (int indexP = 0; indexP < header->nrOfLights; indexP++) {
      uint8_t* p = &packet_buffer[packetSize + 18];
      uint8_t* c = &layerP.lights.channelsD[indexP * header->channelsPerLight];

      memcpy(p, c, header->channelsPerLight);
      rgbwBufferMapping(p + header->offsetRGBW, c + header->offsetRGBW);
      if (header->offsetRGBW1 != UINT8_MAX) {
        rgbwBufferMapping(p + header->offsetRGBW1, c + header->offsetRGBW1);
        if (header->offsetRGBW2 != UINT8_MAX) {
          rgbwBufferMapping(p + header->offsetRGBW2, c + header->offsetRGBW2);
          if (header->offsetRGBW3 != UINT8_MAX)
            rgbwBufferMapping(p + header->offsetRGBW3, c + header->offsetRGBW3);
        }
      }

      if (header->lightPreset == 9 && indexP < 72)  // RGBWYP: mix of 4 and 6 channels
        packetSize += 4;
      else
        packetSize += header->channelsPerLight;

      channels_remaining -= header->channelsPerLight;

      if (packetSize + header->channelsPerLight > universeSize || channels_remaining < header->channelsPerLight) {
        if (!sendArtNetPacket()) return;
        addYield(10);

        if (channels_remaining < header->channelsPerLight) {
          channels_remaining = channelsPerOutput;
          while (universe % universesPerOutput != 0) universe++;
          processedOutputs++;
          if (!broadcast && processedOutputs >= nrOfOutputsPerIP) {
            if (actualIPIndex + 1 < nrOfIPAddresses) actualIPIndex++;
            processedOutputs = 0;
            universe = 0;
            controllerIP[3] = ipAddresses[actualIPIndex];
          }
        }
      }
    }

    if (packetSize > 0) sendArtNetPacket();
  }

  // -----------------------------------------------------------------------
  // DDP
  // -----------------------------------------------------------------------

  bool sendDDPPacket(IPAddress& ip, uint32_t channelOffset, uint16_t dataLen, bool push) {
    uint8_t flags = DDP_FLAGS1_VER1 | (push ? DDP_FLAGS1_PUSH : 0);
    uint8_t dataType = (layerP.lights.header.channelsPerLight >= 4) ? DDP_TYPE_RGBW32 : DDP_TYPE_RGB24;

    packet_buffer[0] = flags;
    packet_buffer[1] = sequenceNumber++ & 0x0F;
    packet_buffer[2] = dataType;
    packet_buffer[3] = DDP_ID_DISPLAY;
    packet_buffer[4] = (channelOffset >> 24) & 0xFF;
    packet_buffer[5] = (channelOffset >> 16) & 0xFF;
    packet_buffer[6] = (channelOffset >>  8) & 0xFF;
    packet_buffer[7] =  channelOffset        & 0xFF;
    packet_buffer[8] = (dataLen >> 8) & 0xFF;
    packet_buffer[9] =  dataLen       & 0xFF;

    return udp.writeTo(packet_buffer, DDP_HEADER_LEN + dataLen, ip, port);
  }

  void loopDDP(LightsHeader* header) {
    uint32_t lightsPerIP = header->nrOfLights / nrOfIPAddresses;

    for (uint8_t ipIdx = 0; ipIdx < nrOfIPAddresses; ipIdx++) {
      IPAddress ip = networkLocalIP();
      ip[3] = ipAddresses[ipIdx];
      if (!ip) continue;

      uint32_t lightStart = (uint32_t)ipIdx * lightsPerIP;
      uint32_t lightEnd   = (ipIdx == nrOfIPAddresses - 1) ? header->nrOfLights : lightStart + lightsPerIP;

      uint16_t packetDataLen    = 0;
      uint32_t ddpChannelOffset = 0;

      for (uint32_t indexP = lightStart; indexP < lightEnd; indexP++) {
        uint8_t* dst = &packet_buffer[DDP_HEADER_LEN + packetDataLen];
        uint8_t* src = &layerP.lights.channelsD[indexP * header->channelsPerLight];

        memcpy(dst, src, header->channelsPerLight);
        rgbwBufferMapping(dst + header->offsetRGBW, src + header->offsetRGBW);
        if (header->offsetRGBW1 != UINT8_MAX) {
          rgbwBufferMapping(dst + header->offsetRGBW1, src + header->offsetRGBW1);
          if (header->offsetRGBW2 != UINT8_MAX) {
            rgbwBufferMapping(dst + header->offsetRGBW2, src + header->offsetRGBW2);
            if (header->offsetRGBW3 != UINT8_MAX)
              rgbwBufferMapping(dst + header->offsetRGBW3, src + header->offsetRGBW3);
          }
        }
        packetDataLen += header->channelsPerLight;

        bool isLastLight = (indexP == lightEnd - 1);
        bool packetFull  = (packetDataLen + header->channelsPerLight > DDP_CHANNELS_PER_PACKET);

        if (packetFull || isLastLight) {
          if (!sendDDPPacket(ip, ddpChannelOffset, packetDataLen, /*push=*/isLastLight)) return;
          ddpChannelOffset += packetDataLen;
          packetDataLen = 0;
          addYield(10);
        }
      }
    }
  }

  // -----------------------------------------------------------------------
  // E1.31 / sACN
  // -----------------------------------------------------------------------

  // Pre-fill the static portions of the E1.31 packet header (638 bytes for 512 channels).
  // Dynamic fields (lengths, sequence, universe, channel count, data) are set per packet
  // in sendE131Packet().
  void setupE131Header() {
    memset(packet_buffer, 0, E131_HEADER_LEN);
    // Preamble size = 0x0010 (big-endian)
    packet_buffer[0] = 0x00; packet_buffer[1] = 0x10;
    // ACN Packet Identifier (12 bytes at offset 4)
    memcpy(&packet_buffer[4], "ASC-E1.17\0\0\0", 12);
    // Root vector = 0x00000004 VECTOR_ROOT_E131_DATA (big-endian, offset 18)
    packet_buffer[21] = 0x04;
    // CID (16 bytes at offset 22): leave as zeros (valid; any fixed value works)
    // Frame vector = 0x00000002 VECTOR_E131_DATA_PACKET (big-endian, offset 40)
    packet_buffer[43] = 0x02;
    // Source name (64 bytes at offset 44): null-terminated
    memcpy(&packet_buffer[44], "MoonLight", 9);
    // Priority = 100 (offset 108)
    packet_buffer[108] = 100;
    // DMP vector = 0x02 (offset 117)
    packet_buffer[117] = 0x02;
    // DMP address & data type = 0xa1: relative, range, 8-bit (offset 118)
    packet_buffer[118] = 0xa1;
    // Address increment = 0x0001 big-endian (offset 121–122)
    packet_buffer[122] = 0x01;
    // DMX start code = 0x00 (offset 125 = E131_DMP_DATA)
    packet_buffer[125] = 0x00;
  }

  bool sendE131Packet(IPAddress& ip, uint16_t e131universe, uint8_t seqNum, uint16_t nrOfChannels) {
    uint16_t packetLen = E131_HEADER_LEN + nrOfChannels;

    // root_flength (offset 16–17): PDU length from offset 16, flags 0x70 in high nibble
    uint16_t rootLen  = 0x7000 | (packetLen - 16);
    packet_buffer[16] = rootLen >> 8;  packet_buffer[17] = rootLen & 0xFF;
    // frame_flength (offset 38–39): PDU length from offset 38
    uint16_t frameLen = 0x7000 | (packetLen - 38);
    packet_buffer[38] = frameLen >> 8; packet_buffer[39] = frameLen & 0xFF;
    // sequence_number (offset 111)
    packet_buffer[111] = seqNum;
    // universe (offset 113–114, big-endian; E1.31 universes are 1-based)
    packet_buffer[113] = e131universe >> 8; packet_buffer[114] = e131universe & 0xFF;
    // dmp_flength (offset 115–116): PDU length from offset 115
    uint16_t dmpLen   = 0x7000 | (packetLen - 115);
    packet_buffer[115] = dmpLen >> 8;  packet_buffer[116] = dmpLen & 0xFF;
    // property_value_count (offset 123–124, big-endian): channels + 1 start code
    uint16_t propCount = nrOfChannels + 1;
    packet_buffer[123] = propCount >> 8; packet_buffer[124] = propCount & 0xFF;

    return udp.writeTo(packet_buffer, packetLen, ip, port);
  }

  void loopE131(LightsHeader* header) {
    uint8_t seqNum = (sequenceNumber++ % 254) + 1;
    uint16_t e131universeSize = MIN(universeSize, (uint16_t)512);  // E1.31 max 512 per universe

    uint16_t e131universe = 1;  // E1.31 universes are 1-based
    uint16_t e131packetSize = 0;
    channels_remaining = channelsPerOutput;
    processedOutputs = 0;
    uint8_t actualIPIndex = 0;

    IPAddress ip = networkLocalIP();
    ip[3] = ipAddresses[actualIPIndex];
    if (!ip) return;

    for (int indexP = 0; indexP < header->nrOfLights; indexP++) {
      uint8_t* p = &packet_buffer[E131_HEADER_LEN + e131packetSize];  // DMX data after 126-byte header
      uint8_t* c = &layerP.lights.channelsD[indexP * header->channelsPerLight];

      memcpy(p, c, header->channelsPerLight);
      rgbwBufferMapping(p + header->offsetRGBW, c + header->offsetRGBW);
      if (header->offsetRGBW1 != UINT8_MAX) {
        rgbwBufferMapping(p + header->offsetRGBW1, c + header->offsetRGBW1);
        if (header->offsetRGBW2 != UINT8_MAX) {
          rgbwBufferMapping(p + header->offsetRGBW2, c + header->offsetRGBW2);
          if (header->offsetRGBW3 != UINT8_MAX)
            rgbwBufferMapping(p + header->offsetRGBW3, c + header->offsetRGBW3);
        }
      }

      e131packetSize  += header->channelsPerLight;
      channels_remaining -= header->channelsPerLight;

      if (e131packetSize + header->channelsPerLight > e131universeSize || channels_remaining < header->channelsPerLight) {
        if (!sendE131Packet(ip, e131universe, seqNum, e131packetSize)) return;
        e131packetSize = 0;
        e131universe++;
        addYield(10);

        if (channels_remaining < header->channelsPerLight) {
          channels_remaining = channelsPerOutput;
          while ((e131universe - 1) % universesPerOutput != 0) e131universe++;  // advance to next output boundary
          processedOutputs++;
          if (processedOutputs >= nrOfOutputsPerIP) {
            if (actualIPIndex + 1 < nrOfIPAddresses) actualIPIndex++;
            processedOutputs = 0;
            e131universe = 1;
            ip[3] = ipAddresses[actualIPIndex];
          }
        }
      }
    }

    if (e131packetSize > 0) sendE131Packet(ip, e131universe, seqNum, e131packetSize);
  }

  // -----------------------------------------------------------------------
  // Main loop — selects protocol
  // -----------------------------------------------------------------------

  void loop() override {
    DriverNode::loop();  // populates LUT tables when needed

    if (!networkIsConnected()) return;

    unsigned long currentTime = millis();
    if (currentTime - lastSendTime < (unsigned long)(1000 / FPSLimiter)) return;
    lastSendTime = currentTime;

    if (nrOfIPAddresses == 0 && !(protocol == 0 && broadcast)) return;

    LightsHeader* header = &layerP.lights.header;
    if (!header->nrOfLights || !layerP.lights.channelsD) return;

    // If global brightness is 0, send one final black frame then stop.
    if (header->brightness == 0) {
      if (blackFrameSent) return;
      blackFrameSent = true;
      // fall through — sends one black frame so fixtures actually turn off
    } else {
      blackFrameSent = false;
    }

    if (protocol == 1) {
      loopDDP(header);
    } else if (protocol == 2) {
      loopE131(header);
    } else {
      loopArtNet(header);
      sendArtSync();
    }
  }
};

#endif
