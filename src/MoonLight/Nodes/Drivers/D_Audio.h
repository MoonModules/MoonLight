/**
    @title     MoonLight
    @file      Audio.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2025 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONLIGHT

class AudioDriver : public Node {
 public:
  static const char* name() { return "Audio"; }
  static uint8_t dim() { return _NoD; }
  static const char* tags() { return "☸️♫"; }

  uint8_t inputType = 1;
  uint8_t squelch = 10;
  uint8_t gain = 60;
  uint8_t agc = 1;
  uint8_t fftWindow = 0;
  String statusMessage = "Okay";

  void setup() override {
    JsonObject property;
    JsonArray values;

    property = addControl(inputType, "inputType", "select");
    values = property["values"].to<JsonArray>();
    values.add("Generic I2S 🎤");
    values.add("Generic I2S 🎤with Mclk");
    values.add("Line-in 🔌 ES7243");
    values.add("...");
    // the type determines the pins see onUpdate

    addControl(squelch, "squelch", "slider");
    addControl(gain, "gain", "slider");

    property = addControl(agc, "Automatic Gain Control", "select");
    values = property["values"].to<JsonArray>();
    values.add("Off");
    values.add("Normal");
    values.add("Vivid");
    values.add("Lazy");

    property = addControl(fftWindow, "FFT Window", "select");
    values = property["values"].to<JsonArray>();
    values.add("Blackman-Harris (MM standard)");
    values.add("Hann (balanced)");
    values.add("Nuttall (more accurate)");
    values.add("Blackman");
    values.add("Hamming");
    values.add("Flat-Top (AC WLED, inaccurate)");

    addControl(statusMessage, "status", "text", 0, 64, true); //read only

    // create / allocate any resources the audio driver needs including creating the FFT task
    //  init the audio driver with necessary parameters
  };

  uint8_t pins[8];
  bool success = true;

  void onUpdate(String& oldValue, JsonObject control) override {
    if (control["name"] == "inputType") {  // set pins and other stuff to use mic, line in ...
      switch (control["value"].as<uint8_t>()) {
      case 0:                 // Generic I2S 🎤
        pins[0] = UINT8_MAX;  // get mic pin clk from the io module, if not exist, make the audio init fail
        pins[1] = UINT8_MAX;  // get mic pin sda from the io module, if not exist, make the audio init fail
        pins[2] = UINT8_MAX;  // get mic pin scl from the io module, if not exist, make the audio init fail
        //...
        if (pins[0] == UINT8_MAX || pins[1] == UINT8_MAX || pins[2] == UINT8_MAX) {
          success = false;
          statusMessage = "Pins not found";
        }
        break;
      }
    }
    if (success)
      statusMessage = "okay";
    // update the audio driver with pins etc...
  }

  void loop() override {
    if (!success) return;

    // call the audio driver sample function and return frequency bands and volume and ...

    // set MoonLight shared data
    sharedData.bands;  // memcpy(sharedData.bands, sync.fftResult, NUM_GEQ_CHANNELS);
    sharedData.volume; // = sync.volumeSmth;
    sharedData.volumeRaw; // = sync.volumeRaw;
    sharedData.majorPeak; // = sync.FFT_MajorPeak;
  }

  ~AudioDriver() override {
    // free any resources claimed by Audio, including FFT task
  };
};

#endif