/**
    @title     MoonLight
    @file      ArtnetOut.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include <AsyncUDP.h>
  #include <WiFi.h>

class ArtNetOutDriver : public DriverNode {
 public:
  static const char* name() { return "Art-Net Out"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  Char<32> controllerIP3s = "11";
  uint16_t port = 6454;               // Art-Net default port
  uint8_t FPSLimiter = 50;            // default 50 FPS
  uint16_t universeSize = 512;        // default 512 bytes
  uint8_t nrOfOutputsPerIP = 1;       // max 12 on Art-Net LED Controller
  uint8_t universesPerOutput = 1;     // 8 on on Art-Net LED Controller { 0,7,14,21,28,35,42,49 }
  uint16_t channelsPerOutput = 1024;  // 3096 (1024x3) on Art-Net LED Controller {1024,1024,1024,1024,1024,1024,1024,1024};

  uint16_t usedChannelsPerUniverse = 510;  // calculated
  uint16_t totalUniverses = 0;             // calculated
  uint32_t totalChannels = 0;              // calculated

  void setup() override {
    DriverNode::setup();

    addControl(controllerIP3s, "controllerIPs", "text", 0, 32);
    addControl(port, "port", "number", 0, 65538);
    addControl(FPSLimiter, "Limiter", "number", 1, 255, false, "FPS");
    addControl(universeSize, "universeSize", "number", 0, 1000);
    addControl(usedChannelsPerUniverse, "usedChannels", "number", 1, 1000, true);  // calculated
    addControl(nrOfOutputsPerIP, "#Outputs per IP", "number", 0, 255);
    addControl(universesPerOutput, "universesPerOutput", "number", 0, 255);
    addControl(totalUniverses, "totalUniverses", "number", 0, 65538, true);
    addControl(channelsPerOutput, "channelsPerOutput", "number", 0, 65538);
    addControl(totalChannels, "totalChannels", "number", 0, 390000, true);

    // set Art-Net header
    memcpy(packet_buffer,
           (uint8_t[]){
               'A', 'r', 't', '-', 'N', 'e', 't', 0x00, 0x00, 0x50,  // OpCode ArtDMX (little endian)
               0x00, 0x0e,                                           // ProtVer 14
               0x00,                                                 // Sequence (filled later)
               0x00,                                                 // The physical input port from which DMX512 data was input
               0x00, 0x00,                                           // Universe (filled later)
               0x00, 0x00                                            // Length (filled later)
           },
           18);
  };

  uint8_t ipAddresses[16];  // max 16
  uint8_t nrOfIPAddresses = 0;

  void onUpdate(const Char<20>& oldValue, const JsonObject& control) override {
    DriverNode::onUpdate(oldValue, control);  // !!

    if (control["name"] == "controllerIPs") {
      EXT_LOGD(MB_TAG, "IPs: %s", controllerIP3s.c_str());
      nrOfIPAddresses = 0;
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

    totalChannels = layerP.lights.header.nrOfLights * layerP.lights.header.channelsPerLight;
    usedChannelsPerUniverse = universeSize / layerP.lights.header.channelsPerLight * layerP.lights.header.channelsPerLight;  // calculated
    totalUniverses = (totalChannels + usedChannelsPerUniverse - 1) / usedChannelsPerUniverse;                                // ceiling //calculated

    // Update the JSON control values so UI sees the changes
    updateControl("usedChannels", usedChannelsPerUniverse);
    updateControl("totalUniverses", totalUniverses);  // See note below about naming!
    updateControl("totalChannels", totalChannels);

    moduleNodes->requestUIUpdate = true;  // update the calculated values in the UI

    EXT_LOGD(ML_TAG, "c/u:%d #u:%d #c%d (%d)", usedChannelsPerUniverse, totalUniverses, totalChannels, totalUniverses * usedChannelsPerUniverse);
  };

  // loop variables:
  IPAddress controllerIP;  // tbd: controllerIP also configurable from fixtures and Art-Net instead of pin output
  unsigned long wait;
  uint8_t packet_buffer[1024];  // big enough for normal use
  uint_fast16_t packetSize;
  size_t sequenceNumber = 0;  // this needs to be shared across all outputs
  AsyncUDP artnetudp;         // AsyncUDP so we can just blast packets.

  uint_fast16_t universe = 0;
  uint_fast16_t channels_remaining;

  bool writePackage() {
    // for (int i=0; i< 18+packetSize;i++) Serial.printf(" %d", packet_buffer[i]);Serial.println();
    // set the parts of the Art-Net packet header that change:
    packet_buffer[14] = universe;         // The low byte of the 15 bit Port-Address to which this packet is destined
    packet_buffer[15] = universe >> 8;    // The top 7 bits of the 15 bit Port-Address to which this packet is destined
    packet_buffer[16] = packetSize >> 8;  // The length of the DMX512 data array. High Byte
    packet_buffer[17] = packetSize;       // Low Byte of above

    if (!artnetudp.writeTo(packet_buffer, MIN(packetSize, universeSize) + 18, controllerIP, port)) {
      // Serial.print("🐛");
      return false;  // borked //no connection...
    }
    // else Serial.printf(" %d", packetSize);

    packetSize = 0;
    universe++;  // each packet is one universe
    return true;
  }

  uint8_t processedOutputs = 0;
  TickType_t xLastWakeTime = xTaskGetTickCount();

  void loop() override {
    DriverNode::loop();

    LightsHeader* header = &layerP.lights.header;

    if (nrOfIPAddresses == 0) return;  // don't sent if no IP addresses found (to do broadcast if no addresses specified...!)

    // continue with Art-Net code
    uint8_t actualIPIndex = 0;
    controllerIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
    controllerIP[3] = ipAddresses[actualIPIndex];

    if (!controllerIP) return;  // if no connection

    // only need to set once per frame
    packet_buffer[12] = (sequenceNumber++ % 254) + 1;  // The sequence number is used to ensure that ArtDmx packets are used in the correct order, ranging from 1..255

    universe = 0;
    packetSize = 0;
    channels_remaining = channelsPerOutput;
    processedOutputs = 0;

    // send all the lights to artnet, light by light
    for (int indexP = 0; indexP < header->nrOfLights; indexP++) {
      // fill a package
      uint8_t* p = &packet_buffer[packetSize + 18];
      uint8_t* c = &layerP.lights.channelsD[indexP * header->channelsPerLight];

      memcpy(p, c, header->channelsPerLight);  // set all the channels, so the non rgb channels are also filled

      // correct the RGB channels for color order and brightness
      reOrderAndDimRGBW(p + header->offsetRGB, c + header->offsetRGB);

      if (header->offsetRGB1 != UINT8_MAX) {
        reOrderAndDimRGBW(p + header->offsetRGB1, c + header->offsetRGB1);
        // nested as offsetRGB2 only exists if offsetRGB1 etc (for speed)
        if (header->offsetRGB2 != UINT8_MAX) {
          reOrderAndDimRGBW(p + header->offsetRGB2, c + header->offsetRGB2);
          if (header->offsetRGB3 != UINT8_MAX) {
            reOrderAndDimRGBW(p + header->offsetRGB3, c + header->offsetRGB3);
          }
        }
      }

      if (header->lightPreset == 9 && indexP < 72)  // RGBWYP this config assumes a mix of 4 channels and 6 channels per light !!!!
        packetSize += 4;
      else
        packetSize += header->channelsPerLight;

      channels_remaining -= header->channelsPerLight;

      // if packet_buffer full, or output full, send the buffer
      if (packetSize + header->channelsPerLight > universeSize || channels_remaining < header->channelsPerLight) {  // next light will not fit in the package, so send what we got
        // Serial.printf("; %d %d %d", header->nrOfLights, packetSize+18, header->nrOfLights * header->channelsPerLight);

        if (!writePackage()) return;  // resets packagesize

        addYield(10);

        if (channels_remaining < header->channelsPerLight) {  // jump to next output
          channels_remaining = channelsPerOutput;             // reset for a new output

          while (universe % universesPerOutput != 0) universe++;  // advance to next port
          processedOutputs++;
          if (processedOutputs >= nrOfOutputsPerIP) {
            if (actualIPIndex + 1 < nrOfIPAddresses) actualIPIndex++;  // advance to the next IP, if exists
            processedOutputs = 0;                                      // processedOutputs per IP
            universe = 0;
            controllerIP[3] = ipAddresses[actualIPIndex];  // assign the new IP address
          }
        }
      }
    }

    // send the last partially filled package
    if (packetSize > 0) {
      // EXT_LOGD(ML_TAG, ", %d %d %d", header->nrOfLights, packetSize + 18, header->nrOfLights * header->channelsPerLight);

      writePackage();  // remaining
    }
    // EXT_LOGD(ML_TAG, "Universes send %d %d", universe, packages);

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000 / FPSLimiter));  // Yields AND feeds watchdog
  }  // loop
};

#endif