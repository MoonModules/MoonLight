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

  #include <WiFi.h>

  #define ARTNET_CHANNELS_PER_PACKET 512
static const uint8_t ART_NET_HEADER[] = {0x41, 0x72, 0x74, 0x2d, 0x4e, 0x65, 0x74, 0x00, 0x00, 0x50, 0x00, 0x0e};
  // 0..7: Array of 8 characters, the final character is a null termination. Value = 'A' 'r' 't' '-' 'N' 'e' 't' 0x00
  // 8-9: OpOutput Transmitted low byte first (so 0x50, 0x00)
  // 10: High byte of the Art-Net protocol revision number
  // 11: Low byte of the Art-Net protocol revision number. Current value 14
  #include <AsyncUDP.h>

class ArtNetOutDriver : public DriverNode {
 public:
  static const char* name() { return "Art-Net Out"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️"; }

  Char<32> controllerIP3s = "11";
  uint16_t port = 6454;               // Art-Net default port
  uint8_t FPSLimiter = 50;            // default 50 FPS
  uint8_t universesPerOutput = 1;     // 7 on on Art-Net LED Controller { 0,7,14,21,28,35,42,49 }
  uint16_t channelsPerOutput = 1024;  // 3096 (1024x3) on Art-Net LED Controller {1024,1024,1024,1024,1024,1024,1024,1024};
  uint8_t nrOfOutputs = 1;            // max 12 on Art-Net LED Controller

  void setup() override {
    DriverNode::setup();

    addControl(controllerIP3s, "controllerIPs", "text", 0, 32);
    addControl(port, "port", "number", 0, 65538);
    addControl(FPSLimiter, "Limiter", "number", 0, 255, false, "FPS");
    addControl(universesPerOutput, "universesPerOutput", "number", 0, 255);
    addControl(channelsPerOutput, "channelsPerOutput", "number", 0, 65538);
    addControl(nrOfOutputs, "#Outputs per IP", "number", 0, 255);

    memcpy(packet_buffer, ART_NET_HEADER, sizeof(ART_NET_HEADER));  // copy in the Art-Net header.
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
  };

  // loop variables:
  IPAddress controllerIP;  // tbd: controllerIP also configurable from fixtures and Art-Net instead of pin output
  unsigned long lastMillis = millis();
  unsigned long wait;
  uint8_t packet_buffer[sizeof(ART_NET_HEADER) + 6 + ARTNET_CHANNELS_PER_PACKET];
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

    if (!artnetudp.writeTo(packet_buffer, MIN(packetSize, 512) + 18, controllerIP, port)) {
      // Serial.print("🐛");
      return false;  // borked //no connection...
    }
    // else Serial.printf(" %d", packetSize);

    packetSize = 0;
    universe++;  // each packet is one universe
    return true;
  }

  uint8_t processedOutputs = 0;

  void loop() override {
    DriverNode::loop();

    LightsHeader* header = &layerP.lights.header;

    if (header->isPositions != 0 || nrOfIPAddresses == 0) return;  // don't sent if positions are sent or no IP addresses found (to do broadcast if no addresses specified...!)

    // continue with Art-Net code
    uint8_t actualIPIndex = 0;
    controllerIP = WiFi.isConnected() ? WiFi.localIP() : ETH.localIP();
    controllerIP[3] = ipAddresses[actualIPIndex];

    if (!controllerIP) return;  // if no connection

    // wait until the throttle FPS is reached
    // wait needed to avoid misalignment between packages sent and displayed - 🚧
    wait = 1000 / FPSLimiter + lastMillis - millis();
    if (wait > 0 && wait < 1000 / FPSLimiter) delay(wait);  // delay in ms
    // else
    //   EXT_LOGW(ML_TAG, "wait %d", wait);
    // if (millis() - lastMillis < 1000 / FPSLimiter)
    //   delay(1000 / FPSLimiter + lastMillis - millis()); // delay in ms
    //   return;
    lastMillis = millis();

    // only need to set once per frame
    packet_buffer[12] = (sequenceNumber++ % 254) + 1;  // The sequence number is used to ensure that ArtDmx packets are used in the correct order, ranging from 1..255
    packet_buffer[13] = 0;                             // The physical input port from which DMX512 data was input

    universe = 0;
    packetSize = 0;
    channels_remaining = channelsPerOutput;

    // send all the leds to artnet
    for (int indexP = 0; indexP < header->nrOfLights; indexP++) {
      // fill a package
      memcpy(&packet_buffer[packetSize + 18], &layerP.lights.channelsD[indexP * header->channelsPerLight], header->channelsPerLight);  // set all the channels

      // correct the RGB channels for color order and brightness
      reOrderAndDimRGBW(&packet_buffer[packetSize + 18 + header->offsetRGB], &layerP.lights.channelsD[indexP * header->channelsPerLight + header->offsetRGB]);

      if (header->offsetRGB1 != UINT8_MAX) reOrderAndDimRGBW(&packet_buffer[packetSize + 18 + header->offsetRGB1], &layerP.lights.channelsD[indexP * header->channelsPerLight + header->offsetRGB1]);
      if (header->offsetRGB2 != UINT8_MAX) reOrderAndDimRGBW(&packet_buffer[packetSize + 18 + header->offsetRGB2], &layerP.lights.channelsD[indexP * header->channelsPerLight + header->offsetRGB2]);
      if (header->offsetRGB3 != UINT8_MAX) reOrderAndDimRGBW(&packet_buffer[packetSize + 18 + header->offsetRGB3], &layerP.lights.channelsD[indexP * header->channelsPerLight + header->offsetRGB3]);

      if (header->lightPreset == 9 && indexP < 72)  // RGBWYP this config assumes a mix of 4 channels and 6 channels per light !!!!
        packetSize += 4;
      else
        packetSize += header->channelsPerLight;

      channels_remaining -= header->channelsPerLight;

      // if packet_buffer full, or output full, send the buffer
      if (packetSize + header->channelsPerLight > ARTNET_CHANNELS_PER_PACKET || channels_remaining < header->channelsPerLight) {  // next light will not fit in the package, so send what we got
        // Serial.printf("; %d %d %d", header->nrOfLights, packetSize+18, header->nrOfLights * header->channelsPerLight);

        if (!writePackage()) return;  // resets packagesize

        if (channels_remaining < header->channelsPerLight) {  // jump to next output
          channels_remaining = channelsPerOutput;             // reset for a new output

          while (universe % universesPerOutput != 0) universe++;  // advance to next port
          processedOutputs++;
          if (processedOutputs >= nrOfOutputs) {
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
      // Serial.printf(", %d %d %d", header->nrOfLights, packetSize+18, header->nrOfLights * header->channelsPerLight);

      writePackage();  // remaining
    }
  }
};

#endif