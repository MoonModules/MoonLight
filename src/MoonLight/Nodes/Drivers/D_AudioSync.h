/**
    @title     MoonLight
    @file      AudioSync.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

  #include <WLED-sync.h>  // https://github.com/netmindz/WLED-sync
  #include <WiFi.h>

class AudioSyncDriver : public Node {
 public:
  static const char* name() { return "AudioSync"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️♫"; }

  WLEDSync sync;
  bool init = false;

  void loop() override {
    if (!WiFi.localIP()) return;
    if (!init) {
      sync.begin();
      init = true;
      EXT_LOGV(ML_TAG, "AudioSync: Initialized");
    }
    if (sync.read()) {
      memcpy(sharedData.bands, sync.fftResult, NUM_GEQ_CHANNELS);
      sharedData.volume = sync.volumeSmth;
      sharedData.volumeRaw = sync.volumeRaw;
      sharedData.majorPeak = sync.FFT_MajorPeak;
      // if (audio.bands[0] > 0) {
      //   EXT_LOGV(ML_TAG, "AudioSync: %d %f", audio.bands[0], audio.volume);
      // }
    }
  }
};

#endif