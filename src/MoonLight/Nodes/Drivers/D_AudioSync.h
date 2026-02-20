/**
    @title     MoonLight
    @file      AudioSync.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include <WLED-sync.h>  // https://github.com/netmindz/WLED-sync
  #include <WiFi.h>

class AudioSyncDriver : public Node {
 public:
  static const char* name() { return "Audio Sync"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️♫"; }

  WLEDSync sync;
  bool init = false;
  static constexpr uint8_t audioPaletteIndex = 18;  // see palettes.h

  void loop() override {
    if (!WiFi.isConnected() && !ETH.connected()) {
      // make WLED Audio Sync network failure resilient - WIP
      if (init) {
        // set all data to 0
        memset(sharedData.bands, 0, sizeof(sharedData.bands));
        sharedData.volume = 0;
        sharedData.volumeRaw = 0;
        sharedData.majorPeak = 0;
        init = false;
        EXT_LOGI(ML_TAG, "Audio Sync: stopped");
      }
      return;
    }

    if (!init) {
      sync.begin();
      init = true;
      EXT_LOGI(ML_TAG, "Audio Sync: Initialized");
    }

    if (sync.read()) {
      memcpy(sharedData.bands, sync.fftResult, sizeof(sharedData.bands));
      sharedData.volume = sync.volumeSmth;
      sharedData.volumeRaw = sync.volumeRaw;
      sharedData.majorPeak = sync.FFT_MajorPeak;

      moduleControl->read(
          [&](const ModuleState& state) {
            uint8_t palette = state.data["palette"];
            if (palette >= audioPaletteIndex && palette <= audioPaletteIndex + 2) {  // Audio palettes
              layerP.palette.loadDynamicGradientPalette(getAudioPalette(palette - audioPaletteIndex));
            }
          },
          name());

      // if (audio.bands[0] > 0) {
      //   EXT_LOGV(ML_TAG, "Audio Sync: %d %f", audio.bands[0], audio.volume);
      // }
    }
  }

  // WLEDMM netmindz ar palette
  CRGB getCRGBForBand(int x, uint8_t* fftResult, int pal) {
    CRGB value;
    CHSV hsv;
    if (pal == 0) {  // bit hacky to use palette id here, but don't want to litter the code with lots of different methods. TODO: add enum for palette creation type
      if (x == 1) {
        value = CRGB(fftResult[10] / 2, fftResult[4] / 2, fftResult[0] / 2);
      } else if (x == 255) {
        value = CRGB(fftResult[10] / 2, fftResult[0] / 2, fftResult[4] / 2);
      } else {
        value = CRGB(fftResult[0] / 2, fftResult[4] / 2, fftResult[10] / 2);
      }
    } else if (pal == 1) {
      int b = map(x, 1, 255, 0, 10);                                      // convert palette position to lower half of freq band
      hsv = CHSV(fftResult[b], 255, map(fftResult[b], 0, 255, 30, 255));  // pick hue
      hsv2rgb_rainbow(hsv, value);                                        // convert to R,G,B
    } else if (pal == 2) {
      int b = map(x, 0, 255, 0, 8);  // convert palette position to lower half of freq band
      hsv = CHSV(uint8_t(fftResult[b]), 255, x);
      hsv2rgb_rainbow(hsv, value);  // convert to R,G,B
    }

    return value;
  }

  // WLEDMM netmindz ar palette
  uint8_t* getAudioPalette(int pal) {
    // https://forum.makerforums.info/t/hi-is-it-possible-to-define-a-gradient-palette-at-runtime-the-define-gradient-palette-uses-the/63339

    static uint8_t xyz[16];  // Needs to be 4 times however many colors are being used.
                             // 3 colors = 12, 4 colors = 16, etc.

    xyz[0] = 0;  // anchor of first color - must be zero
    xyz[1] = 0;
    xyz[2] = 0;
    xyz[3] = 0;

    CRGB rgb = getCRGBForBand(1, sharedData.bands, pal);
    xyz[4] = 1;  // anchor of first color
    xyz[5] = rgb.r;
    xyz[6] = rgb.g;
    xyz[7] = rgb.b;

    rgb = getCRGBForBand(128, sharedData.bands, pal);
    xyz[8] = 128;
    xyz[9] = rgb.r;
    xyz[10] = rgb.g;
    xyz[11] = rgb.b;

    rgb = getCRGBForBand(255, sharedData.bands, pal);
    xyz[12] = 255;  // anchor of last color - must be 255
    xyz[13] = rgb.r;
    xyz[14] = rgb.g;
    xyz[15] = rgb.b;

    return xyz;
  }
};

#endif