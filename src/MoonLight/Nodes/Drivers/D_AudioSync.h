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
      // if (audio.bands[0] > 0) {
      //   EXT_LOGV(ML_TAG, "Audio Sync: %d %f", audio.bands[0], audio.volume);
      // }
    }
  }
};

#endif