/**
    @title     MoonLight
    @file      Mods.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#pragma once

#if FT_MOONLIGHT

#include <WLED-sync.h> // https://github.com/netmindz/WLED-sync
#include <WiFi.h>

//alphabetically from here

#define MAX_FREQUENCY   11025          // sample frequency / 2 (as per Nyquist criterion)

//data shared between nodes
static struct Audio {
  uint8_t bands[16]= {0}; // Our calculated freq. channel result table to be used by effects
  float volume; // either sampleAvg or sampleAgc depending on soundAgc; smoothed sample
  float majorPeak; // FFT: strongest (peak) frequency
} audio;


#include "Nodes.h" //needed here because of Mods.cpp includes Mods.h, otherwise Node unknown

class DriverNode: public Node {
  uint8_t colorOrderSaved = UINT8_MAX;
  uint8_t colorOrder = 3;
  uint16_t maxPowerSaved = UINT8_MAX;
  uint16_t maxPower = 10;
  uint8_t correctedBrightness = UINT8_MAX;

  public:

  void setup() override;

  void loop() override;

  void reOrderAndDimRGB(uint8_t *packetRGBChannel, uint8_t *lightsRGBChannel);
};

class ArtNetDriverMod: public DriverNode {
  public:

  static const char * name() {return "Art-Net Driver ☸️";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint16_t controllerIP3 = 11;
  uint16_t FPSLimiter = 50; //default 50ms
  uint16_t nrOfOutputs = 2; //8 on Art-Net LED Controller
  uint16_t channelsPerOutput = 512; //3096 (1024x3) on Art-Net LED Controller {1024,1024,1024,1024,1024,1024,1024,1024};
  uint16_t universesPerOutput = 1; //7 on on Art-Net LED Controller { 0,7,14,21,28,35,42,49 }

  void setup() override;

  const size_t ART_NET_HEADER_SIZE = 12;
  const uint8_t ART_NET_HEADER[12] = {0x41,0x72,0x74,0x2d,0x4e,0x65,0x74,0x00,0x00,0x50,0x00,0x0e};

  IPAddress controllerIP; //tbd: controllerIP also configurable from fixtures and Art-Net instead of pin output
  size_t sequenceNumber = 0;

  unsigned long lastMillis = millis();

  void loop() override;
};

class AudioSyncMod: public Node {
  public:

  static const char * name() {return "AudioSync ☸️ ♫";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  WLEDSync sync;
  bool init = false;

  void loop() override {
    if (!WiFi.localIP()) return;
    if (!init) {
      sync.begin();
      init = true;
      ESP_LOGD(TAG, "AudioSync: Initialized");
    }
    if (sync.read()) {
      memcpy(audio.bands, sync.fftResult, NUM_GEQ_CHANNELS);
      audio.volume = sync.volumeSmth;
      audio.majorPeak = sync.FFT_MajorPeak;
      // if (audio.bands[0] > 0) {
      //   ESP_LOGD(TAG, "AudioSync: %d %f", audio.bands[0], audio.volume);
      // }
    }
  }
};

static AudioSyncMod *audioNode;


class FastLEDDriverMod: public Node {
  public:

  static const char * name() {return "FastLED Driver ☸️";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  uint16_t maxPowerSaved = 0;
  uint16_t maxPower = 10;

  void setup() override;
  void loop() override;

  void addLayout() override;
};

class HUB75DriverMod: public Node {
  public:

  static const char * name() {return "HUB75 Driver ☸️";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  void setup() override;
  void loop() override;

  void addLayout() override;
};

class PhysicalDriverMod: public DriverNode {
  public:

  static const char * name() {return "Physical Driver ☸️";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  void setup() override;
  void loop() override;

  void addLayout() override;
};

class VirtualDriverMod: public DriverNode {
  public:

  static const char * name() {return "Virtual Driver ☸️";}
  static uint8_t dim() {return _3D;}
  static const char * tags() {return "";}

  void setup() override;
  void loop() override;

  void addLayout() override;
};

#endif